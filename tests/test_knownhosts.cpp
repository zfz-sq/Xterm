#include "ssh/knownhostsstore.h"
#include "ssh/sshtypes.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace KnownHostsStoreDetail {
bool rollbackMigrationRename(const QString &currentPath,
                             const QString &legacyPath,
                             const QString &creationError,
                             QString *error);
}

namespace {

constexpr auto kFingerprint =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

QByteArray fingerprintForBlob(const QByteArray &blob)
{
    return QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex();
}

void appendSshString(QByteArray *blob, const QByteArray &value)
{
    const quint32 size = static_cast<quint32>(value.size());
    blob->append(static_cast<char>((size >> 24) & 0xff));
    blob->append(static_cast<char>((size >> 16) & 0xff));
    blob->append(static_cast<char>((size >> 8) & 0xff));
    blob->append(static_cast<char>(size & 0xff));
    blob->append(value);
}

QByteArray ed25519Blob(char fill)
{
    QByteArray blob;
    appendSshString(&blob, QByteArrayLiteral("ssh-ed25519"));
    appendSshString(&blob, QByteArray(32, fill));
    return blob;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

class FakeKnownHostKeyValidator final : public KnownHostKeyValidator
{
public:
    bool validateKnownHostKey(const QByteArray &algorithm,
                              const QByteArray &encodedKey) const override
    {
        ++calls;
        const auto decoded = QByteArray::fromBase64Encoding(
            encodedKey, QByteArray::AbortOnBase64DecodingErrors);
        const bool supported =
            algorithm == QByteArrayLiteral("ssh-ed25519")
            || algorithm == QByteArrayLiteral("ssh-rsa");
        return accept && decoded && supported
            && decoded.decoded.size() == 40 + algorithm.size();
    }

    mutable int calls = 0;
    bool accept = true;
};

} // namespace

class TestKnownHosts : public QObject
{
    Q_OBJECT

private slots:
    void migratesLegacyFileWithoutLosingIt();
    void readsLegacySha256ForHostAndPort();
    void ignoresMalformedLegacyRows();
    void standardKnownHostsPathStaysInsideAppConfig();
    void createsAndMaintainsEmptyGlobalKnownHosts();
    void readsStrictStandardFingerprintThroughValidator();
    void standardFingerprintSelectsExpectedKeyType();
    void listsAllStandardKeysInStableDeduplicatedOrder();
    void normalizesAcceptedHostKeyAtomically();
    void noPendingNormalizationPreservesAlternateKeys();
    void consumesLegacyAgainstDifferentStandardHost();
    void atomicRewritePreservesUnrelatedHosts();
    void requiresMatchingStandardKeyBeforeConsumingLegacy();
    void unreadableStandardFileDoesNotConsumeLegacy();
    void rejectsMalformedOrMismatchedStandardKeys();
    void rejectsNegatedHostPatterns();
    void matchesStandardHostPatternsAndPorts();
    void standardFileIsNotMigrated();
    void migrationRefusesLegacyConflict();
    void migrationReadFailureLeavesPathUntouched();
    void migrationRollbackRestoresOriginalFile();
    void migrationRollbackFailureReportsActualState();

private:
    FakeKnownHostKeyValidator m_validator;
};

void TestKnownHosts::migratesLegacyFileWithoutLosingIt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray legacyContents =
        "# retained comment\nexample.test:2222 " + QByteArray(kFingerprint) + "\n";
    QVERIFY(writeFile(store.path(), legacyContents));

    QString error;
    QVERIFY2(store.prepareMigration(&error), qPrintable(error));
    QCOMPARE(readFile(store.legacyPath()), legacyContents);
    QCOMPARE(readFile(store.path()), QByteArray());
}

void TestKnownHosts::readsLegacySha256ForHostAndPort()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray upperFingerprint = QByteArray(kFingerprint).toUpper();
    QVERIFY(writeFile(store.legacyPath(),
                      "alpha.test:22 " + upperFingerprint + "\n"
                      "alpha.test:2200 " + QByteArray(kFingerprint) + "\n"));

    QCOMPARE(store.legacyFingerprint(QStringLiteral("alpha.test"), 22),
             QString::fromLatin1(kFingerprint));
    QCOMPARE(store.legacyFingerprint(QStringLiteral("alpha.test"), 2200),
             QString::fromLatin1(kFingerprint));
    QVERIFY(store.legacyFingerprint(QStringLiteral("alpha.test"), 2222).isEmpty());
}

