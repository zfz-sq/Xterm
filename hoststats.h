#ifndef HOSTSTATS_H
#define HOSTSTATS_H

#include <QObject>
#include <QString>

class SshSession;

struct HostCpuSample
{
    bool ok = false;
    quint64 total = 0;
    quint64 idle = 0;
};

struct HostMemSample
{
    bool ok = false;
    bool percentOk = false;
    int percent = 0;
    qint64 usedKiB = 0;
    qint64 totalKiB = 0;
};

/*
 * HostStats: 状态栏的主机 CPU/内存适配器。
 *
 * 不再自己开第二条 SSH 连接：解析共享会话 stats 通道送来的 /proc 帧，
 * 维护相邻两帧的 CPU 计数差值；通道降级（statsDegraded）时整帧显示
 * "--"。解析函数是公开静态，供单测直接喂 /proc 文本。
 */
class HostStats : public QObject
{
    Q_OBJECT

public:
    explicit HostStats(SshSession *session, QObject *parent = nullptr);

    static HostCpuSample parseCpuSample(const QString &text);
    static HostMemSample parseMemSample(const QString &text);

    // 丢弃 CPU 差值基线（重连后调用，避免跨连接算差值）。
    void resetDelta();

    void testApplySample(const QString &sample);   // 仅单测用

signals:
    // 任一字段无效时对应的 ok 为 false，界面显示 "--"。
    // usedKiB / totalKiB 来自 /proc/meminfo，单位是 KiB。
    void statsUpdated(bool cpuOk, int cpuPercent,
                      bool memOk, bool memPercentOk, int memPercent,
                      qint64 usedKiB, qint64 totalKiB);

private slots:
    void onSample(const QString &sample);
    void onDegraded();

private:
    void applySample(const QString &sample);

    SshSession *m_session = nullptr;
    bool m_haveCpu = false;
    quint64 m_prevTotal = 0;
    quint64 m_prevIdle = 0;
};

#endif // HOSTSTATS_H
