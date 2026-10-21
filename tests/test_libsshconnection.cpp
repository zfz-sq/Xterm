#include "fakes/fakelibsshapi.h"
#include "ssh/knownhostsstore.h"
#include "ssh/libsshconnection.h"

#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

SshTarget target()
{
    SshTarget value;
    value.host = QStringLiteral("alias.test");
    value.port = 2200;
    value.user = QStringLiteral("ui-user");
    value.password = QStringLiteral("secret");
    value.privateKey = QStringLiteral("C:/keys/ui-key");
    value.connectTimeoutSec = 17;
    return value;
}

void appendSshString(QByteArray *blob, const QByteArray &value)
{
    const quint32 size = quint32(value.size());
    blob->append(char((size >> 24) & 0xff));
    blob->append(char((size >> 16) & 0xff));
    blob->append(char((size >> 8) & 0xff));
    blob->append(char(size & 0xff));
    blob->append(value);
}

QByteArray ed25519Blob()
{
    QByteArray blob;
    appendSshString(&blob, QByteArrayLiteral("ssh-ed25519"));
    appendSshString(&blob, QByteArray(32, 'm'));
    return blob;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

void advanceUntilFinished(LibsshConnection &connection, QSignalSpy &finished)
{
    for (int i = 0; i < 20 && finished.isEmpty(); ++i) {
        connection.advance();
    }
}

} // namespace

class TestLibsshConnection : public QObject
{
    Q_OBJECT

private slots:
    void explicitUiOptionsOverrideParsedConfig();
    void unknownHostUsesTofuAndWritesFullKey();
    void matchingLegacyFingerprintMigratesToFullKey();
    void changedHostKeyWaitsForDecision();
    void rejectedChangedKeyDisconnects();
    void triesAgentThenExplicitKeyThenAutoKeyThenKeyboardInteractiveThenPassword();
    void explicitKeyLocalErrorStopsAuthentication();
    void keyboardInteractivePublishesAllPromptsAndResumes();
    void keyboardInteractiveZeroPromptsContinuesWithoutUi();
    void changedKnownHostReadsOldFingerprintFromPrivateStore();
    void knownLegacyMatchRecoversInterruptedConsumption();
    void cancellationCompletesOnce();
    void reentrantBeginCancelsPreviousOperationOnce();
    void keepaliveFailureBecomesRetryableNetworkError();
};

void TestLibsshConnection::explicitUiOptionsOverrideParsedConfig()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.connectResults = {SSH_AGAIN, SSH_OK};
    api.agentResults = {SSH_AUTH_SUCCESS};
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    connection.advance();

    const int hostIndex = api.calls.indexOf(
        QStringLiteral("string:%1=alias.test").arg(int(SSH_OPTIONS_HOST)));
    const int configIndex = api.calls.indexOf(QStringLiteral("parseConfig"));
    const int userIndex = api.calls.indexOf(
        QStringLiteral("string:%1=ui-user").arg(int(SSH_OPTIONS_USER)));
    QVERIFY(hostIndex >= 0);
    QVERIFY(configIndex > hostIndex);
    QVERIFY(userIndex > configIndex);
    QCOMPARE(api.stringOptions.value(SSH_OPTIONS_IDENTITY),
             QStringLiteral("C:/keys/ui-key"));
    QCOMPARE(api.unsignedOptions.value(SSH_OPTIONS_PORT), 2200u);
    QCOMPARE(api.unsignedOptions.value(SSH_OPTIONS_TIMEOUT), 17u);
    QCOMPARE(api.stringOptions.value(SSH_OPTIONS_KNOWNHOSTS), store.path());
    QCOMPARE(api.stringOptions.value(SSH_OPTIONS_GLOBAL_KNOWNHOSTS),
             store.globalPath());
    QVERIFY(api.boolOptions.value(SSH_OPTIONS_STRICTHOSTKEYCHECK));
    QVERIFY(!api.boolOptions.value(SSH_OPTIONS_PROCESS_CONFIG));
    QVERIFY(QFile::exists(store.globalPath()));
    QCOMPARE(QFile(store.globalPath()).size(), qint64(0));
    QVERIFY(api.calls.contains(QStringLiteral("blocking:0")));

    connection.advance();
    QCOMPARE(api.calls.count(QStringLiteral("connect")), 1);
    QVERIFY(finished.isEmpty());
    connection.advance();
    QCOMPARE(api.calls.count(QStringLiteral("connect")), 2);
}

