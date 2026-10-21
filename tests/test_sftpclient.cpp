#include "fakes/fakelibsshapi.h"
#include "sftpclient.h"
#include "ssh/knownhostsstore.h"
#include "sshsession.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
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

int newSessionCount(const FakeLibsshApi &api)
{
    return int(api.calls.filter(QStringLiteral("newSession")).size());
}

} // namespace

class TestSftpClient : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void offlineSubmissionsFailImmediately();
    void forwardsListRealPathAndResults();
    void forwardsOperationsWithOpNames();
    void uploadMapsIntIdProgressAndFinish();
    void cancelTransferCancelsActiveDownload();
    void destructionCancelsOwnRequestsAndKeepsSession();
    void ignoresResultsFromOtherAdapters();

private:
    void connectSession();

    QTemporaryDir *m_dir = nullptr;
    FakeLibsshApi *m_api = nullptr;
    KnownHostsStore *m_store = nullptr;
    SshSession *m_session = nullptr;
    SftpClient *m_client = nullptr;
};

void TestSftpClient::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
    m_api = new FakeLibsshApi;
    prepareSuccessfulConnection(*m_api);
    m_store = new KnownHostsStore(*m_api, m_dir->path());
    m_session = new SshSession(*m_api, *m_store);
    m_client = new SftpClient(m_session);
}

void TestSftpClient::cleanup()
{
    delete m_client;
    delete m_session;
    delete m_store;
    delete m_api;
    delete m_dir;
}

void TestSftpClient::connectSession()
{
    QSignalSpy opened(m_session, &SshSession::opened);
    QCOMPARE(m_session->open(target()), QString());
    QTRY_COMPARE(opened.count(), 1);
    QCOMPARE(m_session->state(), Transport::Connected);
}

void TestSftpClient::offlineSubmissionsFailImmediately()
{
    // 会话未连接：所有槽同步给失败结果，worker 上没有任何动作。
    QSignalSpy listedFailed(m_client, &SftpClient::listedFailed);
    QSignalSpy transfers(m_client, &SftpClient::transferFinished);
    QSignalSpy ops(m_client, &SftpClient::opFinished);

    m_client->listDir(QStringLiteral("/a"));
    m_client->realPath(QStringLiteral("/b"));
    m_client->uploadFile(3, QStringLiteral("l"), QStringLiteral("r"));
    m_client->downloadFile(4, QStringLiteral("r"), QStringLiteral("l"));
    m_client->makeDir(QStringLiteral("/n"));
    m_client->createFile(QStringLiteral("/n/f"));
    m_client->removeEntry(QStringLiteral("/n/f"), false);
    m_client->renameEntry(QStringLiteral("/n"), QStringLiteral("/m"));
    m_client->copyRemote(QStringLiteral("/n"), QStringLiteral("/c"), true);
    m_client->setPermissions(QStringLiteral("/m"), 0700);
    m_client->execCommand(QStringLiteral("true"));

    QCOMPARE(listedFailed.count(), 1);
    QCOMPARE(listedFailed.first().at(0).toString(), QStringLiteral("/a"));
    QCOMPARE(transfers.count(), 2);
    QCOMPARE(transfers.at(0).at(0).toInt(), 3);
    QCOMPARE(transfers.at(1).at(0).toInt(), 4);
    QVERIFY(!transfers.at(0).at(1).toBool());
    QVERIFY(!transfers.at(1).at(1).toBool());
    QCOMPARE(ops.count(), 8);
    QCOMPARE(ops.at(0).at(0).toString(), QStringLiteral("realpath"));
    QCOMPARE(ops.at(1).at(0).toString(), QStringLiteral("mkdir"));
    QCOMPARE(ops.at(2).at(0).toString(), QStringLiteral("create"));
    QCOMPARE(ops.at(3).at(0).toString(), QStringLiteral("remove"));
    QCOMPARE(ops.at(4).at(0).toString(), QStringLiteral("rename"));
    QCOMPARE(ops.at(5).at(0).toString(), QStringLiteral("copy"));
    QCOMPARE(ops.at(6).at(0).toString(), QStringLiteral("chmod"));
    QCOMPARE(ops.at(7).at(0).toString(), QStringLiteral("exec"));
    QCOMPARE(ops.at(7).at(1).toString(), QStringLiteral("true"));
    for (const QList<QVariant> &event : ops)
        QVERIFY(!event.at(2).toBool());
    QCOMPARE(m_api->sftpNewChannelCount, 0);
    QCOMPARE(newSessionCount(*m_api), 0);
}

