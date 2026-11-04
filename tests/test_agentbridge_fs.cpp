#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include "agentbridge.h"
#include "fsbridge.h"

/*
 * FsBridge + xterm.fs.* 工具端到端测试（M2.b）。
 *
 * 后端用内存 fake（目录表 + 文件表），download/upload 走真实本地文件——
 * FsBridge 的临时文件桥接与限长读同样被覆盖；沙箱与 allow_write 是重点。
 * 独立管道名 + 临时 agent.json，不碰真实配置与网络。
 */

// 内存远端：list 查目录表；download/upload 读写本地文件（与真实链路同形）
struct FakeFs
{
    QString root = QStringLiteral("/home/dev");
    QHash<QString, QVector<SftpEntry>> dirs;
    QHash<QString, QByteArray> files;
    QStringList uploads; // 成功 upload 的远端路径记录

    FsBridge::FsHooks hooks()
    {
        FsBridge::FsHooks h;
        h.realPath = [this](const QString &path, QString *err) {
            Q_UNUSED(err);
            return path == QLatin1String(".") ? root : path;
        };
        h.list = [this](const QString &path, QString *err) {
            const auto it = dirs.constFind(path);
            if (it == dirs.constEnd()) {
                *err = QStringLiteral("无法打开目录: %1").arg(path);
                return QVector<SftpEntry>();
            }
            return it.value();
        };
        h.download = [this](const QString &remote, const QString &local, QString *err) {
            const auto it = files.constFind(remote);
            if (it == files.constEnd()) {
                *err = QStringLiteral("文件不存在: %1").arg(remote);
                return false;
            }
            QFile f(local);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                *err = QStringLiteral("本地写入失败: %1").arg(local);
                return false;
            }
            f.write(it.value());
            return true;
        };
        h.upload = [this](const QString &local, const QString &remote, QString *err) {
            QFile f(local);
            if (!f.open(QIODevice::ReadOnly)) {
                *err = QStringLiteral("本地读取失败: %1").arg(local);
                return false;
            }
            files[remote] = f.readAll();
            uploads << remote;
            return true;
        };
        return h;
    }
};

class AgentBridgeFsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void toolsListIncludesFsTools();
    void listReturnsEntries();
    void statReportsEntryAndMissing();
    void readReturnsText();
    void readRejectsOversize();
    void sandboxRejectsOutside();
    void writeRejectedWithoutAllowWrite();
    void writeUploadsWhenAllowed();
    void downloadWorksByDefault();
    void uploadRejectedWithoutAllowWrite();
    void fsUnavailableWithoutRoot();
    void normalizePosixCollapsesAndClamps();

private:
    QJsonObject waitReply(QLocalSocket *sock);
    void sendRequest(QLocalSocket *sock, int id, const QString &method, const QJsonObject &params);

    AgentBridge *m_bridge = nullptr;
    QTemporaryDir m_dir;
    QString m_pipe;
    QString m_token;
    FsBridge m_fs;
    FakeFs m_fake;
};