void TestLibsshConnection::unknownHostUsesTofuAndWritesFullKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.knownState = SSH_KNOWN_HOSTS_UNKNOWN;
    api.agentResults = {SSH_AUTH_SUCCESS};
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    advanceUntilFinished(connection, finished);

    QCOMPARE(api.updateKnownHostsCount, 1);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!qvariant_cast<SshError>(finished.first().first()).isError());
}

void TestLibsshConnection::matchingLegacyFingerprintMigratesToFullKey()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    QVERIFY(store.prepareMigration());
    const QByteArray blob = ed25519Blob();
    const QByteArray fingerprint =
        QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex();
    QVERIFY(writeFile(store.legacyPath(),
                      "alias.test:2200 " + fingerprint + "\n"));
    QVERIFY(writeFile(store.path(),
                      "[alias.test]:2200 ssh-ed25519 " + blob.toBase64() + "\n"));

    api.knownState = SSH_KNOWN_HOSTS_UNKNOWN;
    api.serverFingerprint = QString::fromLatin1(fingerprint);
    api.agentResults = {SSH_AUTH_SUCCESS};
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    advanceUntilFinished(connection, finished);

    QCOMPARE(api.updateKnownHostsCount, 1);
    QCOMPARE(QFile(store.legacyPath()).size(), qint64(0));
    QCOMPARE(finished.count(), 1);
}

void TestLibsshConnection::changedHostKeyWaitsForDecision()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.knownState = SSH_KNOWN_HOSTS_UNKNOWN;
    const QString oldFingerprint(64, QLatin1Char('a'));
    QVERIFY(writeFile(store.legacyPath(),
                      "alias.test:2200 " + oldFingerprint.toLatin1()
                          + "\n"));
    api.agentResults = {SSH_AUTH_SUCCESS};
    LibsshConnection connection(api, store);
    QSignalSpy decision(&connection,
                        &LibsshConnection::hostKeyDecisionRequired);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    for (int i = 0; i < 10 && decision.isEmpty(); ++i) {
        connection.advance();
    }
    QCOMPARE(decision.count(), 1);
    const SshHostKeyChange change =
        qvariant_cast<SshHostKeyChange>(decision.first().first());
    QCOMPARE(change.oldFingerprint, oldFingerprint);
    QCOMPARE(change.newFingerprint, api.serverFingerprint);
    connection.advance();
    QCOMPARE(decision.count(), 1);
    QVERIFY(finished.isEmpty());

    connection.submitHostKeyDecision(change.requestId + 1, true);
    connection.advance();
    QCOMPARE(api.updateKnownHostsCount, 0);
    connection.submitHostKeyDecision(change.requestId, true);
    advanceUntilFinished(connection, finished);
    QCOMPARE(api.updateKnownHostsCount, 1);
    QCOMPARE(finished.count(), 1);
}

void TestLibsshConnection::rejectedChangedKeyDisconnects()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    const QByteArray oldBlob = ed25519Blob();
    QVERIFY(writeFile(store.path(),
                      "[alias.test]:2200 ssh-ed25519 "
                          + oldBlob.toBase64() + "\n"));
    api.knownState = SSH_KNOWN_HOSTS_OTHER;
    LibsshConnection connection(api, store);
    QSignalSpy decision(&connection,
                        &LibsshConnection::hostKeyDecisionRequired);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    for (int i = 0; i < 10 && decision.isEmpty(); ++i) {
        connection.advance();
    }
    const SshRequestId id =
        qvariant_cast<SshHostKeyChange>(decision.first().first()).requestId;
    connection.submitHostKeyDecision(id, false);

    QCOMPARE(finished.count(), 1);
    const SshError error = qvariant_cast<SshError>(finished.first().first());
    QCOMPARE(error.domain, SshError::Domain::HostKey);
    QCOMPARE(api.disconnectCount, 1);
}