void TestKnownHosts::ignoresMalformedLegacyRows()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    QVERIFY(writeFile(store.legacyPath(),
                      "# comment\n"
                      "\n"
                      "missing-port " + QByteArray(kFingerprint) + "\n"
                      "bad.test:22 short\n"
                      "good.test:22 " + QByteArray(kFingerprint) + "\n"
                      "good.test:22 " + QByteArray(64, 'z') + "\n"));

    QCOMPARE(store.legacyFingerprint(QStringLiteral("good.test"), 22),
             QString::fromLatin1(kFingerprint));
    QVERIFY(store.legacyFingerprint(QStringLiteral("bad.test"), 22).isEmpty());
}

void TestKnownHosts::standardKnownHostsPathStaysInsideAppConfig()
{
    KnownHostsStore defaultStore(m_validator);
    const QString configRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QCOMPARE(defaultStore.path(), configRoot + QStringLiteral("/known_hosts"));
    QCOMPARE(defaultStore.legacyPath(),
             configRoot + QStringLiteral("/known_hosts.legacy"));
    QCOMPARE(defaultStore.globalPath(),
             configRoot + QStringLiteral("/known_hosts.global"));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore injectedStore(m_validator, directory.path());
    QCOMPARE(injectedStore.path(), directory.path() + QStringLiteral("/known_hosts"));
    QCOMPARE(injectedStore.globalPath(),
             directory.path() + QStringLiteral("/known_hosts.global"));
}

void TestKnownHosts::createsAndMaintainsEmptyGlobalKnownHosts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    QVERIFY(writeFile(store.globalPath(), QByteArrayLiteral("untrusted key\n")));

    QString error;
    QVERIFY2(store.prepareMigration(&error), qPrintable(error));

    QVERIFY(QFile::exists(store.path()));
    QVERIFY(QFile::exists(store.globalPath()));
    QCOMPARE(readFile(store.globalPath()), QByteArray());
}

void TestKnownHosts::readsStrictStandardFingerprintThroughValidator()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray blob = ed25519Blob('f');
    const QString expected =
        QString::fromLatin1(fingerprintForBlob(blob));
    QVERIFY(writeFile(store.path(),
                      "[target.test]:2200 ssh-ed25519 "
                          + blob.toBase64() + "\n"));
    m_validator.calls = 0;

    QString error;
    QCOMPARE(store.standardFingerprint(QStringLiteral("target.test"), 2200,
                                       QStringLiteral("ssh-ed25519"),
                                       &error),
             expected);
    QVERIFY(error.isEmpty());
    QCOMPARE(m_validator.calls, 1);

    m_validator.accept = false;
    QVERIFY(store.standardFingerprint(QStringLiteral("target.test"), 2200,
                                      QStringLiteral("ssh-ed25519"),
                                      &error).isEmpty());
    QVERIFY(!error.isEmpty());
    m_validator.accept = true;
}

void TestKnownHosts::standardFingerprintSelectsExpectedKeyType()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray rsa = [] {
        QByteArray blob;
        appendSshString(&blob, QByteArrayLiteral("ssh-rsa"));
        appendSshString(&blob, QByteArray(32, 'r'));
        return blob;
    }();
    const QByteArray ed = ed25519Blob('e');
    QVERIFY(writeFile(store.path(),
                      "target.test ssh-rsa " + rsa.toBase64() + "\n"
                      "target.test ssh-ed25519 " + ed.toBase64() + "\n"));

    QString error;
    QCOMPARE(store.standardFingerprint(
                 QStringLiteral("target.test"), 22,
                 QStringLiteral("ssh-ed25519"), &error),
             QString::fromLatin1(fingerprintForBlob(ed)));
}

void TestKnownHosts::listsAllStandardKeysInStableDeduplicatedOrder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    QByteArray rsa;
    appendSshString(&rsa, QByteArrayLiteral("ssh-rsa"));
    appendSshString(&rsa, QByteArray(32, 'r'));
    const QByteArray ed = ed25519Blob('e');
    QVERIFY(writeFile(store.path(),
                      "[real.test]:2222 ssh-rsa " + rsa.toBase64() + "\n"
                      "[real.test]:2222 ssh-ed25519 " + ed.toBase64() + "\n"
                      "[real.test]:2222 ssh-rsa " + rsa.toBase64() + "\n"));

    QString error;
    const QList<KnownHostKeyInfo> keys =
        store.standardKeys(QStringLiteral("REAL.TEST"), 2222, &error);

    QCOMPARE(keys.size(), 2);
    QCOMPARE(keys.at(0).keyType, QStringLiteral("ssh-rsa"));
    QCOMPARE(keys.at(0).fingerprint,
             QString::fromLatin1(fingerprintForBlob(rsa)));
    QCOMPARE(keys.at(1).keyType, QStringLiteral("ssh-ed25519"));
    QCOMPARE(keys.at(1).fingerprint,
             QString::fromLatin1(fingerprintForBlob(ed)));
}