void TestSftpClient::forwardsListRealPathAndResults()
{
    connectSession();
    m_api->directoryEntries[QStringLiteral("/home")] = {
        {QStringLiteral("b.txt"), 10, 11, 0644, false, false},
        {QStringLiteral("a"), 0, 12, 0755, true, false}
    };
    m_api->realPaths[QStringLiteral(".")] = QStringLiteral("/home/tester");

    QSignalSpy listed(m_client, &SftpClient::listed);
    QSignalSpy ops(m_client, &SftpClient::opFinished);

    m_client->listDir(QStringLiteral("/home"));
    QTRY_COMPARE(listed.count(), 1);
    QCOMPARE(listed.first().at(0).toString(), QStringLiteral("/home"));
    const QVector<SftpEntry> entries =
        listed.first().at(1).value<QVector<SftpEntry>>();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).name, QStringLiteral("a")); // 目录排前
    QVERIFY(entries.at(0).isDir);
    QCOMPARE(entries.at(1).name, QStringLiteral("b.txt"));

    m_client->realPath(QStringLiteral("."));
    QTRY_COMPARE(ops.count(), 1);
    QCOMPARE(ops.first().at(0).toString(), QStringLiteral("realpath"));
    QCOMPARE(ops.first().at(1).toString(), QStringLiteral("/home/tester"));
    QVERIFY(ops.first().at(2).toBool());
    QCOMPARE(newSessionCount(*m_api), 1);
}

void TestSftpClient::forwardsOperationsWithOpNames()
{
    connectSession();
    QSignalSpy ops(m_client, &SftpClient::opFinished);

    m_client->makeDir(QStringLiteral("/n"));
    m_client->createFile(QStringLiteral("/n/f"));
    m_client->removeEntry(QStringLiteral("/n/f"), false);
    m_client->renameEntry(QStringLiteral("/n"), QStringLiteral("/m"));
    m_client->setPermissions(QStringLiteral("/m"), 0700);

    QTRY_COMPARE(ops.count(), 5);
    for (const QList<QVariant> &event : ops)
        QVERIFY(event.at(2).toBool());
    QCOMPARE(m_api->mkdirPaths, QStringList({QStringLiteral("/n")}));
    QCOMPARE(m_api->createdPaths, QStringList({QStringLiteral("/n/f")}));
    QCOMPARE(m_api->unlinkedPaths, QStringList({QStringLiteral("/n/f")}));
    QCOMPARE(m_api->renames.size(), 1);
    QCOMPARE(m_api->chmodPaths, QStringList({QStringLiteral("/m")}));
    QCOMPARE(m_api->chmodModes, QList<quint32>({0700}));
}

void TestSftpClient::uploadMapsIntIdProgressAndFinish()
{
    connectSession();
    const QByteArray payload(1200, 'U');
    const QString local = writeTempFile(*m_dir, QStringLiteral("up.bin"),
                                        payload);
    QVERIFY(!local.isEmpty());

    QSignalSpy progress(m_client, &SftpClient::transferProgress);
    QSignalSpy finished(m_client, &SftpClient::transferFinished);

    m_client->uploadFile(7, local, QStringLiteral("/up.bin"));
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toInt(), 7);
    QVERIFY(finished.first().at(1).toBool());
    QVERIFY(progress.count() > 0);
    for (const QList<QVariant> &event : progress)
        QCOMPARE(event.at(0).toInt(), 7);
    QCOMPARE(progress.last().at(1).toLongLong(), qint64(payload.size()));
    QCOMPARE(m_api->remoteContents.value(QStringLiteral("/up.bin")), payload);
}