void TestLibsshConnection::
    triesAgentThenExplicitKeyThenAutoKeyThenKeyboardInteractiveThenPassword()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.agentResults = {SSH_AUTH_DENIED};
    api.explicitKeyResults = {SSH_AUTH_DENIED};
    api.autoKeyResults = {SSH_AUTH_DENIED};
    api.keyboardResults = {SSH_AUTH_DENIED};
    api.passwordResults = {SSH_AUTH_AGAIN, SSH_AUTH_SUCCESS};
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    advanceUntilFinished(connection, finished);

    const int agent = api.calls.indexOf(QStringLiteral("authAgent"));
    const int explicitKey = api.calls.indexOf(
        QStringLiteral("authExplicit:C:/keys/ui-key"));
    const int autoKey = api.calls.indexOf(QStringLiteral("authAuto"));
    const int keyboard = api.calls.indexOf(QStringLiteral("authKeyboard"));
    const int password = api.calls.indexOf(QStringLiteral("authPassword"));
    QVERIFY(agent >= 0);
    QVERIFY(explicitKey > agent);
    QVERIFY(autoKey > explicitKey);
    QVERIFY(keyboard > autoKey);
    QVERIFY(password > keyboard);
    QCOMPARE(api.calls.count(QStringLiteral("authPassword")), 2);
    QCOMPARE(finished.count(), 1);
}

void TestLibsshConnection::explicitKeyLocalErrorStopsAuthentication()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.agentResults = {SSH_AUTH_DENIED};
    api.explicitKeyResults = {SSH_AUTH_ERROR};
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    advanceUntilFinished(connection, finished);

    QCOMPARE(finished.count(), 1);
    QCOMPARE(qvariant_cast<SshError>(finished.first().first()).domain,
             SshError::Domain::Authentication);
    QVERIFY(!api.calls.contains(QStringLiteral("authAuto")));
    QVERIFY(!api.calls.contains(QStringLiteral("authKeyboard")));
    QVERIFY(!api.calls.contains(QStringLiteral("authPassword")));
}

void TestLibsshConnection::keyboardInteractivePublishesAllPromptsAndResumes()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.agentResults = {SSH_AUTH_DENIED};
    api.explicitKeyResults = {SSH_AUTH_DENIED};
    api.autoKeyResults = {SSH_AUTH_DENIED};
    api.keyboardResults = {SSH_AUTH_INFO, SSH_AUTH_SUCCESS};
    api.prompts = {{QStringLiteral("Password:"), false},
                   {QStringLiteral("OTP:"), true}};
    LibsshConnection connection(api, store);
    QSignalSpy requested(&connection, &LibsshConnection::authAnswersRequired);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    for (int i = 0; i < 15 && requested.isEmpty(); ++i) {
        connection.advance();
    }
    QCOMPARE(requested.count(), 1);
    const SshRequestId id =
        qvariant_cast<SshRequestId>(requested.first().at(0));
    const QList<SshAuthPrompt> prompts =
        qvariant_cast<QList<SshAuthPrompt>>(requested.first().at(1));
    QCOMPARE(prompts.size(), 2);
    QCOMPARE(prompts.at(0).text, QStringLiteral("Password:"));
    QCOMPARE(prompts.at(1).echo, true);

    connection.submitAuthAnswers(id + 1, {QStringLiteral("ignored")});
    connection.advance();
    QVERIFY(api.answers.isEmpty());
    connection.submitAuthAnswers(
        id, {QStringLiteral("answer-one"), QStringLiteral("answer-two")});
    advanceUntilFinished(connection, finished);
    QCOMPARE(api.answers,
             QStringList({QStringLiteral("answer-one"),
                          QStringLiteral("answer-two")}));
    QCOMPARE(finished.count(), 1);
}

void TestLibsshConnection::keyboardInteractiveZeroPromptsContinuesWithoutUi()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.agentResults = {SSH_AUTH_DENIED};
    api.explicitKeyResults = {SSH_AUTH_DENIED};
    api.autoKeyResults = {SSH_AUTH_DENIED};
    api.keyboardResults = {SSH_AUTH_INFO, SSH_AUTH_SUCCESS};
    LibsshConnection connection(api, store);
    QSignalSpy requested(&connection, &LibsshConnection::authAnswersRequired);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    advanceUntilFinished(connection, finished);

    QCOMPARE(requested.count(), 0);
    QCOMPARE(api.calls.count(QStringLiteral("authKeyboard")), 2);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!qvariant_cast<SshError>(finished.first().first()).isError());
}

