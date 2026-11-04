#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "auditlog.h"

/*
 * AuditLog（M2.e 可观测）：jsonl 追加 + 按大小轮转 + 坏路径静默。
 * rollBytes 注入小阈值验证轮转；纯文件协作类，不碰网络与管道。
 */
class AuditLogTest : public QObject
{
    Q_OBJECT

private slots:
    void appendWritesJsonlLines();
    void rollsWhenExceedsLimit();
    void badPathIsSilent();

private:
    static QJsonArray readLines(const QString &path);
};

// 常规追加：目录自动创建、每行一个 JSON、ts 由 append 统一补
void AuditLogTest::appendWritesJsonlLines()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("logs/nested/audit.jsonl"));

    {
        AuditLog log(path);
        for (int i = 0; i < 3; ++i) {
            QJsonObject e;
            e.insert(QStringLiteral("event"), QStringLiteral("call"));
            e.insert(QStringLiteral("seq"), i);
            log.append(e);
        }
    } // 析构关句柄，确保内容都落盘

    const QJsonArray lines = readLines(path);
    QCOMPARE(lines.size(), 3);
    for (int i = 0; i < 3; ++i) {
        const QJsonObject e = lines.at(i).toObject();
        QCOMPARE(e.value(QStringLiteral("seq")).toInt(), i);
        QVERIFY(!e.value(QStringLiteral("ts")).toString().isEmpty()); // 时间戳统一补
    }
}

// 轮转：超过 rollBytes 后当前文件降级 .old（只保两代），新内容重开写
void AuditLogTest::rollsWhenExceedsLimit()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("audit.jsonl"));
    const QString oldPath = path + QStringLiteral(".old");

    {
        AuditLog log(path, 1024); // 1 KiB 阈值：几条就够触发
        for (int i = 0; i < 40; ++i) {
            QJsonObject e;
            e.insert(QStringLiteral("event"), QStringLiteral("call"));
            e.insert(QStringLiteral("seq"), i);
            e.insert(QStringLiteral("padding"),
                     QString(QStringLiteral("x")).repeated(60));
            log.append(e);
        }
    }

    QVERIFY(QFile::exists(oldPath));
    const QJsonArray current = readLines(path);
    const QJsonArray rolled = readLines(oldPath);
    QVERIFY(current.size() + rolled.size() <= 40); // 轮转后总量只少不多
    QVERIFY(rolled.size() > 0);
    QVERIFY(!current.isEmpty());
    // 序列连续性抽查：.old 的首条早于当前文件的首条
    QVERIFY(rolled.at(0).toObject().value(QStringLiteral("seq")).toInt()
            < current.at(0).toObject().value(QStringLiteral("seq")).toInt());
}

// 坏路径静默：父路径是普通文件时 mkpath/open 都失败，append 不崩不写
void AuditLogTest::badPathIsSilent()
{
    QTemporaryDir dir;
    const QString blocker = QDir(dir.path()).filePath(QStringLiteral("blocker"));
    {
        QFile f(blocker);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    const QString badPath = blocker + QStringLiteral("/sub/audit.jsonl");

    AuditLog log(badPath);
    QJsonObject e;
    e.insert(QStringLiteral("event"), QStringLiteral("call"));
    log.append(e); // 不得崩、不得抛
    QCOMPARE(log.sizeBytes(), qint64(0));
    QVERIFY(!QFile::exists(badPath));
}

QJsonArray AuditLogTest::readLines(const QString &path)
{
    QJsonArray out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty())
            continue;
        out.append(QJsonDocument::fromJson(line).object());
    }
    return out;
}

QTEST_MAIN(AuditLogTest)
#include "test_auditlog.moc"
