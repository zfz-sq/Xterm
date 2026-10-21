#ifndef STATSCHANNEL_H
#define STATSCHANNEL_H

#include "libsshapi.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QString>

/*
 * StatsChannel: 共享连接上的主机状态采样通道。
 *
 * 独占一条 exec 通道跑远端循环，每 3 秒在 /proc/stat 与 /proc/meminfo
 * 外面打 begin/end 标记输出；只有标记之间的内容才算样本，循环自身或
 * 服务器插入的杂音不会进入 shell，也不会污染采样。
 *
 * 任何失败都不算错误：关闭通道后按指数退避重开（1s 起步，封顶 30s），
 * shell 与 SFTP 完全不受影响。由 worker 在 Ready 轮询里逐 tick 驱动，
 * 所有 libssh 调用走注入的 LibsshApi。
 */
class StatsChannel
{
public:
    enum class Result {
        Progress,   // 本轮有推进（含失败转入退避），可继续轮询
        Again       // 本轮无事可做（未激活或退避未到期）
    };

    static const QByteArray kFrameBegin;
    static const QByteArray kFrameEnd;
    static constexpr int InitialRetryDelayMs = 1000;
    static constexpr int MaxRetryDelayMs = 30000;

    explicit StatsChannel(LibsshApi &api, ssh_session session);
    ~StatsChannel();

    StatsChannel(const StatsChannel &) = delete;
    StatsChannel &operator=(const StatsChannel &) = delete;

    void start();
    void stop();
    bool isActive() const { return m_active; }
    int retryDelayMs() const { return m_retryDelayMs; }

    // 每轮推进一步（打开/exec/读流/退避）。取到样本时追加进 samples；
    // degraded 在本轮降级时置真，recovered 在降级后的首个成功帧置真。
    // 两个出参每轮先清零。
    Result advance(QList<QString> *samples, bool *degraded, bool *recovered);

    // 从 buffer 提取所有完整帧（begin/end 标记之间），返回帧数。buffer
    // 只保留可能构成下一帧 begin 前缀的尾巴，其余残留丢弃。
    static int extractFrames(QByteArray *buffer, QList<QString> *samples);

    // 仅测试用：把退避截止时间拨到现在，让下一轮 advance 立即重试。
    void skipRetryDelayForTest();

private:
    enum class Step {
        Idle,
        OpenChannel,
        RequestExec,
        Streaming,
        Backoff
    };

    void failChannel(bool *degraded);
    void releaseChannel(bool closeFirst);
    void resetRetry();

    LibsshApi &m_api;
    ssh_session m_session;
    Step m_step = Step::Idle;
    ssh_channel m_channel = nullptr;
    QByteArray m_buffer;
    bool m_active = false;
    bool m_degraded = false;      // 当前处于降级（用于判 recovered）
    int m_retryDelayMs = InitialRetryDelayMs / 2;
    qint64 m_retryAtMs = 0;
    QElapsedTimer m_clock;
};

#endif // STATSCHANNEL_H