void TestKnownHosts::normalizesAcceptedHostKeyAtomically()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray oldBlob = ed25519Blob('o');
    const QByteArray newBlob = ed25519Blob('n');
    const QByteArray other =
        "other.test ssh-ed25519 " + ed25519Blob('x').toBase64() + "\n";
    const QString newFingerprint =
        QString::fromLatin1(fingerprintForBlob(newBlob));
    QVERIFY(writeFile(store.path(),
                      "# comment\n" + other
                      + "target.test ssh-ed25519 "
                      + oldBlob.toBase64() + "\n"
                      + "target.test ssh-ed25519 "
                      + newBlob.toBase64() + "\n"));

    QString error;
    QVERIFY(store.containsStandardKey(
        QStringLiteral("TARGET.TEST"), 22,
        QStringLiteral("ssh-ed25519"), newFingerprint, &error));
    QVERIFY2(store.normalizeHostKeys(
                 QStringLiteral("TARGET.TEST"), 22,
                 QStringLiteral("ssh-ed25519"), newFingerprint, &error),
             qPrintable(error));

    const QByteArray normalized = readFile(store.path());
    QVERIFY(normalized.contains("# comment\n"));
    QVERIFY(normalized.contains(other));
    QVERIFY(!normalized.contains(oldBlob.toBase64()));
    QCOMPARE(normalized.count(newBlob.toBase64()), 1);
}

void TestKnownHosts::noPendingNormalizationPreservesAlternateKeys()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    QByteArray rsa;
    appendSshString(&rsa, QByteArrayLiteral("ssh-rsa"));
    appendSshString(&rsa, QByteArray(32, 'r'));
    const QByteArray ed = ed25519Blob('e');
    const QByteArray original =
        "target.test ssh-rsa " + rsa.toBase64() + "\n"
        "target.test ssh-ed25519 " + ed.toBase64() + "\n";
    QVERIFY(writeFile(store.path(), original));

    QString error;
    QVERIFY2(store.retryPendingNormalization(
                 QStringLiteral("target.test"), 22,
                 QStringLiteral("ssh-ed25519"),
                 QString::fromLatin1(fingerprintForBlob(ed)), &error),
             qPrintable(error));
    QCOMPARE(readFile(store.path()), original);
}

void TestKnownHosts::consumesLegacyAgainstDifferentStandardHost()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray newBlob = ed25519Blob('n');
    const QString oldFingerprint(64, QLatin1Char('a'));
    const QString newFingerprint =
        QString::fromLatin1(fingerprintForBlob(newBlob));
    QVERIFY(writeFile(store.legacyPath(),
                      "alias.test:2200 "
                          + oldFingerprint.toLatin1() + "\n"));
    QVERIFY(writeFile(store.path(),
                      "[real.test]:2222 ssh-ed25519 "
                          + newBlob.toBase64() + "\n"));

    QString error;
    QVERIFY2(store.consumeLegacyRecord(
                 QStringLiteral("alias.test"), 2200, oldFingerprint,
                 QStringLiteral("real.test"), 2222,
                 QStringLiteral("ssh-ed25519"), newFingerprint, &error),
             qPrintable(error));
    QCOMPARE(readFile(store.legacyPath()), QByteArray());
}

void TestKnownHosts::atomicRewritePreservesUnrelatedHosts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray keyBlob = ed25519Blob('t');
    const QByteArray fingerprint = fingerprintForBlob(keyBlob);
    const QByteArray standardContents =
        "target.test ssh-ed25519 " + keyBlob.toBase64() + "\n";
    const QByteArray legacyContents =
        "# preserve exactly\n"
        "target.test:22 " + fingerprint + "\n"
        "\n"
        "malformed row\n"
        "other.test:2200 " + QByteArray(64, 'a') + "\n";
    const QByteArray expectedLegacy =
        "# preserve exactly\n"
        "\n"
        "malformed row\n"
        "other.test:2200 " + QByteArray(64, 'a') + "\n";
    QVERIFY(writeFile(store.path(), standardContents));
    QVERIFY(writeFile(store.legacyPath(), legacyContents));

    QString error;
    QVERIFY2(store.rememberLegacyMatch(QStringLiteral("target.test"), 22,
                                       QString::fromLatin1(fingerprint).toUpper(),
                                       &error),
             qPrintable(error));
    QCOMPARE(readFile(store.legacyPath()), expectedLegacy);
    QCOMPARE(readFile(store.path()), standardContents);

    QVERIFY(!store.rememberLegacyMatch(QStringLiteral("other.test"), 2200,
                                       QString(64, QLatin1Char('b')), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.legacyPath()), expectedLegacy);
}