void TestLibsshConnection::
    changedKnownHostReadsOldFingerprintFromPrivateStore()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    const QByteArray oldBlob = ed25519Blob();
    const QString oldFingerprint = QString::fromLatin1(
        QCryptographicHash::hash(oldBlob, QCryptographicHash::Sha256).toHex());
    QVERIFY(writeFile(store.path(),
                      "[alias.test]:2200 ssh-ed25519 "
                          + oldBlob.toBase64() + "\n"));
    api.knownState = SSH_KNOWN_HOSTS_CHANGED;
    api.serverFingerprint = QString(64, QLatin1Char('b'));
    LibsshConnection connection(api, store);
    QSignalSpy decision(&connection,
                        &LibsshConnection::hostKeyDecisionRequired);

    connection.begin(target());
    for (int i = 0; i < 10 && decision.isEmpty(); ++i) {
        connection.advance();
    }

    QCOMPARE(decision.count(), 1);
    const SshHostKeyChange change =
        qvariant_cast<SshHostKeyChange>(decision.first().first());
    QCOMPARE(change.oldFingerprint, oldFingerprint);
    QCOMPARE(change.newFingerprint, api.serverFingerprint);
    QVERIFY(!api.calls.contains(QStringLiteral("knownHostFingerprint")));
}

void TestLibsshConnection::knownLegacyMatchRecoversInterruptedConsumption()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    const QByteArray blob = ed25519Blob();
    const QByteArray fingerprint =
        QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex();
    QVERIFY(writeFile(store.path(),
                      "[alias.test]:2200 ssh-ed25519 " + blob.toBase64() + "\n"));
    QVERIFY(writeFile(store.legacyPath(),
                      "alias.test:2200 " + fingerprint + "\n"));
    api.knownState = SSH_KNOWN_HOSTS_UNKNOWN;
    api.serverFingerprint = QString::fromLatin1(fingerprint);
    api.keyValidationResults = {false, true};
    api.agentResults = {SSH_AUTH_SUCCESS};
    LibsshConnection first(api, store);
    QSignalSpy firstFinished(&first, &LibsshConnection::finished);

    first.begin(target());
    advanceUntilFinished(first, firstFinished);
    QCOMPARE(firstFinished.count(), 1);
    QCOMPARE(qvariant_cast<SshError>(firstFinished.first().first()).domain,
             SshError::Domain::HostKey);
    QVERIFY(QFile(store.legacyPath()).size() > 0);

    api.knownState = SSH_KNOWN_HOSTS_OK;
    LibsshConnection retry(api, store);
    QSignalSpy retryFinished(&retry, &LibsshConnection::finished);
    retry.begin(target());
    advanceUntilFinished(retry, retryFinished);

    QCOMPARE(retryFinished.count(), 1);
    QVERIFY(!qvariant_cast<SshError>(retryFinished.first().first()).isError());
    QCOMPARE(QFile(store.legacyPath()).size(), qint64(0));
}

void TestLibsshConnection::cancellationCompletesOnce()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.connectResults = {SSH_AGAIN};
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    connection.advance();
    connection.advance();
    connection.close();
    connection.close();
    connection.advance();

    QCOMPARE(finished.count(), 1);
    QCOMPARE(qvariant_cast<SshError>(finished.first().first()).domain,
             SshError::Domain::Cancelled);
    QCOMPARE(api.disconnectCount, 1);
}

void TestLibsshConnection::reentrantBeginCancelsPreviousOperationOnce()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.connectResults = {SSH_AGAIN};
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    connection.advance();
    connection.advance();
    connection.begin(target());

    QCOMPARE(finished.count(), 1);
    QCOMPARE(qvariant_cast<SshError>(finished.first().first()).domain,
             SshError::Domain::Cancelled);
    QCOMPARE(api.calls.count(QStringLiteral("newSession")), 2);
    QCOMPARE(api.disconnectCount, 1);
}

void TestLibsshConnection::keepaliveFailureBecomesRetryableNetworkError()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    api.agentResults = {SSH_AUTH_SUCCESS};
    api.keepaliveResult = SSH_ERROR;
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);
    connection.begin(target());
    advanceUntilFinished(connection, finished);
    QCOMPARE(finished.count(), 1);

    const SshError error = connection.sendKeepalive();

    QCOMPARE(error.domain, SshError::Domain::Network);
    QVERIFY(error.retryable);
    QCOMPARE(error.code, api.errorNumber);
    QCOMPARE(error.message, api.errorText);
}

QTEST_MAIN(TestLibsshConnection)
#include "test_libsshconnection.moc"
