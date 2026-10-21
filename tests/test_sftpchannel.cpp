#include "fakes/fakelibsshapi.h"
#include "ssh/knownhostsstore.h"
#include "ssh/sftpchannel.h"
#include "ssh/sftprequest.h"
#include "ssh/sshworker.h"
#include "sshsession.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {

SshTarget target()
{
    SshTarget value;
    value.host = QStringLiteral("sftp.test");
    value.user = QStringLiteral("tester");
    return value;
}

void prepareSuccessfulConnection(FakeLibsshApi &api)
{
    api.knownState = SSH_KNOWN_HOSTS_OK;
    api.agentResults = {SSH_AUTH_SUCCESS};
}

SftpRequest makeRequest(SshRequestId id, SftpOp op, const QString &path = {})
{
    SftpRequest request;
    request.requestId = id;
    request.op = op;
    request.path = path;
    return request;
}

QList<SftpResult> pump(SftpChannel &channel, int maxTicks = 80)
{
    QList<SftpResult> all;
    for (int i = 0; i < maxTicks; ++i) {
        QList<SftpResult> batch;
        const SftpChannel::Result result = channel.advance(&batch);
        all += batch;
        if (result == SftpChannel::Result::Idle
            || result == SftpChannel::Result::Error) {
            break;
        }
    }
    return all;
}

QList<SftpResult> ofKind(const QList<SftpResult> &results, SftpResult::Kind kind)
{
    QList<SftpResult> matched;
    for (const SftpResult &result : results) {
        if (result.kind == kind) {
            matched.append(result);
        }
    }
    return matched;
}

QTimer *workerTimer(SshWorker &worker)
{
    const QList<QTimer *> timers = worker.findChildren<QTimer *>();
    return timers.isEmpty() ? nullptr : timers.first();
}

int newSessionCount(const FakeLibsshApi &api)
{
    return int(api.calls.filter(QStringLiteral("newSession")).size());
}

QString writeTempFile(const QTemporaryDir &directory, const QString &name,
                      const QByteArray &bytes)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    file.write(bytes);
    return path;
}

} // namespace

class TestSftpChannel : public QObject
{
    Q_OBJECT

private slots:
    void doesNotCreateSftpUntilFirstRequest();
    void listsDirectoryAndResolvesRealPath();
    void mkdirCreateDeleteRenameAndChmod();
    void recursiveCopyUsesExecWithoutNewSshSession();
    void fileCopyUsesAio();
    void copyRetriesSourceCloseOnAgain();
    void execCommandReturnsOutputAndStatus();
    void uploadUsesAioAndReportsProgress();
    void downloadWritesTempFileThenRenames();
    void partialAioAndSshAgainResumeOnNextTick();
    void cancelDownloadDeletesTempFileExactlyOnce();
    void sftpFailureFreesAndRebuildsOnceOnNextRequest();
    void connectionLossFailsCurrentOpWithoutSecondSession();
    void successfulListCompletesExactlyOnce();
    void workerTransferDoesNotStarveShellReadEveryTick();
    void facadeForwardsSftpWithRequestId();
};

void TestSftpChannel::doesNotCreateSftpUntilFirstRequest()
{
    FakeLibsshApi api;
    SftpChannel channel(api, api.newSession());
    QCOMPARE(api.sftpNewCount, 0);
    QCOMPARE(api.sftpInitCount, 0);

    api.directoryEntries[QStringLiteral("/tmp")] = {
        {QStringLiteral("a.txt"), 4, 1, 0644, false, false}
    };
    channel.enqueue(makeRequest(1, SftpOp::List, QStringLiteral("/tmp")));
    const QList<SftpResult> results = pump(channel);

    QCOMPARE(api.sftpNewCount, 1);
    QCOMPARE(api.sftpInitCount, 1);
    QCOMPARE(newSessionCount(api), 1);
    QCOMPARE(ofKind(results, SftpResult::Kind::Listed).size(), 1);
}

