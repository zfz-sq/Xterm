#include "sftpchannel.h"

#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <fcntl.h>

namespace {

SftpEntry entryFromAttributes(sftp_attributes attrs)
{
    SftpEntry entry;
    if (attrs == nullptr) {
        return entry;
    }
    entry.name = QString::fromUtf8(attrs->name == nullptr ? "" : attrs->name);
    entry.size = attrs->size;
    entry.mtime = qint64(attrs->mtime);
    entry.permissions = attrs->permissions;
    entry.isDir = attrs->type == SSH_FILEXFER_TYPE_DIRECTORY
        || (attrs->permissions & SSH_S_IFDIR) != 0;
    entry.isSymlink = attrs->type == SSH_FILEXFER_TYPE_SYMLINK
        || (attrs->permissions & SSH_S_IFLNK) != 0;
    return entry;
}

// libssh 的经典 sftp 客户端调用内部用 ssh_channel_read 同步收响应；
// 非阻塞会话下无数据时返回的 SSH_AGAIN(2) 会被当作已读字节数累加，
// 协议立即错位（真机上 realpath 等必失败）。因此这些同步调用期间
// 临时切回阻塞（受 SSH_OPTIONS_TIMEOUT 约束），离开作用域恢复非阻塞。
// begin 提交侧不套守卫，AIO 流水线的提交仍是非阻塞的。
// 守卫会嵌套（failActive 里的 closeRemoteFiles/releaseSftp 也带守卫）：
// 用深度计数保证整个窗口只有最外层那一对 blocking 开关，内层不提前
// 恢复非阻塞。
class ScopedSyncSftp
{
public:
    ScopedSyncSftp(LibsshApi &api, ssh_session session)
        : m_api(api), m_session(session)
    {
        if (s_depth++ == 0) {
            m_api.setBlocking(m_session, true);
        }
    }
    ~ScopedSyncSftp()
    {
        if (--s_depth == 0) {
            m_api.setBlocking(m_session, false);
        }
    }

    ScopedSyncSftp(const ScopedSyncSftp &) = delete;
    ScopedSyncSftp &operator=(const ScopedSyncSftp &) = delete;

private:
    static thread_local int s_depth;

    LibsshApi &m_api;
    ssh_session m_session;
};

thread_local int ScopedSyncSftp::s_depth = 0;

} // namespace

SftpChannel::SftpChannel(LibsshApi &api, ssh_session session)
    : m_api(api),
      m_ownerThread(QThread::currentThread()),
      m_session(session)
{
}

SftpChannel::~SftpChannel()
{
    if (onOwnerThread()) {
        QList<SftpResult> ignored;
        close(&ignored);
    }
}

void SftpChannel::enqueue(const SftpRequest &request)
{
    if (!onOwnerThread()) {
        return;
    }
    m_queue.enqueue(request);
}

void SftpChannel::cancel(SshRequestId requestId)
{
    if (!onOwnerThread()) {
        return;
    }
    if (m_hasActive && m_active.requestId == requestId) {
        m_cancelled = true;
        return;
    }
    QQueue<SftpRequest> remaining;
    while (!m_queue.isEmpty()) {
        const SftpRequest request = m_queue.dequeue();
        if (request.requestId == requestId) {
            m_cancelledQueued.append(request);
        } else {
            remaining.enqueue(request);
        }
    }
    m_queue = remaining;
}

SftpChannel::Result SftpChannel::advance(QList<SftpResult> *results)
{
    if (!onOwnerThread()) {
        return Result::Error;
    }
    if (results == nullptr) {
        return Result::Error;
    }

    m_tickBytes = 0;
    m_tickRequests = 0;

    while (!m_cancelledQueued.isEmpty()) {
        emitCancelled(results, m_cancelledQueued.takeFirst());
    }

    if (!m_hasActive) {
        if (m_queue.isEmpty()) {
            return Result::Idle;
        }
        m_active = m_queue.dequeue();
        m_hasActive = true;
        m_cancelled = false;
        m_phase = Phase::None;
        m_done = 0;
        m_total = 0;
        m_readEof = false;
        m_pending.clear();
        m_execOutput.clear();
        m_entries.clear();
        m_copyMode = 0644;
    }

    if (m_cancelled) {
        abortActive(results, QStringLiteral("已取消"));
        return m_queue.isEmpty() ? Result::Idle : Result::Progress;
    }

    const Result ensured = ensureSession(results);
    if (ensured != Result::Progress) {
        return ensured;
    }
    return advanceActive(results);
}

