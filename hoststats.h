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
 *
 * M1.d 起最新一帧的展示缓存（CPU/内存各字段与 ok 标志）下沉到本类，
 * 状态栏文案由 statusText() 直接生成（MainWindow 不再逐字段保存）；
 * 未连接期间到达的迟到帧在 onSample 里丢弃，避免断开后残留旧读数。
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

    // 清空展示缓存（断开后调用，状态栏回 "--"）。
    void clearCache();

    // 状态栏文本："user@host:port · CPU xx% · 内存 xx% · used/total"。
    // 主机标识取会话当前 target；未连接/无样本时各字段显示 "--"。
    QString statusText() const;

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
    void updateCache(bool cpuOk, int cpuPercent, bool memOk, bool memPercentOk,
                     int memPercent, qint64 usedKiB, qint64 totalKiB);

    SshSession *m_session = nullptr;
    bool m_haveCpu = false;
    quint64 m_prevTotal = 0;
    quint64 m_prevIdle = 0;
    // 最新一帧的展示缓存（statusText 的数据源）
    bool m_cpuOk = false;
    int m_cpuPercent = 0;
    bool m_memOk = false;
    bool m_memPercentOk = false;
    int m_memPercent = 0;
    qint64 m_usedKiB = 0;
    qint64 m_totalKiB = 0;
};

#endif // HOSTSTATS_H