void TestSftpChannel::listsDirectoryAndResolvesRealPath()
{
    FakeLibsshApi api;
    api.directoryEntries[QStringLiteral("/data")] = {
        {QStringLiteral("b.txt"), 10, 11, 0644, false, false},
        {QStringLiteral("a"), 0, 12, 0755, true, false}
    };
    api.realPaths[QStringLiteral(".")] = QStringLiteral("/home/tester");
    SftpChannel channel(api, api.newSession());

    channel.enqueue(makeRequest(3, SftpOp::List, QStringLiteral("/data")));
    channel.enqueue(makeRequest(4, SftpOp::RealPath, QStringLiteral(".")));
    const QList<SftpResult> results = pump(channel);

    const QList<SftpResult> listed = ofKind(results, SftpResult::Kind::Listed);
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed.first().requestId, SshRequestId(3));
    QCOMPARE(listed.first().path, QStringLiteral("/data"));
    QCOMPARE(listed.first().entries.size(), 2);
    QCOMPARE(listed.first().entries.at(0).name, QStringLiteral("a"));
    QVERIFY(listed.first().entries.at(0).isDir);
    QCOMPARE(listed.first().entries.at(1).name, QStringLiteral("b.txt"));

    const QList<SftpResult> ops = ofKind(results, SftpResult::Kind::OpFinished);
    QCOMPARE(ops.size(), 1);
    QCOMPARE(ops.first().requestId, SshRequestId(4));
    QCOMPARE(ops.first().op, QStringLiteral("realpath"));
    QCOMPARE(ops.first().arg, QStringLiteral("/home/tester"));
    QVERIFY(ops.first().ok);
}

void TestSftpChannel::mkdirCreateDeleteRenameAndChmod()
{
    FakeLibsshApi api;
    SftpChannel channel(api, api.newSession());

    channel.enqueue(makeRequest(10, SftpOp::Mkdir, QStringLiteral("/n")));
    SftpRequest created = makeRequest(11, SftpOp::Create, QStringLiteral("/n/f"));
    channel.enqueue(created);
    SftpRequest removed = makeRequest(12, SftpOp::Delete, QStringLiteral("/n/f"));
    removed.isDir = false;
    channel.enqueue(removed);
    SftpRequest renamed = makeRequest(13, SftpOp::Rename, QStringLiteral("/n"));
    renamed.dest = QStringLiteral("/m");
    channel.enqueue(renamed);
    SftpRequest chmod = makeRequest(14, SftpOp::Chmod, QStringLiteral("/m"));
    chmod.mode = 0700;
    channel.enqueue(chmod);

    const QList<SftpResult> ops =
        ofKind(pump(channel), SftpResult::Kind::OpFinished);
    QCOMPARE(ops.size(), 5);
    QCOMPARE(ops.at(0).op, QStringLiteral("mkdir"));
    QCOMPARE(ops.at(1).op, QStringLiteral("create"));
    QCOMPARE(ops.at(2).op, QStringLiteral("remove"));
    QCOMPARE(ops.at(3).op, QStringLiteral("rename"));
    QCOMPARE(ops.at(4).op, QStringLiteral("chmod"));
    for (const SftpResult &op : ops) {
        QVERIFY(op.ok);
    }
    QCOMPARE(api.mkdirPaths, QStringList({QStringLiteral("/n")}));
    QCOMPARE(api.createdPaths, QStringList({QStringLiteral("/n/f")}));
    QCOMPARE(api.unlinkedPaths, QStringList({QStringLiteral("/n/f")}));
    QCOMPARE(api.renames.size(), 1);
    QCOMPARE(api.renames.first().first, QStringLiteral("/n"));
    QCOMPARE(api.renames.first().second, QStringLiteral("/m"));
    QCOMPARE(api.chmodPaths, QStringList({QStringLiteral("/m")}));
    QCOMPARE(api.chmodModes, QList<quint32>({0700}));
}

void TestSftpChannel::recursiveCopyUsesExecWithoutNewSshSession()
{
    FakeLibsshApi api;
    api.execExitStatus = 0;
    SftpChannel channel(api, api.newSession());
    SftpRequest request = makeRequest(20, SftpOp::Copy, QStringLiteral("/src"));
    request.dest = QStringLiteral("/dst");
    request.isDir = true;

    channel.enqueue(request);
    const QList<SftpResult> ops =
        ofKind(pump(channel), SftpResult::Kind::OpFinished);

    QCOMPARE(ops.size(), 1);
    QVERIFY(ops.first().ok);
    QCOMPARE(ops.first().op, QStringLiteral("copy"));
    QCOMPARE(newSessionCount(api), 1);
    QCOMPARE(api.sftpNewCount, 1);
    QVERIFY(api.execCommands.size() >= 1);
    QVERIFY(QString::fromUtf8(api.execCommands.first())
                .contains(QStringLiteral("cp -a")));
}