void AgentBridgeFsTest::initTestCase()
{
    m_fake.dirs.insert(QStringLiteral("/home/dev"),
                       {SftpEntry{QStringLiteral("sub"), 0, 100, 0755, true, false},
                        SftpEntry{QStringLiteral("a.txt"), 9, 200, 0644, false, false}});
    m_fake.dirs.insert(QStringLiteral("/home/dev/sub"), {});
    m_fake.files.insert(QStringLiteral("/home/dev/a.txt"), QByteArrayLiteral("hello fs\n"));

    m_pipe = QStringLiteral("XtermAgentFsTest-%1").arg(qApp->applicationPid());
    m_bridge = new AgentBridge(this, m_pipe);
    AgentBridge::Hooks hooks; // FS 用例不碰会话钩子，留空即可
    m_fs.setBackend(m_fake.hooks());
    m_fs.setAllowWrite(false);
    m_fs.registerTools(&hooks);
    QVERIFY(m_fs.refreshRoot()); // fake realPath(".") → /home/dev，沙箱根就位
    QCOMPARE(m_fs.root(), QStringLiteral("/home/dev"));
    m_bridge->start(hooks, m_dir.filePath(QStringLiteral("agent.json")));

    QFile f(m_dir.filePath(QStringLiteral("agent.json")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject agent = QJsonDocument::fromJson(f.readAll()).object();
    m_token = agent.value(QStringLiteral("token")).toString();
    QVERIFY(!m_token.isEmpty());
    // 写类开关同步披露到 agent.json（M2.b），与 FsBridge 默认一致
    QVERIFY(agent.contains(QStringLiteral("allow_write")));
    QVERIFY(!agent.value(QStringLiteral("allow_write")).toBool());
}

QJsonObject AgentBridgeFsTest::waitReply(QLocalSocket *sock)
{
    for (int i = 0; i < 300 && !sock->canReadLine(); ++i)
        QTest::qWait(10);
    if (!sock->canReadLine())
        return {};
    return QJsonDocument::fromJson(sock->readLine()).object();
}

void AgentBridgeFsTest::sendRequest(QLocalSocket *sock, int id, const QString &method,
                                    const QJsonObject &params)
{
    QJsonObject req;
    req.insert(QStringLiteral("id"), id);
    req.insert(QStringLiteral("token"), m_token);
    req.insert(QStringLiteral("method"), method);
    req.insert(QStringLiteral("params"), params);
    sock->write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    sock->write("\n");
}

// tools.list：六件 FS 工具都注册进了清单（M2.a 的自发现对扩展工具同样生效）
void AgentBridgeFsTest::toolsListIncludesFsTools()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    sendRequest(&sock, 1, QStringLiteral("tools.list"), {});
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QStringList names;
    const QJsonArray tools = resp.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    for (const QJsonValue &v : tools)
        names << v.toObject().value(QStringLiteral("name")).toString();
    for (const char *name : { "xterm.fs.stat", "xterm.fs.list", "xterm.fs.read",
                              "xterm.fs.write", "xterm.fs.download", "xterm.fs.upload" })
        QVERIFY2(names.contains(QString::fromLatin1(name)), name);
    sock.disconnectFromServer();
}

// list：相对路径按会话根解析，返回条目与归一化后的绝对路径
void AgentBridgeFsTest::listReturnsEntries()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QJsonObject params;
    params.insert(QStringLiteral("path"), QStringLiteral("."));
    sendRequest(&sock, 2, QStringLiteral("xterm.fs.list"), params);
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonObject result = resp.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("path")).toString(), QStringLiteral("/home/dev"));
    const QJsonArray entries = result.value(QStringLiteral("entries")).toArray();
    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("sub"));
    QVERIFY(entries.at(0).toObject().value(QStringLiteral("is_dir")).toBool());
    QCOMPARE(entries.at(1).toObject().value(QStringLiteral("size")).toDouble(), 9.0);
    sock.disconnectFromServer();
}

// stat：存在 → 元数据；不存在 → exists=false（探测不算错）；目录/文件分型正确
void AgentBridgeFsTest::statReportsEntryAndMissing()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QJsonObject p1;
    p1.insert(QStringLiteral("path"), QStringLiteral("a.txt"));
    sendRequest(&sock, 3, QStringLiteral("xterm.fs.stat"), p1);
    QJsonObject r = waitReply(&sock).value(QStringLiteral("result")).toObject();
    QVERIFY(r.value(QStringLiteral("exists")).toBool());
    QCOMPARE(r.value(QStringLiteral("size")).toDouble(), 9.0);
    QVERIFY(!r.value(QStringLiteral("is_dir")).toBool());
    QCOMPARE(r.value(QStringLiteral("mtime")).toDouble(), 200.0);

    QJsonObject p2;
    p2.insert(QStringLiteral("path"), QStringLiteral("sub"));
    sendRequest(&sock, 4, QStringLiteral("xterm.fs.stat"), p2);
    r = waitReply(&sock).value(QStringLiteral("result")).toObject();
    QVERIFY(r.value(QStringLiteral("exists")).toBool());
    QVERIFY(r.value(QStringLiteral("is_dir")).toBool());

    QJsonObject p3;
    p3.insert(QStringLiteral("path"), QStringLiteral("nope.bin"));
    sendRequest(&sock, 5, QStringLiteral("xterm.fs.stat"), p3);
    const QJsonObject resp3 = waitReply(&sock);
    QVERIFY(resp3.value(QStringLiteral("ok")).toBool()); // 探测不存在 = 成功 + exists=false
    QVERIFY(!resp3.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("exists")).toBool());
    sock.disconnectFromServer();
}