void TestKnownHosts::requiresMatchingStandardKeyBeforeConsumingLegacy()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray keyBlob = ed25519Blob('e');
    const QByteArray fingerprint = fingerprintForBlob(keyBlob);
    const QByteArray legacyContents =
        "target.test:22 " + fingerprint + "\n";
    QVERIFY(writeFile(store.legacyPath(), legacyContents));
    QVERIFY(writeFile(store.path(), QByteArray()));

    QString error;
    QVERIFY(!store.rememberLegacyMatch(QStringLiteral("target.test"), 22,
                                       QString::fromLatin1(fingerprint), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.legacyPath()), legacyContents);

    const QByteArray wrongBlob = ed25519Blob('w');
    QVERIFY(writeFile(store.path(),
                      "target.test ssh-ed25519 " + wrongBlob.toBase64() + "\n"));
    error.clear();
    QVERIFY(!store.rememberLegacyMatch(QStringLiteral("target.test"), 22,
                                       QString::fromLatin1(fingerprint), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.legacyPath()), legacyContents);

    QVERIFY(writeFile(store.path(),
                      "target.test ssh-ed25519 not!valid!base64\n"));
    error.clear();
    QVERIFY(!store.rememberLegacyMatch(QStringLiteral("target.test"), 22,
                                       QString::fromLatin1(fingerprint), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.legacyPath()), legacyContents);
}

void TestKnownHosts::unreadableStandardFileDoesNotConsumeLegacy()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray keyBlob = ed25519Blob('e');
    const QByteArray fingerprint = fingerprintForBlob(keyBlob);
    const QByteArray legacyContents =
        "target.test:22 " + fingerprint + "\n";
    QVERIFY(writeFile(store.legacyPath(), legacyContents));
    QVERIFY(QDir().mkdir(store.path()));

    QString error;
    QVERIFY(!store.rememberLegacyMatch(QStringLiteral("target.test"), 22,
                                       QString::fromLatin1(fingerprint), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.legacyPath()), legacyContents);
    QVERIFY(QFileInfo(store.path()).isDir());
}

void TestKnownHosts::rejectsMalformedOrMismatchedStandardKeys()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());

    QByteArray truncatedBlob;
    appendSshString(&truncatedBlob, QByteArrayLiteral("ssh-ed25519"));
    truncatedBlob.append("\0\0\0\x20", 4);
    truncatedBlob.append("tiny", 4);

    const QList<QPair<QByteArray, QByteArray>> invalidKeys = {
        {QByteArrayLiteral("arbitrary-text"), QByteArrayLiteral("arbitrary")},
        {QByteArrayLiteral("ssh-rsa"), ed25519Blob('m')},
        {QByteArrayLiteral("ssh-ed25519"), truncatedBlob},
    };
    for (const auto &invalidKey : invalidKeys) {
        const QByteArray fingerprint = fingerprintForBlob(invalidKey.second);
        const QByteArray legacyContents =
            "target.test:22 " + fingerprint + "\n";
        QVERIFY(writeFile(store.path(),
                          "target.test " + invalidKey.first + " "
                              + invalidKey.second.toBase64() + "\n"));
        QVERIFY(writeFile(store.legacyPath(), legacyContents));

        QString error;
        QVERIFY(!store.rememberLegacyMatch(
            QStringLiteral("target.test"), 22,
            QString::fromLatin1(fingerprint), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readFile(store.legacyPath()), legacyContents);
    }
}

void TestKnownHosts::rejectsNegatedHostPatterns()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray keyBlob = ed25519Blob('n');
    const QByteArray fingerprint = fingerprintForBlob(keyBlob);
    const QByteArray legacyContents =
        "target.test:22 " + fingerprint + "\n";
    const QList<QByteArray> patterns = {
        QByteArrayLiteral("target.test,!target.test"),
        QByteArrayLiteral("target.test,!*.test"),
        QByteArrayLiteral("target.test,!other.test"),
        QByteArrayLiteral("!target.test"),
    };

    for (const QByteArray &patternsField : patterns) {
        QVERIFY(writeFile(store.path(),
                          patternsField + " ssh-ed25519 "
                              + keyBlob.toBase64() + "\n"));
        QVERIFY(writeFile(store.legacyPath(), legacyContents));

        QString error;
        QVERIFY(!store.rememberLegacyMatch(
            QStringLiteral("target.test"), 22,
            QString::fromLatin1(fingerprint), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readFile(store.legacyPath()), legacyContents);
    }
}