void TestSftpChannel::fileCopyUsesAio()
{
    FakeLibsshApi api;
    api.remoteContents[QStringLiteral("/src.bin")] = QByteArray(64, 'C');
    SftpChannel channel(api, api.newSession());
    SftpRequest request = makeRequest(21, SftpOp::Copy, QStringLiteral("/src.bin"));
    request.dest = QStringLiteral("/dst.bin");
    request.isDir = false;

    channel.enqueue(request);
    const QList<SftpResult> ops =
        ofKind(pump(channel), SftpResult::Kind::OpFinished);

    QCOMPARE(ops.size(), 1);
    QVERIFY(ops.first().ok);
    QCOMPARE(api.remoteContents.value(QStringLiteral("/dst.bin")),
             QByteArray(64, 'C'));
    QVERIFY(api.aioBeginReadCount > 0);
    QVERIFY(api.aioWaitReadCount > 0);
    QVERIFY(api.aioBeginWriteCount > 0);
    QVERIFY(api.aioWaitWriteCount > 0);
}

void TestSftpChannel::copyRetriesSourceCloseOnAgain()
{
    FakeLibsshApi api;
    api.remoteContents[QStringLiteral("/s.bin")] = QByteArray(40, 'K');
    api.closeFileResults = {SSH_OK, SSH_AGAIN};
    SftpChannel channel(api, api.newSession());
    SftpRequest request = makeRequest(23, SftpOp::Copy, QStringLiteral("/s.bin"));
    request.dest = QStringLiteral("/d.bin");
    request.isDir = false;

    channel.enqueue(request);
    QList<SftpResult> mid;
    QCOMPARE(channel.advance(&mid), SftpChannel::Result::Again);
    QCOMPARE(ofKind(mid, SftpResult::Kind::OpFinished).size(), 0);

    const QList<SftpResult> ops =
        ofKind(pump(channel), SftpResult::Kind::OpFinished);
    QCOMPARE(ops.size(), 1);
    QVERIFY(ops.first().ok);
    QCOMPARE(api.sftpCloseFileCount, 3);
    QCOMPARE(api.remoteContents.value(QStringLiteral("/d.bin")),
             QByteArray(40, 'K'));
}

void TestSftpChannel::execCommandReturnsOutputAndStatus()
{
    FakeLibsshApi api;
    api.execStdout = QByteArrayLiteral("hello");
    api.execExitStatus = 0;
    SftpChannel channel(api, api.newSession());
    SftpRequest request = makeRequest(22, SftpOp::Exec);
    request.command = QStringLiteral("echo hello");

    channel.enqueue(request);
    const QList<SftpResult> ops =
        ofKind(pump(channel), SftpResult::Kind::OpFinished);

    QCOMPARE(ops.size(), 1);
    QVERIFY(ops.first().ok);
    QCOMPARE(ops.first().op, QStringLiteral("exec"));
    QCOMPARE(ops.first().error, QStringLiteral("hello"));
    QCOMPARE(api.execCommands,
             QList<QByteArray>({QByteArrayLiteral("echo hello")}));
}

void TestSftpChannel::uploadUsesAioAndReportsProgress()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray payload(1200, 'U');
    const QString local = writeTempFile(directory, QStringLiteral("up.bin"),
                                        payload);
    QVERIFY(!local.isEmpty());
    FakeLibsshApi api;
    SftpChannel channel(api, api.newSession());
    SftpRequest request = makeRequest(30, SftpOp::Upload);
    request.localPath = local;
    request.remotePath = QStringLiteral("/up.bin");

    channel.enqueue(request);
    const QList<SftpResult> results = pump(channel);
    const QList<SftpResult> progress =
        ofKind(results, SftpResult::Kind::Progress);
    const QList<SftpResult> finished =
        ofKind(results, SftpResult::Kind::TransferFinished);

    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last().done, qint64(payload.size()));
    QCOMPARE(progress.last().total, qint64(payload.size()));
    QCOMPARE(finished.size(), 1);
    QVERIFY(finished.first().ok);
    QCOMPARE(finished.first().requestId, SshRequestId(30));
    QCOMPARE(api.remoteContents.value(QStringLiteral("/up.bin")), payload);
    QVERIFY(api.aioBeginWriteCount > 0);
    QVERIFY(api.aioWaitWriteCount > 0);
}

