#include "fakes/fakelibsshapi.h"
#include "hoststats.h"
#include "ssh/statschannel.h"

#include <QSignalSpy>
#include <QtTest>

namespace {

const char *const kProcSample =
    "cpu  100 0 50 850 0 0 0 0 0 0\n"
    "cpu0 100 0 50 850 0 0 0 0 0 0\n"
    "intr 12345\n"
    "ctxt 999\n"
    "MemTotal:       1000 kB\n"
    "MemFree:         100 kB\n"
    "MemAvailable:    400 kB\n"
    "Buffers:          50 kB\n"
    "Cached:          150 kB\n";

} // namespace

class TestStatsChannel : public QObject
{
    Q_OBJECT

private slots:
    // ---- 计划 Step 1：/proc 解析 ----
    void parsesNormalProcSample();
    void missingMemAvailableFallsBackToFreePlusCache();
    void malformedCountersFailCleanly();
    void cpuCounterResetInvalidatesDelta();
    // ---- 计划 Step 1/3：帧提取 ----
    void extractsMultipleFramesFromOneRead();
    void reassemblesSplitFramesAcrossReads();
    // ---- 计划 Step 2/3：通道 ----
    void streamsFramedSamplesViaOwnExecChannel();
    void openFailureDegradesAndBacksOffExponentially();
    void stopCancelsRetriesAndFreesChannel();
    void recoversAfterDegradedSample();
};

void TestStatsChannel::parsesNormalProcSample()
{
    const HostCpuSample cpu =
        HostStats::parseCpuSample(QLatin1String(kProcSample));
    QVERIFY(cpu.ok);
    QCOMPARE(cpu.total, quint64(1000));
    QCOMPARE(cpu.idle, quint64(850));

    const HostMemSample mem =
        HostStats::parseMemSample(QLatin1String(kProcSample));
    QVERIFY(mem.ok);
    QVERIFY(mem.percentOk);
    QCOMPARE(mem.totalKiB, qint64(1000));
    QCOMPARE(mem.usedKiB, qint64(600));   // MemAvailable 400
    QCOMPARE(mem.percent, 60);
}

void TestStatsChannel::missingMemAvailableFallsBackToFreePlusCache()
{
    const QString text =
        QStringLiteral("cpu  10 0 5 85 0 0 0 0\n"
                       "MemTotal:       1000 kB\n"
                       "MemFree:         100 kB\n"
                       "Buffers:          50 kB\n"
                       "Cached:          150 kB\n");
    const HostMemSample mem = HostStats::parseMemSample(text);
    QVERIFY(mem.ok);
    QCOMPARE(mem.usedKiB, qint64(700));   // 1000-(free 100+buffers 50+cached 150)
}

void TestStatsChannel::malformedCountersFailCleanly()
{
    QVERIFY(!HostStats::parseCpuSample(
                 QStringLiteral("cpu  abc 0 1 2 3\n")).ok);
    QVERIFY(!HostStats::parseCpuSample(
                 QStringLiteral("cpu  1 2 3\n")).ok);
    QVERIFY(!HostStats::parseCpuSample(
                 QStringLiteral("MemTotal: 100 kB\n")).ok);
    QVERIFY(!HostStats::parseMemSample(
                 QStringLiteral("MemTotal: xxx kB\n"
                                "MemFree: 1 kB\n")).ok);
    QVERIFY(!HostStats::parseMemSample(QString()).ok);
}

void TestStatsChannel::cpuCounterResetInvalidatesDelta()
{
    HostStats stats(nullptr);
    const QString first =
        QStringLiteral("cpu  1000 0 0 1000 0 0 0 0\n"
                       "MemTotal: 100 kB\nMemFree: 50 kB\n");
    const QString reset =
        QStringLiteral("cpu  5 0 0 5 0 0 0 0\n"
                       "MemTotal: 100 kB\nMemFree: 50 kB\n");
    const QString afterReset =
        QStringLiteral("cpu  10 0 0 10 0 0 0 0\n"
                       "MemTotal: 100 kB\nMemFree: 50 kB\n");
    // 首帧只建基线（无差值）；第二帧计数回退（远端重启）→ cpuOk=false
    // 并重新基线；第三帧在新基线上恢复。
    QSignalSpy spy(&stats, &HostStats::statsUpdated);
    stats.testApplySample(first);
    stats.testApplySample(reset);
    stats.testApplySample(afterReset);
    QCOMPARE(spy.count(), 3);
    QVERIFY(!spy.at(0).at(0).toBool());      // 首帧没有差值
    QVERIFY(!spy.at(1).at(0).toBool());      // 回退帧 cpuOk=false
    QVERIFY(spy.at(2).at(0).toBool());       // 新基线上恢复
}

