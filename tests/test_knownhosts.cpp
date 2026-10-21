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

} // namespace

class TestKnownHosts : public QObject
{
    Q_OBJECT

private slots:
    void migratesLegacyFileWithoutLosingIt();
    void readsLegacySha256ForHostAndPort();
    void ignoresMalformedLegacyRows();
    void standardKnownHostsPathStaysInsideAppConfig();
    void atomicRewritePreservesUnrelatedHosts();
    void requiresMatchingStandardKeyBeforeConsumingLegacy();
    void unreadableStandardFileDoesNotConsumeLegacy();
    void matchesStandardHostPatternsAndPorts();
    void standardFileIsNotMigrated();
    void migrationRefusesLegacyConflict();
    void migrationReadFailureLeavesPathUntouched();
    void migrationRollbackRestoresOriginalFile();
    void migrationRollbackFailureReportsActualState();
};

void TestKnownHosts::migratesLegacyFileWithoutLosingIt()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(directory.path());
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
    KnownHostsStore store(directory.path());
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
    KnownHostsStore store(directory.path());
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
    KnownHostsStore defaultStore;
    const QString configRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QCOMPARE(defaultStore.path(), configRoot + QStringLiteral("/known_hosts"));
    QCOMPARE(defaultStore.legacyPath(),
             configRoot + QStringLiteral("/known_hosts.legacy"));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore injectedStore(directory.path());
    QCOMPARE(injectedStore.path(), directory.path() + QStringLiteral("/known_hosts"));
}

void TestKnownHosts::atomicRewritePreservesUnrelatedHosts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(directory.path());
    const QByteArray keyBlob("target-public-key-blob");
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
    KnownHostsStore store(directory.path());
    const QByteArray keyBlob("expected-public-key-blob");
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

    const QByteArray wrongBlob("different-public-key-blob");
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
    KnownHostsStore store(directory.path());
    const QByteArray keyBlob("expected-public-key-blob");
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

void TestKnownHosts::matchesStandardHostPatternsAndPorts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    KnownHostsStore store(directory.path());
    const QByteArray defaultBlob("default-port-key-blob");
    const QByteArray alternateBlob("alternate-port-key-blob");
    const QByteArray defaultFingerprint = fingerprintForBlob(defaultBlob);
    const QByteArray alternateFingerprint = fingerprintForBlob(alternateBlob);
    QVERIFY(writeFile(store.path(),
                      "# standard hosts\n"
                      "other.test,target.test ssh-ed25519 "
                          + defaultBlob.toBase64() + "\n"
                      "other.test,[target.test]:2200 ssh-rsa "
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
    KnownHostsStore store(directory.path());
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
    KnownHostsStore store(directory.path());
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
    KnownHostsStore store(directory.path());
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
    KnownHostsStore store(directory.path());
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
    KnownHostsStore store(directory.path());
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