void TestSftpChannel::downloadWritesTempFileThenRenames()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString dest = directory.filePath(QStringLiteral("down.bin"));
    const QString part = dest + QStringLiteral(".part");
    FakeLibsshApi api;
    api.remoteContents[QStringLiteral("/down.bin")] = QByteArray(80, 'D');
    api.aioWaitReadResults = {SSH_AGAIN};
    SftpChannel channel(api, api.newSession());
    SftpRequest request = makeRequest(31, SftpOp::Download);
    request.remotePath = QStringLiteral("/down.bin");
    request.localPath = dest;

    channel.enqueue(request);
    QList<SftpResult> mid;
    QCOMPARE(channel.advance(&mid), SftpChannel::Result::Again);
    QVERIFY(QFile::exists(part));
    QVERIFY(!QFile::exists(dest));

    api.aioWaitReadResults.clear();
    const QList<SftpResult> finished =
        ofKind(pump(channel), SftpResult::Kind::TransferFinished);
    QCOMPARE(finished.size(), 1);
    QVERIFY(finished.first().ok);
    QVERIFY(QFile::exists(dest));
    QVERIFY(!QFile::exists(part));
    QFile out(dest);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), QByteArray(80, 'D'));
}

void TestSftpChannel::partialAioAndSshAgainResumeOnNextTick()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray payload(400, 'P');
    const QString local = writeTempFile(directory, QStringLiteral("part.bin"),
                                        payload);
    FakeLibsshApi api;
    api.aioBeginWriteResults = {200, SSH_AGAIN, 200};
    api.aioWaitWriteResults = {SSH_AGAIN, 200, 200};
    SftpChannel channel(api, api.newSession());
    SftpRequest request = makeRequest(32, SftpOp::Upload);
    request.localPath = local;
    request.remotePath = QStringLiteral("/part.bin");

    channel.enqueue(request);
    QList<SftpResult> first;
    QCOMPARE(channel.advance(&first), SftpChannel::Result::Again);
    QCOMPARE(ofKind(first, SftpResult::Kind::TransferFinished).size(), 0);

    const QList<SftpResult> finished =
        ofKind(pump(channel), SftpResult::Kind::TransferFinished);
    QCOMPARE(finished.size(), 1);
    QVERIFY(finished.first().ok);
    QCOMPARE(api.remoteContents.value(QStringLiteral("/part.bin")), payload);
}

void TestSftpChannel::cancelDownloadDeletesTempFileExactlyOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString dest = directory.filePath(QStringLiteral("cancel.bin"));
    const QString part = dest + QStringLiteral(".part");
    FakeLibsshApi api;
    api.remoteContents[QStringLiteral("/cancel.bin")] = QByteArray(200, 'X');
    api.aioWaitReadResults.fill(SSH_AGAIN, 20);
    SftpChannel channel(api, api.newSession());
    SftpRequest request = makeRequest(33, SftpOp::Download);
    request.remotePath = QStringLiteral("/cancel.bin");
    request.localPath = dest;

    channel.enqueue(request);
    QList<SftpResult> mid;
    channel.advance(&mid);
    QVERIFY(QFile::exists(part));
    channel.cancel(33);
    channel.cancel(33);
    const QList<SftpResult> finished =
        ofKind(pump(channel), SftpResult::Kind::TransferFinished);

    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished.first().ok);
    QCOMPARE(finished.first().error, QStringLiteral("已取消"));
    QVERIFY(!QFile::exists(part));
    QVERIFY(!QFile::exists(dest));
}

void TestSftpChannel::sftpFailureFreesAndRebuildsOnceOnNextRequest()
{
    FakeLibsshApi api;
    api.sftpInitResults = {SSH_ERROR, SSH_OK};
    api.directoryEntries[QStringLiteral("/ok")] = {
        {QStringLiteral("f"), 1, 1, 0644, false, false}
    };
    SftpChannel channel(api, api.newSession());

    channel.enqueue(makeRequest(40, SftpOp::List, QStringLiteral("/ok")));
    const QList<SftpResult> first = pump(channel);
    QCOMPARE(ofKind(first, SftpResult::Kind::ListedFailed).size(), 1);
    QCOMPARE(api.sftpNewCount, 1);
    QCOMPARE(api.sftpInitCount, 1);
    QCOMPARE(api.sftpFreeCount, 1);

    channel.enqueue(makeRequest(41, SftpOp::List, QStringLiteral("/ok")));
    const QList<SftpResult> second = pump(channel);
    QCOMPARE(ofKind(second, SftpResult::Kind::Listed).size(), 1);
    QCOMPARE(api.sftpNewCount, 2);
    QCOMPARE(api.sftpInitCount, 2);
    QCOMPARE(newSessionCount(api), 1);
}