// read：download → 临时文件 → 限长读回，文本与字节数一致
void AgentBridgeFsTest::readReturnsText()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QJsonObject params;
    params.insert(QStringLiteral("path"), QStringLiteral("a.txt"));
    sendRequest(&sock, 6, QStringLiteral("xterm.fs.read"), params);
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonObject result = resp.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("text")).toString(), QStringLiteral("hello fs\n"));
    QCOMPARE(result.value(QStringLiteral("size")).toDouble(), 9.0);
    sock.disconnectFromServer();
}

// read 超限：引导走 download，不把巨文件塞进 JSON 响应
void AgentBridgeFsTest::readRejectsOversize()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QJsonObject params;
    params.insert(QStringLiteral("path"), QStringLiteral("a.txt"));
    params.insert(QStringLiteral("max_bytes"), 3);
    sendRequest(&sock, 7, QStringLiteral("xterm.fs.read"), params);
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("xterm.fs.download")));
    sock.disconnectFromServer();
}

// 沙箱：绝对路径在根外 / .. 逃逸 / 词法绕道，一律拒绝
void AgentBridgeFsTest::sandboxRejectsOutside()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    const QList<QPair<int, QString>> attempts = {
        {8, QStringLiteral("/etc")},                       // 根外绝对路径
        {9, QStringLiteral("../escape")},                  // 相对 .. 逃逸
        {10, QStringLiteral("/home/../etc/passwd")},       // 词法绕道归一后仍在根外
        {11, QStringLiteral("sub/../../other/x")},         // 多级回退逃出
    };
    for (const auto &attempt : attempts) {
        QJsonObject params;
        params.insert(QStringLiteral("path"), attempt.second);
        sendRequest(&sock, attempt.first, QStringLiteral("xterm.fs.list"), params);
        const QJsonObject resp = waitReply(&sock);
        QVERIFY2(!resp.value(QStringLiteral("ok")).toBool(),
                 qPrintable(attempt.second));
        QVERIFY2(resp.value(QStringLiteral("error")).toString()
                     .contains(QStringLiteral("越界")),
                 qPrintable(resp.value(QStringLiteral("error")).toString()));
    }
    sock.disconnectFromServer();
}

// 写类默认拒绝（M2.b 验收：写类默认拒绝），报错引导 allow_write
void AgentBridgeFsTest::writeRejectedWithoutAllowWrite()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    m_fs.setAllowWrite(false);
    m_fake.uploads.clear();
    QJsonObject params;
    params.insert(QStringLiteral("path"), QStringLiteral("sub/new.txt"));
    params.insert(QStringLiteral("content"), QStringLiteral("x"));
    sendRequest(&sock, 12, QStringLiteral("xterm.fs.write"), params);
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("allow_write")));
    QVERIFY(m_fake.uploads.isEmpty()); // 后端确实没被碰到
    sock.disconnectFromServer();
}

// 开启 allow_write 后：write 覆盖写 + read 回读一致（临时文件桥接双向验证）
void AgentBridgeFsTest::writeUploadsWhenAllowed()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    m_fs.setAllowWrite(true);
    QJsonObject params;
    params.insert(QStringLiteral("path"), QStringLiteral("sub/new.txt"));
    params.insert(QStringLiteral("content"), QStringLiteral("written by agent"));
    sendRequest(&sock, 13, QStringLiteral("xterm.fs.write"), params);
    const QJsonObject resp = waitReply(&sock);
    m_fs.setAllowWrite(false);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QCOMPARE(m_fake.files.value(QStringLiteral("/home/dev/sub/new.txt")),
             QByteArrayLiteral("written by agent"));
    QVERIFY(m_fake.uploads.contains(QStringLiteral("/home/dev/sub/new.txt")));

    QJsonObject rp;
    rp.insert(QStringLiteral("path"), QStringLiteral("sub/new.txt"));
    sendRequest(&sock, 14, QStringLiteral("xterm.fs.read"), rp);
    const QJsonObject back = waitReply(&sock);
    QVERIFY(back.value(QStringLiteral("ok")).toBool());
    QCOMPARE(back.value(QStringLiteral("result")).toObject()
                 .value(QStringLiteral("text")).toString(),
             QStringLiteral("written by agent"));
    sock.disconnectFromServer();
}