void TestKnownHosts::matchesStandardHostPatternsAndPorts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray defaultBlob = ed25519Blob('d');
    const QByteArray alternateBlob = ed25519Blob('a');
    const QByteArray defaultFingerprint = fingerprintForBlob(defaultBlob);
    const QByteArray alternateFingerprint = fingerprintForBlob(alternateBlob);
    QVERIFY(writeFile(store.path(),
                      "# standard hosts\n"
                      "other.test,target.test ssh-ed25519 "
                          + defaultBlob.toBase64() + "\n"
                      "other.test,[target.test]:2200 ssh-ed25519 "
                          + alternateBlob.toBase64() + " comment\n"));
    QVERIFY(writeFile(store.legacyPath(),
                      "target.test:22 " + defaultFingerprint + "\n"
                      "target.test:2200 " + alternateFingerprint + "\n"));

    QString error;
    QVERIFY2(store.rememberLegacyMatch(QStringLiteral("target.test"), 22,
                                       QString::fromLatin1(defaultFingerprint),
                                       &error),
             qPrintable(error));
    QVERIFY2(store.rememberLegacyMatch(QStringLiteral("target.test"), 2200,
                                       QString::fromLatin1(alternateFingerprint),
                                       &error),
             qPrintable(error));
    QCOMPARE(readFile(store.legacyPath()), QByteArray());
}

void TestKnownHosts::standardFileIsNotMigrated()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray standardContents =
        "example.test ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAITestKey\n";
    QVERIFY(writeFile(store.path(), standardContents));

    QString error;
    QVERIFY2(store.prepareMigration(&error), qPrintable(error));
    QCOMPARE(readFile(store.path()), standardContents);
    QVERIFY(!QFile::exists(store.legacyPath()));
}

void TestKnownHosts::migrationRefusesLegacyConflict()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray currentLegacy =
        "new.test:22 " + QByteArray(kFingerprint) + "\n";
    const QByteArray preservedLegacy =
        "old.test:22 " + QByteArray(64, 'a') + "\n";
    QVERIFY(writeFile(store.path(), currentLegacy));
    QVERIFY(writeFile(store.legacyPath(), preservedLegacy));

    QString error;
    QVERIFY(!store.prepareMigration(&error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.path()), currentLegacy);
    QCOMPARE(readFile(store.legacyPath()), preservedLegacy);
}

void TestKnownHosts::migrationReadFailureLeavesPathUntouched()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    QVERIFY(QDir().mkdir(store.path()));

    QString error;
    QVERIFY(!store.prepareMigration(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo(store.path()).isDir());
    QVERIFY(!QFile::exists(store.legacyPath()));
}

void TestKnownHosts::migrationRollbackRestoresOriginalFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    const QByteArray legacyContents =
        "target.test:22 " + QByteArray(kFingerprint) + "\n";
    QVERIFY(writeFile(store.legacyPath(), legacyContents));
    QVERIFY(!QFile::exists(store.path()));

    QString error;
    QVERIFY(!KnownHostsStoreDetail::rollbackMigrationRename(
        store.path(), store.legacyPath(),
        QStringLiteral("Cannot create replacement"), &error));
    QVERIFY(error.contains(QStringLiteral("rolled back"),
                           Qt::CaseInsensitive));
    QCOMPARE(readFile(store.path()), legacyContents);
    QVERIFY(!QFile::exists(store.legacyPath()));
}

void TestKnownHosts::migrationRollbackFailureReportsActualState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(m_validator, directory.path());
    QVERIFY(writeFile(store.path(), QByteArray("blocking current file")));
    QVERIFY(writeFile(store.legacyPath(), QByteArray("preserved legacy file")));

    QString error;
    QVERIFY(!KnownHostsStoreDetail::rollbackMigrationRename(
        store.path(), store.legacyPath(),
        QStringLiteral("Cannot create replacement"), &error));
    QVERIFY(error.contains(store.path()));
    QVERIFY(error.contains(store.legacyPath()));
    QVERIFY(error.contains(QStringLiteral("exists=true")));
    QCOMPARE(readFile(store.path()), QByteArray("blocking current file"));
    QCOMPARE(readFile(store.legacyPath()), QByteArray("preserved legacy file"));
}

QTEST_MAIN(TestKnownHosts)
#include "test_knownhosts.moc"