void TestStatsChannel::extractsMultipleFramesFromOneRead()
{
    QByteArray buffer =
        QByteArrayLiteral("cpu  1 0 0 9 0\n") + StatsChannel::kFrameBegin
        + QByteArrayLiteral("\ncpu  2 0 0 8 0\nMemTotal: 1 kB\n")
        + StatsChannel::kFrameEnd + QByteArrayLiteral("\n")
        + StatsChannel::kFrameBegin
        + QByteArrayLiteral("\nMemTotal: 2 kB\n")
        + StatsChannel::kFrameEnd + QByteArrayLiteral("\n");
    QList<QString> samples;
    QCOMPARE(StatsChannel::extractFrames(&buffer, &samples), 2);
    QCOMPARE(samples.size(), 2);
    QCOMPARE(samples.first(), QStringLiteral("cpu  2 0 0 8 0\nMemTotal: 1 kB"));
    QCOMPARE(samples.last(), QStringLiteral("MemTotal: 2 kB"));
    QVERIFY(buffer.isEmpty());
}

void TestStatsChannel::reassemblesSplitFramesAcrossReads()
{
    const QByteArray whole =
        StatsChannel::kFrameBegin
        + QByteArrayLiteral("\ncpu  3 0 0 7 0\nMemTotal: 4 kB\n")
        + StatsChannel::kFrameEnd;
    QList<QString> samples;
    // 在每个字节边界断开：先喂前半，再补齐后半（外加一个新 begin，
    // 验证帧提取不会吃掉后续标记）。同一 buffer 累积，模拟分片读流。
    for (int cut = 1; cut < whole.size(); ++cut) {
        QByteArray buffer = whole.left(cut);
        QCOMPARE(StatsChannel::extractFrames(&buffer, &samples), 0);
        buffer += whole.mid(cut) + StatsChannel::kFrameBegin;
        QCOMPARE(StatsChannel::extractFrames(&buffer, &samples), 1);
        QCOMPARE(samples.last(),
                 QStringLiteral("cpu  3 0 0 7 0\nMemTotal: 4 kB"));
        QCOMPARE(buffer, StatsChannel::kFrameBegin);
        samples.clear();
    }
}

void TestStatsChannel::streamsFramedSamplesViaOwnExecChannel()
{
    FakeLibsshApi api;
    StatsChannel channel(api, api.newSession());
    channel.start();
    QVERIFY(channel.isActive());

    // 打开 → exec → 流式读取都由 advance 驱动，每轮推进一步。
    QList<QString> samples;
    bool degraded = true;
    bool recovered = true;
    channel.advance(&samples, &degraded, &recovered);
    const ssh_channel statsChannel = api.createdChannels.constLast();
    api.streamByChannel[statsChannel] = {
        StatsChannel::kFrameBegin
            + QByteArrayLiteral("\ncpu  1 0 0 9 0\nMemTotal: 8 kB\n")
            + StatsChannel::kFrameEnd,
    };

    for (int i = 0; i < 10 && samples.isEmpty(); ++i) {
        channel.advance(&samples, &degraded, &recovered);
    }
    QCOMPARE(samples.size(), 1);
    QCOMPARE(samples.first(),
             QStringLiteral("cpu  1 0 0 9 0\nMemTotal: 8 kB"));
    QVERIFY(!degraded);
    QVERIFY(!recovered);

    // exec 的是自包含远端循环，命令里没有任何面向 shell 的注入。
    QCOMPARE(api.execCommands.size(), 1);
    QVERIFY(api.execCommands.first().contains("XTERM_STATS"));
    QVERIFY(api.execCommands.first().contains("cat /proc/stat"));
}

