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

QByteArray hostKeyBlob(const QByteArray &algorithm, char fill = 'm')
{
    QByteArray blob;
    appendSshString(&blob, algorithm);
    appendSshString(&blob, QByteArray(32, fill));
    return blob;
}

QByteArray ed25519Blob()
{
    return hostKeyBlob(QByteArrayLiteral("ssh-ed25519"));
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
    void productionExplicitKeyImportDiagnostics();
    void keyboardInteractivePublishesAllPromptsAndResumes();
    void keyboardInteractiveZeroPromptsContinuesWithoutUi();
    void changedKnownHostReadsOldFingerprintFromPrivateStore();
    void changedKnownHostSelectsCurrentServerKeyType();
    void acceptedLegacyMismatchIsConsumedBeforeReconnect();
    void knownLegacyMatchRecoversInterruptedConsumption();
    void cancellationCompletesOnce();
    void closeIdleIsNoOp();
    void reentrantBeginCancelsPreviousOperationOnce();
    void synchronousReentrantBeginUsesLastRequest();
    void synchronousCloseDefersBeginFromPhaseSignal();
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
    const QByteArray blob = ed25519Blob();
    QVERIFY(writeFile(store.path(),
                      "[alias.test]:2200 ssh-ed25519 "
                          + blob.toBase64() + "\n"));
    api.serverFingerprint = QString::fromLatin1(
        QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex());
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
                      "[real.test]:2200 ssh-ed25519 " + blob.toBase64() + "\n"));

    api.configuredHost = QStringLiteral("real.test");
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
    const QByteArray newBlob = ed25519Blob();
    QVERIFY(writeFile(store.path(),
                      "[alias.test]:2200 ssh-ed25519 "
                          + newBlob.toBase64() + "\n"));
    api.serverFingerprint = QString::fromLatin1(
        QCryptographicHash::hash(newBlob,
                                 QCryptographicHash::Sha256).toHex());
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
    api.explicitKeyErrorText =
        QStringLiteral("Private key file does not exist: C:/keys/ui-key");
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);

    connection.begin(target());
    advanceUntilFinished(connection, finished);

    QCOMPARE(finished.count(), 1);
    const SshError error =
        qvariant_cast<SshError>(finished.first().first());
    QCOMPARE(error.domain, SshError::Domain::Authentication);
    QCOMPARE(error.message, api.explicitKeyErrorText);
    QVERIFY(!api.calls.contains(QStringLiteral("authAuto")));
    QVERIFY(!api.calls.contains(QStringLiteral("authKeyboard")));
    QVERIFY(!api.calls.contains(QStringLiteral("authPassword")));
}

void TestLibsshConnection::productionExplicitKeyImportDiagnostics()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LibsshApi api;
    const QString missing = directory.filePath(QStringLiteral("missing-key"));

    QCOMPARE(api.authExplicitKey(nullptr, missing, {}), int(SSH_AUTH_ERROR));
    QVERIFY(api.authenticationErrorString(nullptr).contains(
        QStringLiteral("does not exist")));

    const QString corrupt = directory.filePath(QStringLiteral("corrupt-key"));
    QVERIFY(writeFile(corrupt, QByteArrayLiteral("not a private key")));
    QCOMPARE(api.authExplicitKey(nullptr, corrupt, {}), int(SSH_AUTH_ERROR));
    QVERIFY(api.authenticationErrorString(nullptr).contains(
        QStringLiteral("invalid or could not be decrypted")));
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
                      "[real.test]:2222 ssh-ed25519 "
                          + oldBlob.toBase64() + "\n"));
    api.configuredHost = QStringLiteral("real.test");
    api.configuredPort = 2222;
    api.knownState = SSH_KNOWN_HOSTS_CHANGED;
    api.serverFingerprint = QString(64, QLatin1Char('b'));
    LibsshConnection connection(api, store);
    QSignalSpy decision(&connection,
                        &LibsshConnection::hostKeyDecisionRequired);

    SshTarget configuredTarget = target();
    configuredTarget.port = 0;
    connection.begin(configuredTarget);
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

void TestLibsshConnection::changedKnownHostSelectsCurrentServerKeyType()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    const QByteArray rsaBlob =
        hostKeyBlob(QByteArrayLiteral("ssh-rsa"), 'r');
    const QByteArray ed25519 = ed25519Blob();
    const QString expected = QString::fromLatin1(
        QCryptographicHash::hash(ed25519,
                                 QCryptographicHash::Sha256).toHex());
    QVERIFY(writeFile(
        store.path(),
        "[alias.test]:2200 ssh-rsa " + rsaBlob.toBase64() + "\n"
            "[alias.test]:2200 ssh-ed25519 " + ed25519.toBase64() + "\n"));
    api.knownState = SSH_KNOWN_HOSTS_CHANGED;
    api.serverKeyType = QStringLiteral("ssh-ed25519");
    LibsshConnection connection(api, store);
    QSignalSpy decision(&connection,
                        &LibsshConnection::hostKeyDecisionRequired);

    connection.begin(target());
    for (int i = 0; i < 10 && decision.isEmpty(); ++i) {
        connection.advance();
    }

    QCOMPARE(decision.count(), 1);
    QCOMPARE(qvariant_cast<SshHostKeyChange>(
                 decision.first().first()).oldFingerprint,
             expected);
}

