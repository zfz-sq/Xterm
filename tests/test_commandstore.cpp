#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "commandstore.h"

/*
 * SSH 密码不得进 config.json。保存后文件里没有 password 字段，
 * 私钥路径仍在；重新 load 后密码从凭据库回到内存。
 * 应用名带 pid，避免碰到用户真实的 Xterm 配置目录。
 */
class CommandStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void passwordRoundTripSkipsConfigFile();
    void legacyPasswordMigratesOutOfFile();

private:
    QJsonObject sshOfFirstProfile() const;
};

void CommandStoreTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("XtermUnit"));
    QCoreApplication::setApplicationName(
        QStringLiteral("XtermCmd-%1").arg(QCoreApplication::applicationPid()));
}

void CommandStoreTest::cleanupTestCase()
{
    CommandStore store;
    if (store.load()) {
        const QStringList names = store.profileNames();
        for (const QString &name : names)
            store.removeProfile(name);
        store.save();
    }
    QFile::remove(CommandStore::configPath());
}

QJsonObject CommandStoreTest::sshOfFirstProfile() const
{
    QFile file(CommandStore::configPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonArray profiles = QJsonDocument::fromJson(file.readAll()).object()
                                    .value(QStringLiteral("profiles")).toArray();
    if (profiles.isEmpty())
        return {};
    return profiles.at(0).toObject().value(QStringLiteral("ssh")).toObject();
}

void CommandStoreTest::passwordRoundTripSkipsConfigFile()
{
    CommandStore store;
    Profile profile = CommandStore::defaultProfile();
    profile.name = QStringLiteral("board");
    profile.ssh.host = QStringLiteral("10.0.0.8");
    profile.ssh.user = QStringLiteral("root");
    profile.ssh.password = QStringLiteral("s3cret");
    profile.ssh.privateKey = QStringLiteral("C:/keys/id_rsa");
    store.upsertProfile(profile);
    store.setLastProfile(profile.name);
    QVERIFY(store.save());

    const QJsonObject ssh = sshOfFirstProfile();
    QVERIFY(!ssh.contains(QStringLiteral("password")));
    QCOMPARE(ssh.value(QStringLiteral("privateKey")).toString(),
             QStringLiteral("C:/keys/id_rsa"));
    QCOMPARE(ssh.value(QStringLiteral("host")).toString(), QStringLiteral("10.0.0.8"));

    CommandStore again;
    QVERIFY(again.load());
    QCOMPARE(again.profile(QStringLiteral("board")).ssh.password, QStringLiteral("s3cret"));
    QCOMPARE(again.profile(QStringLiteral("board")).ssh.privateKey,
             QStringLiteral("C:/keys/id_rsa"));
}

void CommandStoreTest::legacyPasswordMigratesOutOfFile()
{
    const QString path = CommandStore::configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArrayLiteral(
        "{"
        "\"version\":1,"
        "\"lastProfile\":\"legacy-board\","
        "\"profiles\":[{\"name\":\"legacy-board\",\"ssh\":{"
        "\"host\":\"10.0.0.9\",\"user\":\"root\","
        "\"password\":\"legacy-secret\","
        "\"privateKey\":\"C:/keys/id_ed25519\""
        "},\"connection\":{},\"commands\":[]}]"
        "}"));
    file.close();

    CommandStore store;
    QVERIFY(store.load());
    QCOMPARE(store.profile(QStringLiteral("legacy-board")).ssh.password,
             QStringLiteral("legacy-secret"));
    QCOMPARE(store.profile(QStringLiteral("legacy-board")).ssh.privateKey,
             QStringLiteral("C:/keys/id_ed25519"));

    const QJsonObject ssh = sshOfFirstProfile();
    QVERIFY(!ssh.contains(QStringLiteral("password")));
    QCOMPARE(ssh.value(QStringLiteral("privateKey")).toString(),
             QStringLiteral("C:/keys/id_ed25519"));
}

QTEST_MAIN(CommandStoreTest)
#include "test_commandstore.moc"
