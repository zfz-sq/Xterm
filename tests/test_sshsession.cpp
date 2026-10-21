#include "fakes/fakelibsshapi.h"
#include "ssh/knownhostsstore.h"
#include "sshsession.h"
#include "transport.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

SshTarget target()
{
    SshTarget value;
    value.host = QStringLiteral("facade.test");
    value.user = QStringLiteral("tester");
    return value;
}

void prepareSuccessfulConnection(FakeLibsshApi &api)
{
    api.knownState = SSH_KNOWN_HOSTS_OK;
    api.agentResults = {SSH_AUTH_SUCCESS};
}

int newSessionCount(const FakeLibsshApi &api)
{
    return int(api.calls.filter(QStringLiteral("newSession")).size());
}

} // namespace

class TestSshSession : public QObject
{
    Q_OBJECT

private slots:
    void openReturnsImmediatelyAndEntersConnecting();
    void workerConnectedMapsToExactlyOneOpened();
    void rawWorkerBytesMapUnchangedToReceived();
    void closeIsIdempotent();
    void reopenWaitsForOldGenerationToClose();
    void staleWorkerEventsDoNotAffectNewSession();
};

void TestSshSession::openReturnsImmediatelyAndEntersConnecting()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    SshSession session(api, store);

    QCOMPARE(session.state(), Transport::Disconnected);
    QCOMPARE(session.open(target()), QString());
    QCOMPARE(session.state(), Transport::Connecting);
}

void TestSshSession::workerConnectedMapsToExactlyOneOpened()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshSession session(api, store);
    QSignalSpy opened(&session, &SshSession::opened);

    QCOMPARE(session.open(target()), QString());
    QTRY_COMPARE(opened.count(), 1);
    QCOMPARE(session.state(), Transport::Connected);

    session.handleWorkerConnected(1);
    QCOMPARE(opened.count(), 1);
}

void TestSshSession::rawWorkerBytesMapUnchangedToReceived()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    const QByteArray raw =
        QByteArray("\x1b[2Jtitle:\x1b]0;raw\x07", 18)
        + QByteArray::fromHex("e4b8ad00")
        + QByteArrayLiteral("\x1b[31m");
    api.readEvents = {{false, raw}};
    KnownHostsStore store(api, directory.path());
    SshSession session(api, store);
    QSignalSpy received(&session, &SshSession::received);

    QCOMPARE(session.open(target()), QString());
    QTRY_VERIFY(!received.isEmpty());

    QByteArray bytes;
    for (const QList<QVariant> &event : received)
        bytes += event.at(0).toByteArray();
    QCOMPARE(bytes, raw);
}

void TestSshSession::closeIsIdempotent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    KnownHostsStore store(api, directory.path());
    SshSession session(api, store);
    QSignalSpy closed(&session, &SshSession::closed);

    session.close();
    session.close();
    QCOMPARE(session.state(), Transport::Disconnected);
    QCOMPARE(closed.count(), 0);

    QCOMPARE(session.open(target()), QString());
    QTRY_COMPARE(session.state(), Transport::Connected);
    session.close();
    session.close();
    session.close();
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(session.state(), Transport::Disconnected);
}

void TestSshSession::reopenWaitsForOldGenerationToClose()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.agentResults = {SSH_AUTH_SUCCESS, SSH_AUTH_SUCCESS};
    api.sendEofResults = {SSH_AGAIN, SSH_AGAIN, SSH_AGAIN, SSH_AGAIN, SSH_OK};
    api.closeChannelResults = {SSH_AGAIN, SSH_AGAIN, SSH_OK};
    KnownHostsStore store(api, directory.path());
    SshSession session(api, store);
    QSignalSpy opened(&session, &SshSession::opened);
    QSignalSpy closed(&session, &SshSession::closed);

    QCOMPARE(session.open(target()), QString());
    QTRY_COMPARE(opened.count(), 1);
    const int sessionsAfterFirst = newSessionCount(api);

    SshTarget next = target();
    next.host = QStringLiteral("other.test");
    QCOMPARE(session.open(next), QString());
    QCOMPARE(session.state(), Transport::Disconnecting);
    QTRY_VERIFY(api.channelEofCount >= 1);
    QCOMPARE(opened.count(), 1);
    QCOMPARE(closed.count(), 0);
    QCOMPARE(newSessionCount(api), sessionsAfterFirst);

    QTRY_COMPARE(opened.count(), 2);
    QCOMPARE(closed.count(), 1);
    QCOMPARE(session.target().host, QStringLiteral("other.test"));
    QCOMPARE(session.state(), Transport::Connected);
}

void TestSshSession::staleWorkerEventsDoNotAffectNewSession()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    prepareSuccessfulConnection(api);
    api.agentResults = {SSH_AUTH_SUCCESS, SSH_AUTH_SUCCESS};
    KnownHostsStore store(api, directory.path());
    SshSession session(api, store);
    QSignalSpy opened(&session, &SshSession::opened);
    QSignalSpy closed(&session, &SshSession::closed);
    QSignalSpy received(&session, &SshSession::received);

    QCOMPARE(session.open(target()), QString());
    QTRY_COMPARE(opened.count(), 1);

    SshTarget next = target();
    next.host = QStringLiteral("fresh.test");
    QCOMPARE(session.open(next), QString());
    QTRY_COMPARE(opened.count(), 2);
    QCOMPARE(session.state(), Transport::Connected);

    session.handleWorkerConnected(1);
    session.handleWorkerShellData(1, QByteArrayLiteral("stale-bytes"));
    session.handleWorkerClosed(1);

    QCOMPARE(opened.count(), 2);
    QCOMPARE(received.count(), 0);
    QCOMPARE(session.state(), Transport::Connected);
    QCOMPARE(closed.count(), 1);
}

QTEST_MAIN(TestSshSession)
#include "test_sshsession.moc"