void TestLibsshConnection::acceptedLegacyMismatchIsConsumedBeforeReconnect()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    const QString legacyFingerprint(64, QLatin1Char('a'));
    const QByteArray newBlob = ed25519Blob();
    const QString newFingerprint = QString::fromLatin1(
        QCryptographicHash::hash(newBlob,
                                 QCryptographicHash::Sha256).toHex());
    QVERIFY(writeFile(store.legacyPath(),
                      "alias.test:2200 "
                          + legacyFingerprint.toLatin1() + "\n"));
    QVERIFY(writeFile(store.path(),
                      "[real.test]:2200 ssh-ed25519 "
                          + newBlob.toBase64() + "\n"));
    api.configuredHost = QStringLiteral("real.test");
    api.knownState = SSH_KNOWN_HOSTS_UNKNOWN;
    api.serverFingerprint = newFingerprint;
    api.agentResults = {SSH_AUTH_SUCCESS, SSH_AUTH_SUCCESS};
    LibsshConnection first(api, store);
    QSignalSpy firstDecision(&first,
                             &LibsshConnection::hostKeyDecisionRequired);
    QSignalSpy firstFinished(&first, &LibsshConnection::finished);

    first.begin(target());
    for (int i = 0; i < 10 && firstDecision.isEmpty(); ++i) {
        first.advance();
    }
    QCOMPARE(firstDecision.count(), 1);
    const SshHostKeyChange change =
        qvariant_cast<SshHostKeyChange>(
            firstDecision.first().first());
    QCOMPARE(change.oldFingerprint, legacyFingerprint);
    first.submitHostKeyDecision(change.requestId, true);
    advanceUntilFinished(first, firstFinished);
    QCOMPARE(firstFinished.count(), 1);
    QVERIFY(!qvariant_cast<SshError>(
                 firstFinished.first().first()).isError());
    QCOMPARE(QFile(store.legacyPath()).size(), qint64(0));

    api.knownState = SSH_KNOWN_HOSTS_OK;
    LibsshConnection retry(api, store);
    QSignalSpy retryDecision(&retry,
                             &LibsshConnection::hostKeyDecisionRequired);
    QSignalSpy retryFinished(&retry, &LibsshConnection::finished);
    retry.begin(target());
    advanceUntilFinished(retry, retryFinished);

    QCOMPARE(retryDecision.count(), 0);
    QCOMPARE(retryFinished.count(), 1);
    QVERIFY(!qvariant_cast<SshError>(
                 retryFinished.first().first()).isError());
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

void TestLibsshConnection::closeIdleIsNoOp()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);
    QSignalSpy phases(&connection, &LibsshConnection::phaseChanged);

    connection.close();

    QCOMPARE(finished.count(), 0);
    QCOMPARE(phases.count(), 0);
    QCOMPARE(api.disconnectCount, 0);
    QVERIFY(api.calls.isEmpty());
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

void TestLibsshConnection::synchronousReentrantBeginUsesLastRequest()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);
    SshTarget a = target();
    a.host = QStringLiteral("a.test");
    SshTarget b = target();
    b.host = QStringLiteral("b.test");
    SshTarget c = target();
    c.host = QStringLiteral("c.test");
    bool requestedC = false;
    QObject::connect(
        &connection, &LibsshConnection::finished, &connection,
        [&](const SshError &error) {
            if (!requestedC
                && error.domain == SshError::Domain::Cancelled) {
                requestedC = true;
                connection.begin(c);
            }
        },
        Qt::DirectConnection);

    connection.begin(a);
    connection.advance();
    connection.begin(b);
    connection.advance();

    QCOMPARE(finished.count(), 1);
    QCOMPARE(qvariant_cast<SshError>(finished.first().first()).domain,
             SshError::Domain::Cancelled);
    QCOMPARE(api.calls.count(QStringLiteral("newSession")), 2);
    QCOMPARE(api.calls.count(QStringLiteral("freeSession")), 1);
    QCOMPARE(api.stringOptions.value(SSH_OPTIONS_HOST),
             QStringLiteral("c.test"));
    QVERIFY(!api.calls.contains(
        QStringLiteral("string:%1=b.test").arg(int(SSH_OPTIONS_HOST))));

    connection.close();
    QCOMPARE(api.calls.count(QStringLiteral("freeSession")), 2);
}

void TestLibsshConnection::synchronousCloseDefersBeginFromPhaseSignal()
{
    QTemporaryDir directory;
    FakeLibsshApi api;
    KnownHostsStore store(api, directory.path());
    LibsshConnection connection(api, store);
    QSignalSpy finished(&connection, &LibsshConnection::finished);
    SshTarget replacement = target();
    replacement.host = QStringLiteral("replacement.test");
    bool requestedReplacement = false;
    QObject::connect(
        &connection, &LibsshConnection::phaseChanged, &connection,
        [&](SshPhase phase) {
            if (!requestedReplacement && phase == SshPhase::Closing) {
                requestedReplacement = true;
                connection.begin(replacement);
            }
        },
        Qt::DirectConnection);

    connection.begin(target());
    connection.advance();
    connection.close();
    connection.advance();

    QCOMPARE(finished.count(), 1);
    QCOMPARE(qvariant_cast<SshError>(finished.first().first()).domain,
             SshError::Domain::Cancelled);
    QCOMPARE(api.calls.count(QStringLiteral("newSession")), 2);
    QCOMPARE(api.calls.count(QStringLiteral("freeSession")), 1);
    QCOMPARE(api.stringOptions.value(SSH_OPTIONS_HOST),
             QStringLiteral("replacement.test"));
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