void SftpChannel::close(QList<SftpResult> *results)
{
    if (!onOwnerThread()) {
        return;
    }
    QList<SftpResult> sink;
    QList<SftpResult> *out = results == nullptr ? &sink : results;
    while (!m_queue.isEmpty()) {
        emitCancelled(out, m_queue.dequeue());
    }
    while (!m_cancelledQueued.isEmpty()) {
        emitCancelled(out, m_cancelledQueued.takeFirst());
    }
    if (m_hasActive && !m_cancelled) {
        abortActive(out, QStringLiteral("已取消"));
    } else {
        cleanupTransfer(true);
        closeRemoteFiles();
        resetActive();
    }
    releaseSftp();
}

SftpChannel::Result SftpChannel::ensureSession(QList<SftpResult> *results)
{
    if (m_sftpReady && m_sftp != nullptr) {
        return Result::Progress;
    }
    switch (m_channelStep) {
    case ChannelStep::Open:
        // 真 libssh 的 sftp_new 要求阻塞会话并同步开通道，非阻塞会话
        // 直接被拒绝。因此通道自己非阻塞地开（与 shell 通道同款），
        // 再用 sftp_new_channel 包装，避免阻塞 worker 线程。
        if (m_sftpChannel == nullptr) {
            m_sftpChannel = m_api.newChannel(m_session);
            if (m_sftpChannel == nullptr) {
                return failActive(results, QStringLiteral("打开 SFTP 通道"));
            }
        }
        if (const int rc = m_api.openChannelSession(m_sftpChannel);
            rc == SSH_AGAIN) {
            return Result::Again;
        } else if (rc != SSH_OK) {
            return failActive(results, QStringLiteral("打开 SFTP 通道"));
        }
        m_channelStep = ChannelStep::Subsystem;
        Q_FALLTHROUGH();
    case ChannelStep::Subsystem: {
        const int rc = m_api.channelRequestSftp(m_sftpChannel);
        if (rc == SSH_AGAIN) {
            return Result::Again;
        }
        if (rc != SSH_OK) {
            return failActive(results, QStringLiteral("请求 sftp 子系统"));
        }
        m_channelStep = ChannelStep::Wrap;
        Q_FALLTHROUGH();
    }
    case ChannelStep::Wrap:
        m_sftp = m_api.sftpNewChannel(m_session, m_sftpChannel);
        if (m_sftp == nullptr) {
            return failActive(results, QStringLiteral("创建 SFTP 会话"));
        }
        m_channelStep = ChannelStep::Init;
        Q_FALLTHROUGH();
    case ChannelStep::Init: {
        // sftp_init 是同步的 INIT/VERSION 握手，非阻塞会话上它会等到
        // 超时。阻塞期间受 SSH_OPTIONS_TIMEOUT 约束。
        const ScopedSyncSftp sync(m_api, m_session);
        const int rc = m_api.sftpInit(m_sftp);
        if (rc != SSH_OK) {
            return failActive(results, QStringLiteral("初始化 SFTP 通道"));
        }
        m_channelStep = ChannelStep::Done;
        break;
    }
    case ChannelStep::Done:
        break;
    }
    m_sftpReady = true;
    return Result::Progress;
}

SftpChannel::Result SftpChannel::advanceActive(QList<SftpResult> *results)
{
    switch (m_active.op) {
    case SftpOp::List:
        return advanceList(results);
    case SftpOp::RealPath:
        return advanceRealPath(results);
    case SftpOp::Mkdir:
    case SftpOp::Delete:
    case SftpOp::Rename:
    case SftpOp::Chmod:
        return advanceSimple(results);
    case SftpOp::Create:
        return advanceCreate(results);
    case SftpOp::Upload:
        return advanceUpload(results);
    case SftpOp::Download:
        return advanceDownload(results);
    case SftpOp::Copy:
        return advanceCopy(results);
    case SftpOp::Exec:
        return advanceExec(results);
    }
    return failActive(results, QStringLiteral("未知 SFTP 操作"));
}

