#include "statschannel.h"

namespace {

// 远端循环：每 3 秒在两份 /proc 文件外打标记。自包含、无 stdin、
// 不碰终端 shell。
const char kRemoteCommand[] =
    "while true; do echo __XTERM_STATS_B__; "
    "cat /proc/stat /proc/meminfo 2>/dev/null; "
    "echo __XTERM_STATS_E__; sleep 3; done";

// 一次 /proc 帧几 KiB；远端失控刷屏而没有完整帧时封顶防内存膨胀。
constexpr int kMaxBufferBytes = 64 * 1024;
constexpr int kReadChunkBytes = 4096;

// 没有完整帧时，保留 buffer 末尾可能是 kFrameBegin 前缀的部分
// （分片到达的标记要能拼回来），其余全部丢弃。
void keepBeginPrefix(QByteArray *buffer)
{
    const int size = buffer->size();
    const int maxLen = qMin(size, StatsChannel::kFrameBegin.size() - 1);
    for (int len = maxLen; len > 0; --len) {
        if (buffer->mid(size - len)
            == StatsChannel::kFrameBegin.left(len)) {
            buffer->remove(0, size - len);
            return;
        }
    }
    buffer->clear();
}

} // namespace

const QByteArray StatsChannel::kFrameBegin("__XTERM_STATS_B__");
const QByteArray StatsChannel::kFrameEnd("__XTERM_STATS_E__");

StatsChannel::StatsChannel(LibsshApi &api, ssh_session session)
    : m_api(api), m_session(session)
{
}

StatsChannel::~StatsChannel()
{
    stop();
}

void StatsChannel::start()
{
    stop();
    m_active = true;
    m_step = Step::OpenChannel;
    m_degraded = false;
    resetRetry();
    m_clock.restart();
    m_retryAtMs = 0;
}

void StatsChannel::stop()
{
    releaseChannel(true);
    m_buffer.clear();
    m_active = false;
    m_step = Step::Idle;
    m_degraded = false;
    resetRetry();
}

void StatsChannel::skipRetryDelayForTest()
{
    m_retryAtMs = 0;
}

StatsChannel::Result StatsChannel::advance(QList<QString> *samples,
                                           bool *degraded, bool *recovered)
{
    if (degraded != nullptr) {
        *degraded = false;
    }
    if (recovered != nullptr) {
        *recovered = false;
    }
    if (!m_active) {
        return Result::Again;
    }

    switch (m_step) {
    case Step::Idle:
        return Result::Again;
    case Step::Backoff:
        if (m_clock.elapsed() < m_retryAtMs) {
            return Result::Again;
        }
        m_step = Step::OpenChannel;
        [[fallthrough]];
    case Step::OpenChannel: {
        if (m_channel == nullptr) {
            m_channel = m_api.newChannel(m_session);
            m_buffer.clear();
        }
        const int rc = m_api.openChannelSession(m_channel);
        if (rc == SSH_AGAIN) {
            return Result::Progress;
        }
        if (rc != SSH_OK) {
            failChannel(degraded);   // 通道没开成，只需释放
            return Result::Progress;
        }
        m_step = Step::RequestExec;
        return Result::Progress;
    }
    case Step::RequestExec: {
        const int rc =
            m_api.requestExec(m_channel, QByteArray(kRemoteCommand));
        if (rc == SSH_AGAIN) {
            return Result::Progress;
        }
        if (rc != SSH_OK) {
            failChannel(degraded);
            return Result::Progress;
        }
        m_step = Step::Streaming;
        return Result::Progress;
    }
    case Step::Streaming: {
        if (m_api.channelIsEof(m_channel)) {
            // 远端循环退出（板子重启/断流）：关通道进退避。
            failChannel(degraded);
            return Result::Progress;
        }
        char chunk[kReadChunkBytes];
        const int rc = m_api.readChannelNonblocking(
            m_channel, chunk, sizeof(chunk), false);
        if (rc == SSH_EOF) {
            failChannel(degraded);
            return Result::Progress;
        }
        if (rc != SSH_AGAIN && rc != 0) {
            if (rc < 0) {
                failChannel(degraded);
                return Result::Progress;
            }
            m_buffer.append(chunk, rc);
        }
        if (m_buffer.size() > kMaxBufferBytes) {
            failChannel(degraded);   // 只有垃圾没有帧，重来
            return Result::Progress;
        }
        if (extractFrames(&m_buffer, samples) > 0) {
            resetRetry();
            if (m_degraded) {
                m_degraded = false;
                if (recovered != nullptr) {
                    *recovered = true;
                }
            }
        }
        return Result::Progress;
    }
    }
    return Result::Again;
}

int StatsChannel::extractFrames(QByteArray *buffer, QList<QString> *samples)
{
    int count = 0;
    bool pendingBegin = false;
    for (;;) {
        const int begin = buffer->indexOf(kFrameBegin);
        if (begin < 0) {
            break;
        }
        const int contentStart = begin + int(kFrameBegin.size());
        const int end = buffer->indexOf(kFrameEnd, contentStart);
        if (end < 0) {
            // 帧没收全：丢掉标记之前的内容，等待后续分片。
            if (begin > 0) {
                buffer->remove(0, begin);
            }
            pendingBegin = true;
            break;
        }
        samples->append(QString::fromUtf8(
            buffer->mid(contentStart, end - contentStart).trimmed()));
        ++count;
        buffer->remove(0, end + int(kFrameEnd.size()));
    }
    if (!pendingBegin) {
        keepBeginPrefix(buffer);
    }
    return count;
}

void StatsChannel::failChannel(bool *degraded)
{
    // 打开失败时通道未建立，close 无从谈起；其余路径先 close 再 free。
    releaseChannel(m_step != Step::OpenChannel);
    m_step = Step::Backoff;
    m_retryDelayMs = qMin(m_retryDelayMs * 2, MaxRetryDelayMs);
    m_retryAtMs = m_clock.elapsed() + m_retryDelayMs;
    m_degraded = true;
    if (degraded != nullptr) {
        *degraded = true;
    }
}

void StatsChannel::releaseChannel(bool closeFirst)
{
    if (m_channel == nullptr) {
        return;
    }
    if (closeFirst) {
        m_api.closeChannel(m_channel);   // 结果忽略：尽力送达即可
    }
    m_api.freeChannel(m_channel);
    m_channel = nullptr;
}

void StatsChannel::resetRetry()
{
    m_retryDelayMs = InitialRetryDelayMs / 2;
}