void TestSftpClient::cancelTransferCancelsActiveDownload()
{
    connectSession();
    m_api->remoteContents[QStringLiteral("/big.bin")] = QByteArray(200, 'X');
    m_api->aioWaitReadResults.fill(SSH_AGAIN, 100);
    const QString dest = m_dir->filePath(QStringLiteral("cancel.bin"));
    const QString part = dest + QStringLiteral(".part");

    QSignalSpy finished(m_client, &SftpClient::transferFinished);
    m_client->downloadFile(9, QStringLiteral("/big.bin"), dest);
    QTRY_VERIFY(QFile::exists(part));
    m_client->cancelTransfer(9);
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toInt(), 9);
    QVERIFY(!finished.first().at(1).toBool());
    QCOMPARE(finished.first().at(2).toString(), QStringLiteral("已取消"));
    QVERIFY(!QFile::exists(part));
    QVERIFY(!QFile::exists(dest));
    QCOMPARE(m_session->state(), Transport::Connected);
}

void TestSftpClient::destructionCancelsOwnRequestsAndKeepsSession()
{
    connectSession();
    m_api->remoteContents[QStringLiteral("/slow.bin")] = QByteArray(200, 'X');
    m_api->aioWaitReadResults.fill(SSH_AGAIN, 100);
    const QString dest = m_dir->filePath(QStringLiteral("slow.bin"));
    const QString part = dest + QStringLiteral(".part");

    // 一个卡住的下载（活动）+ 一个排队的 mkdir，销毁适配器时都要取消。
    m_client->downloadFile(11, QStringLiteral("/slow.bin"), dest);
    QTRY_VERIFY(QFile::exists(part));
    m_client->makeDir(QStringLiteral("/later"));

    QSignalSpy transferDone(m_session, &SshSession::sftpTransferFinished);
    QSignalSpy opDone(m_session, &SshSession::sftpOpFinished);
    delete m_client;
    m_client = nullptr;

    QTRY_COMPARE(transferDone.count(), 1);
    QVERIFY(!transferDone.first().at(1).toBool());
    QCOMPARE(transferDone.first().at(2).toString(), QStringLiteral("已取消"));
    QTRY_COMPARE(opDone.count(), 1);
    QCOMPARE(opDone.first().at(1).toString(), QStringLiteral("mkdir"));
    QVERIFY(!opDone.first().at(3).toBool());

    // 会话与 shell 原样保留：仍是同一条连接，没有断开。
    QCOMPARE(m_session->state(), Transport::Connected);
    QCOMPARE(newSessionCount(*m_api), 1);
    QCOMPARE(m_api->disconnectCount, 0);
    QCOMPARE(m_session->send(QByteArrayLiteral("echo alive\n")), QString());
}

void TestSftpClient::ignoresResultsFromOtherAdapters()
{
    connectSession();
    m_api->directoryEntries[QStringLiteral("/home")] = {
        {QStringLiteral("a"), 1, 1, 0644, false, false}
    };
    SftpClient other(m_session);
    QSignalSpy otherListed(&other, &SftpClient::listed);
    QSignalSpy otherFailed(&other, &SftpClient::listedFailed);
    QSignalSpy otherOps(&other, &SftpClient::opFinished);
    QSignalSpy listed(m_client, &SftpClient::listed);

    m_client->listDir(QStringLiteral("/home"));
    QTRY_COMPARE(listed.count(), 1);
    QCOMPARE(otherListed.count(), 0);
    QCOMPARE(otherFailed.count(), 0);
    QCOMPARE(otherOps.count(), 0);
}

QTEST_MAIN(TestSftpClient)
#include "test_sftpclient.moc"