SftpChannel::Result SftpChannel::advanceList(QList<SftpResult> *results)
{
    const ScopedSyncSftp sync(m_api, m_session);
    if (m_phase == Phase::None) {
        m_phase = Phase::ListOpen;
    }
    if (m_phase == Phase::ListOpen) {
        m_dir = m_api.sftpOpendir(m_sftp, m_active.path.toUtf8());
        if (m_dir == nullptr) {
            if (isAgain()) {
                return Result::Again;
            }
            return failActive(results, QStringLiteral("无法打开目录"));
        }
        m_phase = Phase::ListRead;
    }
    if (m_phase == Phase::ListRead) {
        for (;;) {
            sftp_attributes attrs = m_api.sftpReaddir(m_sftp, m_dir);
            if (attrs == nullptr) {
                if (isAgain()) {
                    return Result::Again;
                }
                if (m_api.sftpDirEof(m_dir) == 0) {
                    return failActive(results, QStringLiteral("读取目录失败"));
                }
                m_phase = Phase::ListClose;
                break;
            }
            const SftpEntry entry = entryFromAttributes(attrs);
            m_api.sftpAttributesFree(attrs);
            if (entry.name == QLatin1String(".")
                || entry.name == QLatin1String("..")) {
                continue;
            }
            m_entries.append(entry);
        }
    }
    if (m_phase == Phase::ListClose) {
        const int closed = m_api.sftpClosedir(m_dir);
        m_dir = nullptr;
        if (closed == SSH_AGAIN) {
            return Result::Again;
        }
        if (closed != SSH_OK) {
            return failActive(results, QStringLiteral("关闭目录失败"));
        }
        std::sort(m_entries.begin(), m_entries.end(),
                  [](const SftpEntry &left, const SftpEntry &right) {
                      if (left.isDir != right.isDir) {
                          return left.isDir;
                      }
                      return left.name.compare(right.name, Qt::CaseInsensitive)
                          < 0;
                  });
        completeList(results, true);
        return m_queue.isEmpty() ? Result::Idle : Result::Progress;
    }
    return Result::Progress;
}

SftpChannel::Result SftpChannel::advanceRealPath(QList<SftpResult> *results)
{
    const ScopedSyncSftp sync(m_api, m_session);
    QString resolved;
    const int rc =
        m_api.sftpCanonicalizePath(m_sftp, m_active.path.toUtf8(), &resolved);
    if (rc == SSH_AGAIN || (rc != SSH_OK && isAgain())) {
        return Result::Again;
    }
    if (rc != SSH_OK) {
        return failActive(results, QStringLiteral("无法解析路径"));
    }
    completeOp(results, QStringLiteral("realpath"), resolved, true);
    return m_queue.isEmpty() ? Result::Idle : Result::Progress;
}

SftpChannel::Result SftpChannel::advanceSimple(QList<SftpResult> *results)
{
    const ScopedSyncSftp sync(m_api, m_session);
    int rc = SSH_ERROR;
    QString arg = m_active.path;
    const QByteArray path = m_active.path.toUtf8();
    switch (m_active.op) {
    case SftpOp::Mkdir:
        rc = m_api.sftpMkdir(m_sftp, path, 0755);
        break;
    case SftpOp::Delete:
        rc = m_active.isDir ? m_api.sftpRmdir(m_sftp, path)
                            : m_api.sftpUnlink(m_sftp, path);
        break;
    case SftpOp::Rename:
        rc = m_api.sftpRename(m_sftp, path, m_active.dest.toUtf8());
        break;
    case SftpOp::Chmod:
        rc = m_api.sftpChmod(m_sftp, path, mode_t(m_active.mode));
        break;
    default:
        break;
    }
    if (rc == SSH_AGAIN) {
        return Result::Again;
    }
    if (rc != SSH_OK) {
        return failActive(results, QStringLiteral("SFTP 操作失败"));
    }
    completeOp(results, sftpOpName(m_active.op), arg, true);
    return m_queue.isEmpty() ? Result::Idle : Result::Progress;
}