void TestStatsChannel::openFailureDegradesAndBacksOffExponentially()
{
    FakeLibsshApi api;
    api.openChannelResults = {SSH_ERROR};
    StatsChannel channel(api, api.newSession());
    channel.start();

    QList<QString> samples;
    bool degraded = false;
    bool recovered = false;
    QCOMPARE(channel.advance(&samples, &degraded, &recovered),
             StatsChannel::Result::Progress);
    QVERIFY(degraded);
    QCOMPARE(channel.retryDelayMs(), StatsChannel::InitialRetryDelayMs);

    // 退避期内 advance 不触碰 API。
    const int callsBefore = api.calls.size();
    QCOMPARE(channel.advance(&samples, &degraded, &recovered),
             StatsChannel::Result::Again);
    QCOMPARE(api.calls.size(), callsBefore);

    // 每次失败延迟翻倍，封顶 30 秒。
    api.openChannelResults = {SSH_ERROR};
    channel.skipRetryDelayForTest();
    channel.advance(&samples, &degraded, &recovered);
    QCOMPARE(channel.retryDelayMs(), 2 * StatsChannel::InitialRetryDelayMs);
    for (int i = 0; i < 10; ++i) {
        api.openChannelResults = {SSH_ERROR};
        channel.skipRetryDelayForTest();
        channel.advance(&samples, &degraded, &recovered);
    }
    QCOMPARE(channel.retryDelayMs(), StatsChannel::MaxRetryDelayMs);
}

void TestStatsChannel::stopCancelsRetriesAndFreesChannel()
{
    FakeLibsshApi api;
    StatsChannel channel(api, api.newSession());
    channel.start();

    // 首轮 advance 打开通道，随后脚本化该通道的输出。
    QList<QString> samples;
    bool degraded = false;
    bool recovered = false;
    channel.advance(&samples, &degraded, &recovered);
    const ssh_channel statsChannel = api.createdChannels.constLast();
    api.streamByChannel[statsChannel] = {
        StatsChannel::kFrameBegin + QByteArrayLiteral("\nMemTotal: 1 kB\n")
            + StatsChannel::kFrameEnd,
    };
    while (samples.isEmpty()) {
        channel.advance(&samples, &degraded, &recovered);
    }
    const int closeBefore = api.channelCloseCount;
    const int freeBefore = api.channelFreeCount;

    channel.stop();
    QVERIFY(!channel.isActive());
    QVERIFY(api.channelCloseCount > closeBefore);
    QVERIFY(api.channelFreeCount > freeBefore);

    const int callsBefore = api.calls.size();
    QCOMPARE(channel.advance(&samples, &degraded, &recovered),
             StatsChannel::Result::Again);
    QCOMPARE(api.calls.size(), callsBefore);
}

void TestStatsChannel::recoversAfterDegradedSample()
{
    FakeLibsshApi api;
    StatsChannel channel(api, api.newSession());
    channel.start();

    // 第一次 exec 失败 → 降级；随后成功并拿到帧 → recovered。
    api.execResults = {SSH_ERROR};
    QList<QString> samples;
    bool degraded = false;
    bool recovered = false;
    while (!degraded) {
        channel.advance(&samples, &degraded, &recovered);
    }
    QCOMPARE(channel.retryDelayMs(), StatsChannel::InitialRetryDelayMs);

    api.execResults = {};
    channel.skipRetryDelayForTest();
    // 重试先建新通道，再 exec；脚本化新通道的输出。
    channel.advance(&samples, &degraded, &recovered);
    const ssh_channel statsChannel = api.createdChannels.constLast();
    api.streamByChannel[statsChannel] = {
        StatsChannel::kFrameBegin + QByteArrayLiteral("\nMemTotal: 3 kB\n")
            + StatsChannel::kFrameEnd,
    };
    samples.clear();
    degraded = false;
    while (samples.isEmpty()) {
        channel.advance(&samples, &degraded, &recovered);
    }
    QVERIFY(recovered);
    QVERIFY(!degraded);
}

QTEST_MAIN(TestStatsChannel)
#include "test_statschannel.moc"