void TestSftpChannel::connectionLossFailsCurrentOpWithoutSecondSession()
{
    FakeLibsshApi api;
    api.opendirResults = {nullptr};
    api.errorText = QStringLiteral("connection lost");
    SftpChannel channel(api, api.newSession());

    channel.enqueue(makeRequest(50, SftpOp::List, QStringLiteral("/gone")));
    const QList<SftpResult> results = pump(channel);
    const QList<SftpResult> failed =
        ofKind(results, SftpResult::Kind::ListedFailed);

    QCOMPARE(failed.size(), 1);
    QVERIFY(failed.first().error.contains(QStringLiteral("connection lost")));
    QCOMPARE(api.sftpFreeCount, 1);
    QCOMPARE(newSessionCount(api), 1);
}

void TestSftpChannel::successfulListCompletesExactlyOnce()
{
    FakeLibsshApi api;
    api.directoryEntries[QStringLiteral("/once")] = {};
    SftpChannel channel(api, api.newSession());
    channel.enqueue(makeRequest(60, SftpOp::List, QStringLiteral("/once")));

    const QList<SftpResult> first = pump(channel);
    QCOMPARE(ofKind(first, SftpResult::Kind::Listed).size(), 1);
    QCOMPARE(ofKind(first, SftpResult::Kind::ListedFailed).size(), 0);

    QList<SftpResult> extra;
    QCOMPARE(channel.advance(&extra), SftpChannel::Result::Idle);
    QVERIFY(extra.isEmpty());
}

void TestSftpChannel::workerTransferDoesNotStarveShellReadEveryTick()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray payload(SftpChannel::TickByteBudget * 8, 'W');
    const QString local = writeTempFile(directory, QStringLiteral("big.bin"),
                                        payload);
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshWorker worker(api, store);
    QSignalSpy connected(&worker, &SshWorker::connected);
    QSignalSpy finished(&worker, &SshWorker::sftpResult);

    worker.connectToHost(70, target(), 80, 24);
    QTRY_COMPARE(connected.count(), 1);
    QCOMPARE(newSessionCount(api), 1);

    SftpRequest request = makeRequest(71, SftpOp::Upload);
    request.localPath = local;
    request.remotePath = QStringLiteral("/big.bin");
    worker.submitSftpRequest(70, request);

    QTimer *timer = workerTimer(worker);
    QVERIFY(timer != nullptr);
    int ticks = 0;
    int ticksWithShellRead = 0;
    int lastReads = api.readCallCount;
    QObject::connect(timer, &QTimer::timeout, this, [&] {
        ++ticks;
        if (api.readCallCount > lastReads) {
            ++ticksWithShellRead;
        }
        lastReads = api.readCallCount;
    });

    QTRY_VERIFY(api.aioBeginWriteCount > 0);
    QTest::qWait(40);
    QVERIFY(ticks >= 3);
    QCOMPARE(ticksWithShellRead, ticks);
    QCOMPARE(newSessionCount(api), 1);

    QTRY_VERIFY([&] {
        for (const QList<QVariant> &event : finished) {
            const SftpResult result = event.at(1).value<SftpResult>();
            if (result.kind == SftpResult::Kind::TransferFinished
                && result.requestId == 71 && result.ok) {
                return true;
            }
        }
        return false;
    }());
}

void TestSftpChannel::facadeForwardsSftpWithRequestId()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.directoryEntries[QStringLiteral("/home")] = {
        {QStringLiteral("notes"), 2, 1, 0644, false, false}
    };
    KnownHostsStore store(api, directory.path());
    SshSession session(api, store);
    QSignalSpy opened(&session, &SshSession::opened);
    QSignalSpy listed(&session, &SshSession::sftpListed);
    QSignalSpy listedFailed(&session, &SshSession::sftpListedFailed);

    QCOMPARE(session.open(target()), QString());
    QTRY_COMPARE(opened.count(), 1);

    session.listDir(80, QStringLiteral("/home"));
    QTRY_COMPARE(listed.count(), 1);
    QCOMPARE(listed.first().at(0).value<SshRequestId>(), SshRequestId(80));
    QCOMPARE(listed.first().at(1).toString(), QStringLiteral("/home"));
    QCOMPARE(listed.first().at(2).value<QVector<SftpEntry>>().size(), 1);
    QCOMPARE(listedFailed.count(), 0);
    QCOMPARE(newSessionCount(api), 1);

    session.handleWorkerSftpResult(
        0, {SftpResult::Kind::Listed, 80, {}, {},
            QStringLiteral("/stale"), {}, 0, 0, true, {}});
    QCOMPARE(listed.count(), 1);
}

QTEST_MAIN(TestSftpChannel)
#include "test_sftpchannel.moc"