// download：读类默认可用；远端路径同样受沙箱约束，本地需绝对路径
void AgentBridgeFsTest::downloadWorksByDefault()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QTemporaryFile tmp;
    QVERIFY(tmp.open());
    tmp.close();
    QJsonObject params;
    params.insert(QStringLiteral("remote_path"), QStringLiteral("a.txt"));
    params.insert(QStringLiteral("local_path"), tmp.fileName());
    sendRequest(&sock, 15, QStringLiteral("xterm.fs.download"), params);
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QFile f(tmp.fileName());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArrayLiteral("hello fs\n"));
    sock.disconnectFromServer();
}

// upload：写类，默认同样拒绝
void AgentBridgeFsTest::uploadRejectedWithoutAllowWrite()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QTemporaryFile tmp;
    QVERIFY(tmp.open());
    tmp.write("data");
    tmp.close();
    QJsonObject params;
    params.insert(QStringLiteral("local_path"), tmp.fileName());
    params.insert(QStringLiteral("remote_path"), QStringLiteral("up.bin"));
    sendRequest(&sock, 16, QStringLiteral("xterm.fs.upload"), params);
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("allow_write")));
    sock.disconnectFromServer();
}

// 无沙箱根（SSH 未连接）：读类工具回到「不可用」态，不是后端报错
void AgentBridgeFsTest::fsUnavailableWithoutRoot()
{
    const QString pipe2 = QStringLiteral("XtermAgentFsOffline-%1").arg(qApp->applicationPid());
    AgentBridge bridge(this, pipe2);
    FsBridge fs; // 同一 fake 后端但不 refreshRoot：模拟 SSH 未连接
    AgentBridge::Hooks hooks;
    fs.setBackend(m_fake.hooks());
    fs.registerTools(&hooks);
    bridge.start(hooks, m_dir.filePath(QStringLiteral("agent-offline.json")));

    QFile f(m_dir.filePath(QStringLiteral("agent-offline.json")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString token = QJsonDocument::fromJson(f.readAll()).object()
                              .value(QStringLiteral("token")).toString();

    QLocalSocket sock;
    sock.connectToServer(pipe2);
    QVERIFY(sock.waitForConnected(2000));
    QJsonObject req;
    req.insert(QStringLiteral("id"), 20);
    req.insert(QStringLiteral("token"), token);
    req.insert(QStringLiteral("method"), QStringLiteral("xterm.fs.list"));
    req.insert(QStringLiteral("params"), QJsonObject{{QStringLiteral("path"), QStringLiteral(".")}});
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    sock.write("\n");
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("SSH 未连接")));
    sock.disconnectFromServer();
}

// 词法归一：分隔符合并、. 消解、.. 回退且在根处截停
void AgentBridgeFsTest::normalizePosixCollapsesAndClamps()
{
    QCOMPARE(FsBridge::normalizePosix(QStringLiteral("/a//b/./c")), QStringLiteral("/a/b/c"));
    QCOMPARE(FsBridge::normalizePosix(QStringLiteral("/a/b/../c")), QStringLiteral("/a/c"));
    QCOMPARE(FsBridge::normalizePosix(QStringLiteral("/../../etc")), QStringLiteral("/etc"));
    QCOMPARE(FsBridge::normalizePosix(QStringLiteral("/")), QStringLiteral("/"));
}

QTEST_MAIN(AgentBridgeFsTest)
#include "test_agentbridge_fs.moc"