SftpChannel::Result SftpChannel::advanceCreate(QList<SftpResult> *results)
{
    const ScopedSyncSftp sync(m_api, m_session);
    if (m_phase == Phase::None) {
        m_phase = Phase::CreateOpen;
    }
    if (m_phase == Phase::CreateOpen) {
        m_file = m_api.sftpOpen(m_sftp, m_active.path.toUtf8(),
                                O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (m_file == nullptr) {
            if (isAgain()) {
                return Result::Again;
            }
            return failActive(results, QStringLiteral("创建文件失败"));
        }
        m_phase = Phase::CreateClose;
    }
    const int rc = m_api.sftpCloseFile(m_file);
    if (rc == SSH_AGAIN) {
        return Result::Again;
    }
    m_file = nullptr;
    if (rc != SSH_OK) {
        return failActive(results, QStringLiteral("创建文件失败"));
    }
    completeOp(results, QStringLiteral("create"), m_active.path, true);
    return m_queue.isEmpty() ? Result::Idle : Result::Progress;
}

SftpChannel::Result SftpChannel::advanceUpload(QList<SftpResult> *results)
{
    if (m_phase == Phase::None || m_phase == Phase::TransferPrepare) {
        const ScopedSyncSftp sync(m_api, m_session);
        if (!m_local.isOpen()) {
            m_local.setFileName(m_active.localPath);
            if (!m_local.open(QIODevice::ReadOnly)) {
                completeTransfer(
                    results, false,
                    QStringLiteral("无法打开本地文件 %1").arg(m_active.localPath));
                return m_queue.isEmpty() ? Result::Idle : Result::Progress;
            }
            m_total = m_local.size();
        }
        m_file = m_api.sftpOpen(m_sftp, m_active.remotePath.toUtf8(),
                                O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (m_file == nullptr) {
            if (isAgain()) {
                m_phase = Phase::TransferPrepare;
                return Result::Again;
            }
            return failActive(results, QStringLiteral("无法创建远程文件"));
        }
        m_api.sftpFileSetNonblocking(m_file);
        m_phase = Phase::TransferIo;
    }

    if (m_phase == Phase::TransferIo) {
        while (!m_aio.isEmpty()
               && m_tickRequests < TickRequestBudget) {
            const Result waited = waitAio(&m_aio.first(), results);
            // failActive 已重置全部状态（含清空 m_aio）并返回 Idle；
            // Progress/Again 之外的结果必须立即终止，不能再碰 m_aio。
            if (waited != Result::Progress && waited != Result::Again) {
                return waited;
            }
            if (waited == Result::Again) {
                return Result::Again;
            }
            m_done += m_aio.first().requested;
            results->append({SftpResult::Kind::Progress, m_active.requestId,
                             {}, {}, {}, {}, m_done, m_total, true, {}});
            m_aio.removeFirst();
        }
        while (m_aio.size() < MaxInFlightAio
               && m_tickRequests < TickRequestBudget
               && m_tickBytes < TickByteBudget) {
            if (m_pending.isEmpty() && !m_local.atEnd()) {
                m_pending = m_local.read(ChunkSize);
            }
            if (m_pending.isEmpty()) {
                break;
            }
            const Result began = beginWrite(m_file, m_pending, results);
            // failActive 已重置全部状态（含清空 m_aio）并返回 Idle；
            // Progress/Again 之外的结果必须立即终止，不能再碰 m_aio。
            if (began != Result::Progress && began != Result::Again) {
                return began;
            }
            if (began == Result::Again) {
                return Result::Again;
            }
        }
        if (m_local.atEnd() && m_pending.isEmpty() && m_aio.isEmpty()) {
            m_phase = Phase::TransferClose;
        } else {
            return Result::Again;
        }
    }

    if (m_phase == Phase::TransferClose) {
        const ScopedSyncSftp sync(m_api, m_session);
        const int rc = m_api.sftpCloseFile(m_file);
        if (rc == SSH_AGAIN) {
            return Result::Again;
        }
        m_file = nullptr;
        m_local.close();
        if (rc != SSH_OK) {
            m_api.sftpUnlink(m_sftp, m_active.remotePath.toUtf8());
            return failActive(results, QStringLiteral("远程文件关闭失败"));
        }
        completeTransfer(results, true);
        return m_queue.isEmpty() ? Result::Idle : Result::Progress;
    }
    return Result::Progress;
}

SftpChannel::Result SftpChannel::advanceDownload(QList<SftpResult> *results)
{
    if (m_phase == Phase::None || m_phase == Phase::TransferPrepare) {
        const ScopedSyncSftp sync(m_api, m_session);
        m_file = m_api.sftpOpen(m_sftp, m_active.remotePath.toUtf8(),
                                O_RDONLY, 0);
        if (m_file == nullptr) {
            if (isAgain()) {
                m_phase = Phase::TransferPrepare;
                return Result::Again;
            }
            return failActive(results, QStringLiteral("无法打开远程文件"));
        }
        if (sftp_attributes attrs = m_api.sftpFstat(m_file)) {
            m_total = qint64(attrs->size);
            m_api.sftpAttributesFree(attrs);
        }
        m_api.sftpFileSetNonblocking(m_file);
        m_tempPath = m_active.localPath + QStringLiteral(".part");
        QDir().mkpath(QFileInfo(m_tempPath).absolutePath());
        m_local.setFileName(m_tempPath);
        if (!m_local.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            closeRemoteFiles();
            completeTransfer(
                results, false,
                QStringLiteral("无法写本地文件 %1").arg(m_active.localPath));
            return m_queue.isEmpty() ? Result::Idle : Result::Progress;
        }
        m_phase = Phase::TransferIo;
    }

    if (m_phase == Phase::TransferIo) {
        while (!m_aio.isEmpty()
               && m_tickRequests < TickRequestBudget) {
            AioRequest &request = m_aio.first();
            const Result waited = waitAio(&request, results);
            // failActive 已重置全部状态（含清空 m_aio）并返回 Idle；
            // Progress/Again 之外的结果必须立即终止，不能再碰 m_aio。
            if (waited != Result::Progress && waited != Result::Again) {
                return waited;
            }
            if (waited == Result::Again) {
                return Result::Again;
            }
            if (request.buffer.isEmpty()) {
                m_readEof = true;
            } else if (m_local.write(request.buffer) != request.buffer.size()) {
                return failActive(results, QStringLiteral("写本地磁盘失败"));
            } else {
                m_done += request.buffer.size();
                if (m_total > 0 && m_done > m_total) {
                    m_total = m_done;
                }
                results->append({SftpResult::Kind::Progress, m_active.requestId,
                                 {}, {}, {}, {}, m_done, m_total, true, {}});
            }
            m_aio.removeFirst();
        }
        while (!m_readEof && m_aio.size() < MaxInFlightAio
               && m_tickRequests < TickRequestBudget
               && m_tickBytes < TickByteBudget) {
            const Result began = beginRead(m_file, results);
            // failActive 已重置全部状态（含清空 m_aio）并返回 Idle；
            // Progress/Again 之外的结果必须立即终止，不能再碰 m_aio。
            if (began != Result::Progress && began != Result::Again) {
                return began;
            }
            if (began == Result::Again) {
                return Result::Again;
            }
        }
        if (m_readEof && m_aio.isEmpty()) {
            m_phase = Phase::TransferClose;
        } else {
            return Result::Again;
        }
    }

    if (m_phase == Phase::TransferClose) {
        const ScopedSyncSftp sync(m_api, m_session);
        const int rc = m_api.sftpCloseFile(m_file);
        if (rc == SSH_AGAIN) {
            return Result::Again;
        }
        m_file = nullptr;
        m_local.close();
        QFile::remove(m_active.localPath);
        if (!QFile::rename(m_tempPath, m_active.localPath)) {
            QFile::remove(m_tempPath);
            return failActive(results, QStringLiteral("无法完成本地下载"));
        }
        m_tempPath.clear();
        completeTransfer(results, true);
        return m_queue.isEmpty() ? Result::Idle : Result::Progress;
    }
    return Result::Progress;
}

SftpChannel::Result SftpChannel::advanceCopy(QList<SftpResult> *results)
{
    if (m_active.isDir) {
        if (m_phase == Phase::None) {
            m_active.command = QStringLiteral("cp -a -- ")
                + shellQuote(m_active.path) + QLatin1Char(' ')
                + shellQuote(m_active.dest);
            m_phase = Phase::ExecOpen;
        }
        return advanceExec(results);
    }

    if (m_phase == Phase::None) {
        m_phase = Phase::CopyOpenSrc;
    }
    if (m_phase == Phase::CopyOpenSrc) {
        const ScopedSyncSftp sync(m_api, m_session);
        m_file = m_api.sftpOpen(m_sftp, m_active.path.toUtf8(), O_RDONLY, 0);
        if (m_file == nullptr) {
            if (isAgain()) {
                return Result::Again;
            }
            return failActive(results, QStringLiteral("无法打开源文件"));
        }
        if (sftp_attributes attrs = m_api.sftpFstat(m_file)) {
            m_copyMode = attrs->permissions & 07777;
            m_total = qint64(attrs->size);
            m_api.sftpAttributesFree(attrs);
        }
        m_api.sftpFileSetNonblocking(m_file);
        m_phase = Phase::CopyOpenDst;
    }
    if (m_phase == Phase::CopyOpenDst) {
        const ScopedSyncSftp sync(m_api, m_session);
        m_destFile = m_api.sftpOpen(m_sftp, m_active.dest.toUtf8(),
                                    O_WRONLY | O_CREAT | O_TRUNC,
                                    mode_t(m_copyMode));
        if (m_destFile == nullptr) {
            if (isAgain()) {
                return Result::Again;
            }
            return failActive(results, QStringLiteral("无法创建目标文件"));
        }
        m_api.sftpFileSetNonblocking(m_destFile);
        m_phase = Phase::CopyIo;
    }
    if (m_phase == Phase::CopyIo) {
        while (!m_aio.isEmpty()
               && m_tickRequests < TickRequestBudget) {
            AioRequest &request = m_aio.first();
            const Result waited = waitAio(&request, results);
            // failActive 已重置全部状态（含清空 m_aio）并返回 Idle；
            // Progress/Again 之外的结果必须立即终止，不能再碰 m_aio。
            if (waited != Result::Progress && waited != Result::Again) {
                return waited;
            }
            if (waited == Result::Again) {
                return Result::Again;
            }
            if (request.reading) {
                if (request.buffer.isEmpty()) {
                    m_readEof = true;
                } else {
                    m_pending.append(request.buffer);
                }
            } else {
                m_done += request.requested;
            }
            m_aio.removeFirst();
        }
        if (m_pending.isEmpty() && !m_readEof && m_aio.isEmpty()
            && m_tickRequests < TickRequestBudget
            && m_tickBytes < TickByteBudget) {
            const Result began = beginRead(m_file, results);
            if (began == Result::Error || began == Result::Again) {
                return began;
            }
        }
        if (!m_pending.isEmpty() && m_aio.isEmpty()
            && m_tickRequests < TickRequestBudget
            && m_tickBytes < TickByteBudget) {
            const Result began = beginWrite(m_destFile, m_pending, results);
            if (began == Result::Error || began == Result::Again) {
                return began;
            }
        }
        if (m_readEof && m_pending.isEmpty() && m_aio.isEmpty()) {
            m_phase = Phase::CopyClose;
        } else {
            return Result::Again;
        }
    }
    if (m_phase == Phase::CopyClose) {
        const ScopedSyncSftp sync(m_api, m_session);
        if (m_destFile != nullptr) {
            const int rc = m_api.sftpCloseFile(m_destFile);
            if (rc == SSH_AGAIN) {
                return Result::Again;
            }
            m_destFile = nullptr;
            if (rc != SSH_OK) {
                m_api.sftpUnlink(m_sftp, m_active.dest.toUtf8());
                return failActive(results, QStringLiteral("关闭目标文件失败"));
            }
        }
        if (m_file != nullptr) {
            const int rc = m_api.sftpCloseFile(m_file);
            if (rc == SSH_AGAIN) {
                return Result::Again;
            }
            m_file = nullptr;
            if (rc != SSH_OK) {
                return failActive(results, QStringLiteral("关闭源文件失败"));
            }
        }
        completeOp(results, QStringLiteral("copy"), m_active.path, true);
        return m_queue.isEmpty() ? Result::Idle : Result::Progress;
    }
    return Result::Progress;
}

SftpChannel::Result SftpChannel::advanceExec(QList<SftpResult> *results)
{
    if (m_phase == Phase::None || m_phase == Phase::ExecOpen) {
        m_exec = m_api.newChannel(m_session);
        if (m_exec == nullptr) {
            return failActive(results, QStringLiteral("无法打开远程命令通道"));
        }
        m_phase = Phase::ExecSession;
    }
    if (m_phase == Phase::ExecSession) {
        const int rc = m_api.openChannelSession(m_exec);
        if (rc == SSH_AGAIN) {
            return Result::Again;
        }
        if (rc != SSH_OK) {
            return failActive(results, QStringLiteral("无法打开远程命令通道"));
        }
        m_phase = Phase::ExecRequest;
    }
    if (m_phase == Phase::ExecRequest) {
        const int rc = m_api.requestExec(m_exec, m_active.command.toUtf8());
        if (rc == SSH_AGAIN) {
            return Result::Again;
        }
        if (rc != SSH_OK) {
            return failActive(results, QStringLiteral("远程命令执行失败"));
        }
        m_phase = Phase::ExecRead;
    }
    if (m_phase == Phase::ExecRead) {
        bool progressed = false;
        for (int stream = 0; stream < 2; ++stream) {
            QByteArray buffer(4096, Qt::Uninitialized);
            const int n = m_api.readChannelNonblocking(
                m_exec, buffer.data(), quint32(buffer.size()), stream == 1);
            if (n == SSH_AGAIN || n == 0) {
                continue;
            }
            if (n < 0 && n != SSH_EOF) {
                return failActive(results, QStringLiteral("读取远程命令输出失败"));
            }
            if (n > 0) {
                buffer.resize(n);
                m_execOutput.append(buffer);
                progressed = true;
            }
        }
        if (!m_api.channelIsEof(m_exec)) {
            return progressed ? Result::Progress : Result::Again;
        }
        m_phase = Phase::ExecClose;
    }
    if (m_phase == Phase::ExecClose) {
        int rc = m_api.sendChannelEof(m_exec);
        if (rc == SSH_AGAIN) {
            return Result::Again;
        }
        rc = m_api.closeChannel(m_exec);
        if (rc == SSH_AGAIN) {
            return Result::Again;
        }
        const int status = m_api.channelGetExitStatus(m_exec);
        m_api.freeChannel(m_exec);
        m_exec = nullptr;
        const bool ok = status == 0;
        QString detail = QString::fromUtf8(m_execOutput);
        if (!ok && detail.trimmed().isEmpty()) {
            detail = QStringLiteral("退出码 %1").arg(status);
        }
        completeOp(results, sftpOpName(m_active.op),
                   m_active.op == SftpOp::Copy ? m_active.path
                                               : m_active.command,
                   ok, detail);
        return m_queue.isEmpty() ? Result::Idle : Result::Progress;
    }
    return Result::Progress;
}

SftpChannel::Result SftpChannel::waitAio(AioRequest *request,
                                         QList<SftpResult> *results)
{
    ++m_tickRequests;
    // libssh 0.11 的 sftp_aio_wait_* 非阻塞 poll 路径在响应包分片到达时
    // 也会踩 SSH_AGAIN 被当作已读字节的坑（真机 16K DATA 包必分片，
    // 实测 "Invalid sftp packet size!"）。等待也走阻塞；begin 提交侧
    // 仍是非阻塞，4 路在途流水线让每次阻塞等待摊薄 RTT。
    const ScopedSyncSftp sync(m_api, m_session);
    if (request->reading) {
        QByteArray buffer(int(request->requested), Qt::Uninitialized);
        const ssize_t n = m_api.sftpAioWaitRead(
            &request->aio, buffer.data(), size_t(buffer.size()));
        if (n == SSH_AGAIN) {
            return Result::Again;
        }
        if (n == SSH_ERROR) {
            return failActive(results, QStringLiteral("SFTP AIO 读取失败"));
        }
        buffer.resize(int(qMax(ssize_t(0), n)));
        request->buffer = buffer;
        m_tickBytes += int(qMax(ssize_t(0), n));
        return Result::Progress;
    }
    const ssize_t n = m_api.sftpAioWaitWrite(&request->aio);
    if (n == SSH_AGAIN) {
        return Result::Again;
    }
    if (n == SSH_ERROR) {
        return failActive(results, QStringLiteral("SFTP AIO 写入失败"));
    }
    m_tickBytes += int(request->requested);
    return Result::Progress;
}

SftpChannel::Result SftpChannel::beginRead(sftp_file file,
                                           QList<SftpResult> *results)
{
    ++m_tickRequests;
    sftp_aio aio = nullptr;
    const ssize_t n =
        m_api.sftpAioBeginRead(file, size_t(ChunkSize), &aio);
    if (n == SSH_AGAIN) {
        return Result::Again;
    }
    if (n == SSH_ERROR) {
        return failActive(results, QStringLiteral("SFTP AIO 读取失败"));
    }
    if (n == 0) {
        m_readEof = true;
        if (aio != nullptr) {
            m_api.sftpAioFree(aio);
        }
        return Result::Progress;
    }
    AioRequest request;
    request.aio = aio;
    request.requested = n;
    request.reading = true;
    m_aio.append(request);
    return Result::Progress;
}

SftpChannel::Result SftpChannel::beginWrite(sftp_file file,
                                            const QByteArray &bytes,
                                            QList<SftpResult> *results)
{
    ++m_tickRequests;
    sftp_aio aio = nullptr;
    const ssize_t n = m_api.sftpAioBeginWrite(
        file, bytes.constData(), size_t(bytes.size()), &aio);
    if (n == SSH_AGAIN) {
        return Result::Again;
    }
    if (n == SSH_ERROR || n < 0) {
        return failActive(results, QStringLiteral("SFTP AIO 写入失败"));
    }
    AioRequest request;
    request.aio = aio;
    request.requested = n;
    request.reading = false;
    request.buffer = bytes.left(int(n));
    m_aio.append(request);
    if (n >= bytes.size()) {
        m_pending.clear();
    } else {
        m_pending = bytes.mid(int(n));
    }
    m_tickBytes += int(n);
    return Result::Progress;
}

SftpChannel::Result SftpChannel::failActive(QList<SftpResult> *results,
                                            const QString &operation)
{
    const QString error = apiError(operation);
    const SftpRequest request = m_active;
    cleanupTransfer(false);
    closeRemoteFiles();
    releaseSftp();
    resetActive();
    switch (request.op) {
    case SftpOp::List:
        results->append({SftpResult::Kind::ListedFailed, request.requestId, {},
                         {}, request.path, {}, 0, 0, false, error});
        break;
    case SftpOp::Upload:
    case SftpOp::Download:
        results->append({SftpResult::Kind::TransferFinished, request.requestId,
                         {}, {}, {}, {}, 0, 0, false, error});
        break;
    default:
        results->append({SftpResult::Kind::OpFinished, request.requestId,
                         sftpOpName(request.op),
                         request.op == SftpOp::Exec ? request.command
                                                    : request.path,
                         {}, {}, 0, 0, false, error});
        break;
    }
    return Result::Idle;
}

void SftpChannel::completeOp(QList<SftpResult> *results, const QString &op,
                             const QString &arg, bool ok, const QString &error)
{
    results->append({SftpResult::Kind::OpFinished, m_active.requestId, op, arg,
                     {}, {}, 0, 0, ok, error});
    resetActive();
}

void SftpChannel::completeTransfer(QList<SftpResult> *results, bool ok,
                                   const QString &error)
{
    results->append({SftpResult::Kind::TransferFinished, m_active.requestId,
                     {}, {}, {}, {}, m_done, m_total, ok, error});
    m_local.close();
    resetActive();
}

void SftpChannel::completeList(QList<SftpResult> *results, bool ok,
                               const QString &error)
{
    if (ok) {
        results->append({SftpResult::Kind::Listed, m_active.requestId, {}, {},
                         m_active.path, m_entries, 0, 0, true, {}});
    } else {
        results->append({SftpResult::Kind::ListedFailed, m_active.requestId,
                         {}, {}, m_active.path, {}, 0, 0, false, error});
    }
    resetActive();
}

void SftpChannel::emitCancelled(QList<SftpResult> *results,
                                const SftpRequest &request)
{
    results->append(sftpCancelledResult(request));
}

void SftpChannel::abortActive(QList<SftpResult> *results, const QString &error)
{
    const SftpRequest request = m_active;
    cleanupTransfer(true);
    closeRemoteFiles();
    resetActive();
    switch (request.op) {
    case SftpOp::List:
        results->append({SftpResult::Kind::ListedFailed, request.requestId, {},
                         {}, request.path, {}, 0, 0, false, error});
        break;
    case SftpOp::Upload:
    case SftpOp::Download:
        results->append({SftpResult::Kind::TransferFinished, request.requestId,
                         {}, {}, {}, {}, 0, 0, false, error});
        break;
    default:
        results->append({SftpResult::Kind::OpFinished, request.requestId,
                         sftpOpName(request.op),
                         request.op == SftpOp::Exec ? request.command
                                                    : request.path,
                         {}, {}, 0, 0, false, error});
        break;
    }
}

void SftpChannel::cleanupTransfer(bool cancelled)
{
    freeAio();
    if (m_local.isOpen()) {
        m_local.close();
    }
    if (!m_tempPath.isEmpty()) {
        QFile::remove(m_tempPath);
        m_tempPath.clear();
    }
    if (cancelled && m_sftp != nullptr
        && (m_active.op == SftpOp::Upload)
        && !m_active.remotePath.isEmpty()) {
        m_api.sftpUnlink(m_sftp, m_active.remotePath.toUtf8());
    }
}

void SftpChannel::freeAio()
{
    for (AioRequest &request : m_aio) {
        if (request.aio != nullptr) {
            m_api.sftpAioFree(request.aio);
            request.aio = nullptr;
        }
    }
    m_aio.clear();
}

void SftpChannel::closeRemoteFiles()
{
    const ScopedSyncSftp sync(m_api, m_session);
    if (m_dir != nullptr) {
        m_api.sftpClosedir(m_dir);
        m_dir = nullptr;
    }
    if (m_file != nullptr) {
        m_api.sftpCloseFile(m_file);
        m_file = nullptr;
    }
    if (m_destFile != nullptr) {
        m_api.sftpCloseFile(m_destFile);
        m_destFile = nullptr;
    }
    if (m_exec != nullptr) {
        m_api.freeChannel(m_exec);
        m_exec = nullptr;
    }
}

void SftpChannel::releaseSftp()
{
    if (m_sftp != nullptr) {
        // sftp_free 连同包装进去的 channel 一起 EOF 并释放；阻塞模式下
        // EOF 发送也是同步完成的。所有权已随之交还，必须清空指针，
        // 否则重建时会拿已释放的 channel 去 open（真机 use-after-free）。
        const ScopedSyncSftp sync(m_api, m_session);
        m_api.sftpFree(m_sftp);
        m_sftp = nullptr;
        m_sftpChannel = nullptr;
    } else if (m_sftpChannel != nullptr) {
        // 还没交给 sftp 会话的半开通道自己释放。
        m_api.freeChannel(m_sftpChannel);
        m_sftpChannel = nullptr;
    }
    m_sftpReady = false;
    m_channelStep = ChannelStep::Open;
}

void SftpChannel::resetActive()
{
    m_hasActive = false;
    m_cancelled = false;
    m_phase = Phase::None;
    m_entries.clear();
    m_pending.clear();
    m_execOutput.clear();
    m_done = 0;
    m_total = 0;
    m_readEof = false;
    m_active = {};
}

bool SftpChannel::isAgain() const
{
    return m_api.errorCode(m_session) == SSH_AGAIN;
}

bool SftpChannel::onOwnerThread() const
{
    return QThread::currentThread() == m_ownerThread;
}

QString SftpChannel::apiError(const QString &fallback) const
{
    // session 错误文本会被更早的尽力而为请求（如被拒绝的 env）污染，
    // 附上 sftp 层错误码便于定位真实失败原因。
    const int sftpCode =
        m_sftp != nullptr ? m_api.sftpGetError(m_sftp) : 0;
    const QString detail = m_api.errorString(m_session);
    if (detail.isEmpty()) {
        return sftpCode == 0
            ? fallback
            : QStringLiteral("%1: sftp error %2").arg(fallback).arg(sftpCode);
    }
    return sftpCode == 0
        ? QStringLiteral("%1: %2").arg(fallback, detail)
        : QStringLiteral("%1: sftp error %2: %3")
              .arg(fallback)
              .arg(sftpCode)
              .arg(detail);
}

QString SftpChannel::shellQuote(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}
