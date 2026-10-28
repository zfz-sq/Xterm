#include "fakes/fakelibsshapi.h"
#include "ssh/knownhostsstore.h"
#include "ssh/shellchannel.h"
#include "ssh/sshworker.h"
#include "ssh/statschannel.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

namespace {

SshTarget target()
{
    SshTarget value;
    value.host = QStringLiteral("shell.test");
    value.user = QStringLiteral("tester");
    return value;
}

void prepareSuccessfulConnection(FakeLibsshApi &api)
{
    api.knownState = SSH_KNOWN_HOSTS_OK;
    api.agentResults = {SSH_AUTH_SUCCESS};
}

QTimer *workerTimer(SshWorker &worker)
{
    const QList<QTimer *> timers = worker.findChildren<QTimer *>();
    return timers.isEmpty() ? nullptr : timers.first();
}

} // namespace

class TestSshWorker : public QObject
{
    Q_OBJECT

private slots:
    void requestsXtermPtyWithInitialSize();
    void rejectedEnvironmentDoesNotFailShellOpen();
    void forwardsShellBytesWithoutModification();
    void handlesPartialWritesWithoutDroppingInput();
    void coalescesResizeAndSendsNewestDimensions();
    void shellEofClosesConnectionExactlyOnce();
    void staleGenerationCommandsAreIgnored();
    void closeDuringConnectDoesNotEmitConnected();
    void schedulerReturnsToQtEventLoopOnSshAgain();
    void readSshEofClosesWithoutFailure();
    void closeRetriesAsynchronouslyBeforeFreeing();
    void connectingAgainTimerIsNotZeroMs();
    void openingShellAgainTimerIsNotZeroMs();
    void nonOwnerShellChannelCallsDoNotTouchHandles();
    void pendingWriteDoesNotStarveOutputWithinTwoTicks();
    void streamsStatsSamplesOnDedicatedChannel();
    void statsFailureDegradesButShellStaysReady();
};

void TestSshWorker::requestsXtermPtyWithInitialSize()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);

    worker.connectToHost(7, target(), 132, 43);

    QTRY_COMPARE(connected.count(), 1);
    QCOMPARE(api.ptyTerm, QByteArrayLiteral("xterm-256color"));
    QCOMPARE(api.ptyCols, 132);
    QCOMPARE(api.ptyRows, 43);
    QCOMPARE(api.environment.value(QByteArrayLiteral("TERM")),
             QByteArrayLiteral("xterm-256color"));
    QCOMPARE(api.environment.value(QByteArrayLiteral("COLORTERM")),
             QByteArrayLiteral("truecolor"));
    QCOMPARE(api.requestShellCount, 1);
    QCOMPARE(connected.first().at(0).toULongLong(), quint64(7));
}

void TestSshWorker::rejectedEnvironmentDoesNotFailShellOpen()
{
    // env 请求是尽力而为的：有些 sshd（嵌入式板子常见）直接拒绝，
    // shell 打开流程必须跳过继续，而不是失败断开。
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.environmentResults = {SSH_ERROR, SSH_ERROR};
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);
    QSignalSpy failed(&worker, &SshWorker::connectionFailed);

    worker.connectToHost(7, target(), 80, 24);

    QTRY_COMPARE(connected.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(api.requestShellCount, 1);
}

void TestSshWorker::forwardsShellBytesWithoutModification()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.readEvents = {
        {false, QByteArray("\x1b[2Jtitle:\x1b]0;raw\x07", 18)},
        {true, QByteArray::fromHex("e4b8ad00")},
        {false, QByteArrayLiteral("\x1b[")},
        {false, QByteArrayLiteral("31m")}
    };
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy data(&worker, &SshWorker::shellData);

    worker.connectToHost(9, target(), 80, 24);

    QTRY_VERIFY(data.count() >= 4);
    QByteArray received;
    for (const QList<QVariant> &event : data) {
        QCOMPARE(event.at(0).toULongLong(), quint64(9));
        received += event.at(1).toByteArray();
    }
    const QByteArray expected =
        QByteArray("\x1b[2Jtitle:\x1b]0;raw\x07", 18)
        + QByteArray::fromHex("e4b8ad00")
        + QByteArrayLiteral("\x1b[")
        + QByteArrayLiteral("31m");
    QCOMPARE(received, expected);
}

void TestSshWorker::handlesPartialWritesWithoutDroppingInput()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.writeResults = {2, SSH_AGAIN, 1, 3};
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);

    worker.connectToHost(11, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    worker.writeShell(11, QByteArrayLiteral("abcdef"));

    QTRY_COMPARE(api.acceptedWrites, QByteArrayLiteral("abcdef"));
    QCOMPARE(api.writeCalls,
             QList<QByteArray>({QByteArrayLiteral("abcdef"),
                                QByteArrayLiteral("cdef"),
                                QByteArrayLiteral("cdef"),
                                QByteArrayLiteral("def")}));
}

void TestSshWorker::coalescesResizeAndSendsNewestDimensions()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);

    worker.connectToHost(13, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    api.resizeCalls.clear();
    worker.resizeShell(13, 90, 30);
    worker.resizeShell(13, 100, 40);
    worker.resizeShell(13, 120, 50);

    QTRY_COMPARE(api.resizeCalls.size(), 1);
    QCOMPARE(api.resizeCalls.first(), qMakePair(120, 50));
}

void TestSshWorker::shellEofClosesConnectionExactlyOnce()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);
    QSignalSpy closed(&worker, &SshWorker::connectionClosed);

    worker.connectToHost(15, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    api.channelEof = true;

    QTRY_COMPARE(closed.count(), 1);
    QTest::qWait(20);
    QCOMPARE(closed.count(), 1);
    QCOMPARE(api.channelCloseCount, 1);
    QCOMPARE(api.channelFreeCount, 1);
    QCOMPARE(api.disconnectCount, 1);
}

void TestSshWorker::staleGenerationCommandsAreIgnored()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);

    worker.connectToHost(17, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    api.resizeCalls.clear();
    worker.writeShell(16, QByteArrayLiteral("stale"));
    worker.resizeShell(16, 200, 70);
    worker.closeConnection(16);
    QTest::qWait(20);

    QVERIFY(api.writeCalls.isEmpty());
    QVERIFY(api.resizeCalls.isEmpty());
    QCOMPARE(api.disconnectCount, 0);
}

void TestSshWorker::closeDuringConnectDoesNotEmitConnected()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    api.connectResults = {SSH_AGAIN, SSH_AGAIN};
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);
    QSignalSpy closed(&worker, &SshWorker::connectionClosed);

    worker.connectToHost(19, target(), 80, 24);
    worker.closeConnection(19);

    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(connected.count(), 0);
}

void TestSshWorker::schedulerReturnsToQtEventLoopOnSshAgain()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    api.connectResults.fill(SSH_AGAIN, 100);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    bool eventLoopRan = false;

    worker.connectToHost(21, target(), 80, 24);
    QTimer::singleShot(0, this, [&eventLoopRan] { eventLoopRan = true; });

    QTRY_VERIFY(eventLoopRan);
    QVERIFY(api.calls.count(QStringLiteral("connect")) < 100);
    worker.closeConnection(21);
}

void TestSshWorker::readSshEofClosesWithoutFailure()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.stdoutReadResults = {SSH_EOF};
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy closed(&worker, &SshWorker::connectionClosed);
    QSignalSpy failed(&worker, &SshWorker::connectionFailed);

    worker.connectToHost(23, target(), 80, 24);

    QTRY_COMPARE(closed.count(), 1);
    QTest::qWait(20);
    QCOMPARE(closed.count(), 1);
    QCOMPARE(failed.count(), 0);
}

void TestSshWorker::closeRetriesAsynchronouslyBeforeFreeing()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.sendEofResults = {SSH_AGAIN, SSH_OK};
    api.closeChannelResults = {SSH_AGAIN, SSH_OK};
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);
    QSignalSpy closed(&worker, &SshWorker::connectionClosed);

    worker.connectToHost(25, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    worker.closeConnection(25);

    QCOMPARE(api.channelFreeCount, 0);
    QCOMPARE(api.disconnectCount, 0);
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(api.channelEofCount, 2);
    QCOMPARE(api.channelCloseCount, 2);
    QCOMPARE(api.channelFreeCount, 1);
    QCOMPARE(api.disconnectCount, 1);
    QVERIFY(api.calls.indexOf(QStringLiteral("freeChannel"))
            > api.calls.lastIndexOf(QStringLiteral("closeChannel")));
    QVERIFY(api.calls.indexOf(QStringLiteral("disconnect"))
            > api.calls.indexOf(QStringLiteral("freeChannel")));
}

void TestSshWorker::connectingAgainTimerIsNotZeroMs()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    api.connectFallback = SSH_AGAIN;
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);

    worker.connectToHost(27, target(), 80, 24);
    QTRY_VERIFY(api.calls.count(QStringLiteral("connect")) >= 1);
    QTimer *timer = workerTimer(worker);
    QVERIFY(timer != nullptr);
    QVERIFY(timer->interval() > 0);
    QVERIFY(timer->interval() <= 50);

    const int before = api.calls.count(QStringLiteral("connect"));
    QTest::qWait(50);
    QVERIFY(api.calls.count(QStringLiteral("connect")) - before <= 15);
    worker.closeConnection(27);
}

void TestSshWorker::openingShellAgainTimerIsNotZeroMs()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.openChannelFallback = SSH_AGAIN;
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);

    worker.connectToHost(28, target(), 80, 24);
    QTRY_VERIFY(
        api.calls.count(QStringLiteral("openChannelSession")) >= 1);
    QTimer *timer = workerTimer(worker);
    QVERIFY(timer != nullptr);
    QVERIFY(timer->interval() > 0);
    QVERIFY(timer->interval() <= 50);

    const int before =
        api.calls.count(QStringLiteral("openChannelSession"));
    QTest::qWait(50);
    QVERIFY(api.calls.count(QStringLiteral("openChannelSession"))
            - before <= 15);
    worker.closeConnection(28);
}

void TestSshWorker::nonOwnerShellChannelCallsDoNotTouchHandles()
{
    FakeLibsshApi api;
    ssh_session session = api.newSession();
    ShellChannel channel(api, session, 80, 24);
    QCOMPARE(api.calls.count(QStringLiteral("newChannel")), 1);
    const int callsBefore = api.calls.size();
    const int eofBefore = api.channelEofCount;
    const int closeBefore = api.channelCloseCount;
    const int freeBefore = api.channelFreeCount;

    bool ran = false;
    QThread *thread = QThread::create([&] {
        QList<QByteArray> output;
        QString error;
        channel.advance(&output, &error);
        channel.queueInput(QByteArrayLiteral("x"));
        channel.resize(40, 12);
        channel.close();
        ran = true;
    });
    thread->start();
    QVERIFY(thread->wait(1000));
    delete thread;
    QVERIFY(ran);

    QCOMPARE(api.calls.size(), callsBefore);
    QCOMPARE(api.channelEofCount, eofBefore);
    QCOMPARE(api.channelCloseCount, closeBefore);
    QCOMPARE(api.channelFreeCount, freeBefore);
    QCOMPARE(api.writeCalls.size(), 0);
}

void TestSshWorker::pendingWriteDoesNotStarveOutputWithinTwoTicks()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.writeResults.fill(1, 100);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);
    QSignalSpy data(&worker, &SshWorker::shellData);

    worker.connectToHost(29, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    api.readEvents = {{false, QByteArrayLiteral("server")}};
    worker.writeShell(29, QByteArray(100, 'x'));

    QTRY_COMPARE(data.count(), 1);
    QVERIFY(api.acceptedWrites.size() < 100);
    QCOMPARE(data.first().at(1).toByteArray(),
             QByteArrayLiteral("server"));
}

void TestSshWorker::streamsStatsSamplesOnDedicatedChannel()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);
    QSignalSpy samples(&worker, &SshWorker::statsSample);
    QSignalSpy degraded(&worker, &SshWorker::statsDegraded);
    QSignalSpy failed(&worker, &SshWorker::connectionFailed);

    worker.connectToHost(31, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    // 先把 stats 通道的 open 钉在 SSH_AGAIN：通道建出来后挂好输出
    // 脚本再放行，避免 exec 先于脚本落地时 fake 立刻判 EOF 杀通道。
    api.openChannelFallback = SSH_AGAIN;
    worker.startStats(31);
    QTRY_VERIFY(api.createdChannels.size() >= 2);
    api.streamByChannel[api.createdChannels.at(1)] = {
        StatsChannel::kFrameBegin
            + QByteArrayLiteral("\ncpu  1 0 0 9 0\nMemTotal: 8 kB\n")
            + StatsChannel::kFrameEnd,
    };
    api.openChannelFallback = SSH_OK;

    QTRY_COMPARE(samples.count(), 1);
    QCOMPARE(samples.first().at(0).toULongLong(), quint64(31));
    QCOMPARE(samples.first().at(1).toString(),
             QStringLiteral("cpu  1 0 0 9 0\nMemTotal: 8 kB"));
    QCOMPARE(degraded.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void TestSshWorker::statsFailureDegradesButShellStaysReady()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);
    QSignalSpy degraded(&worker, &SshWorker::statsDegraded);
    QSignalSpy failed(&worker, &SshWorker::connectionFailed);
    QSignalSpy data(&worker, &SshWorker::shellData);

    worker.connectToHost(33, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    api.readEvents = {{false, QByteArrayLiteral("shell-alive")}};
    // shell 通道已开完，此后的 open 全部失败（stats 每次重试都碰壁）。
    api.openChannelFallback = SSH_ERROR;
    worker.startStats(33);

    QTRY_VERIFY(degraded.count() >= 1);
    QTRY_COMPARE(data.count(), 1);          // shell 数据照常流动
    QCOMPARE(data.first().at(1).toByteArray(),
             QByteArrayLiteral("shell-alive"));
    QCOMPARE(connected.count(), 1);         // 连接没有被 stats 拖垮
    QCOMPARE(failed.count(), 0);
}

QTEST_MAIN(TestSshWorker)
#include "test_sshworker.moc"
