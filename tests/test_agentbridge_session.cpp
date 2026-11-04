// SessionBridge（M2.f）：xterm.session.list/current/switch/openSsh/openSerial/close
// 六件工具的工具壳验证。后端是纯函数注入，fake 在内存里记录参数/回放数据，
// 不碰真会话与 UI；真实动作（写参数栏、连接发起）在 test_sessioncontroller
// 的 agent* 用例里覆盖。独立管道名 + 临时 agent.json，不碰真实配置。
#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>

#include "agentbridge.h"
#include "sessionbridge.h"

/*
 * 后端纯内存 fake：list/current 回放手造数据，动作类记录收到的参数、
 * 按 kind/port 决定成功或报错，验证工具壳的透传与错误冒泡。
 */
struct FakeSession
{
    QString switchKind{"<unset>"};
    QJsonObject openSshParams;
    QJsonObject openSerialParams;
    QString closeKind{"<unset>"};
    int closeCalls = 0;

    SessionBridge::SessionHooks hooks()
    {
        SessionBridge::SessionHooks h;
        h.list = [] {
            return QJsonArray{
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("serial")},
                            {QStringLiteral("active"), true},
                            {QStringLiteral("state"), QStringLiteral("connected")},
                            {QStringLiteral("summary"), QStringLiteral("COM3 @ 115200")}},
                QJsonObject{{QStringLiteral("kind"), QStringLiteral("ssh")},
                            {QStringLiteral("active"), false},
                            {QStringLiteral("state"), QStringLiteral("disconnected")},
                            {QStringLiteral("summary"), QStringLiteral("u@h:2222")}},
            };
        };
        h.current = [] {
            return QJsonObject{{QStringLiteral("mode"), QStringLiteral("serial")},
                               {QStringLiteral("state"), QStringLiteral("connected")}};
        };
        h.switchTo = [this](const QString &kind, QJsonObject *out) {
            switchKind = kind;
            if (kind != QLatin1String("serial") && kind != QLatin1String("ssh"))
                return QStringLiteral("kind 必须是 serial 或 ssh");
            out->insert(QStringLiteral("mode"), kind);
            return QString();
        };
        h.openSsh = [this](const QJsonObject &params, QJsonObject *out) {
            openSshParams = params;
            out->insert(QStringLiteral("state"), QStringLiteral("connecting"));
            return QString();
        };
        h.openSerial = [this](const QJsonObject &params, QJsonObject *out) {
            openSerialParams = params;
            if (params.value(QStringLiteral("port")).toString() == QLatin1String("BAD"))
                return QStringLiteral("端口 BAD 不存在");
            out->insert(QStringLiteral("state"), QStringLiteral("connecting"));
            return QString();
        };
        h.close = [this](const QString &kind, QJsonObject *out) {
            closeKind = kind;
            ++closeCalls;
            out->insert(QStringLiteral("closed"), true);
            return QString();
        };
        return h;
    }
};

class AgentBridgeSessionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void probeRegistration();
    void toolsListIncludesSessionTools();
    void listReturnsSessionsArray();
    void currentReturnsDetailObject();
    void switchRejectsUnknownKind();
    void switchPassesKindAndReturnsData();
    void openSshPassesParamsThrough();
    void openSerialRejectsBadPort();
    void closeDefaultsKindToEmpty();
    void partialBackendSkipsRegistration();

private:
    QJsonObject call(const QString &method, const QJsonObject &params = {});

    AgentBridge *m_bridge = nullptr;
    QTemporaryDir m_dir;
    QString m_pipe;
    QString m_token;
    SessionBridge m_sessionTools;
    FakeSession m_fake;
};

void AgentBridgeSessionTest::initTestCase()
{
    m_pipe = QStringLiteral("XtermAgentSessionTest-%1").arg(qApp->applicationPid());
    m_bridge = new AgentBridge(this, m_pipe);
    AgentBridge::Hooks hooks; // Session 用例不碰会话钩子，留空即可
    m_sessionTools.setBackend(m_fake.hooks());
    m_sessionTools.registerTools(&hooks);
    m_bridge->start(hooks, m_dir.filePath(QStringLiteral("agent.json")));

    QFile f(m_dir.filePath(QStringLiteral("agent.json")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject agent = QJsonDocument::fromJson(f.readAll()).object();
    m_token = agent.value(QStringLiteral("token")).toString();
    QVERIFY(!m_token.isEmpty());
}

QJsonObject AgentBridgeSessionTest::call(const QString &method, const QJsonObject &params)
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    if (!sock.waitForConnected(2000))
        return {};
    QJsonObject req;
    req.insert(QStringLiteral("id"), 1);
    req.insert(QStringLiteral("token"), m_token);
    req.insert(QStringLiteral("method"), method);
    req.insert(QStringLiteral("params"), params);
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    sock.write("\n");
    for (int i = 0; i < 300 && !sock.canReadLine(); ++i)
        QTest::qWait(10);
    if (!sock.canReadLine())
        return {};
    return QJsonDocument::fromJson(sock.readLine()).object();
}

void AgentBridgeSessionTest::probeRegistration()
{
    // 全套后端注入后 registerTools 插齐六件（partialBackendSkipsRegistration
    // 的阳性对照；不进管道，直查 hooks）
    AgentBridge::Hooks probe;
    SessionBridge bridge;
    bridge.setBackend(m_fake.hooks());
    bridge.registerTools(&probe);
    QCOMPARE(probe.tools.size(), 6);
    QCOMPARE(probe.toolMeta.size(), 6);
    QVERIFY(probe.tools.contains(QStringLiteral("xterm.session.list")));
    QVERIFY(probe.toolMeta.value(QStringLiteral("xterm.session.list"))
                .value(QStringLiteral("name"))
                .toString()
            == QStringLiteral("xterm.session.list"));
}

void AgentBridgeSessionTest::toolsListIncludesSessionTools()
{
    const QJsonObject resp = call(QStringLiteral("tools.list"));
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QStringList names;
    const QJsonArray tools = resp.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    for (const QJsonValue &v : tools)
        names << v.toObject().value(QStringLiteral("name")).toString();
    for (const char *name : { "xterm.session.list", "xterm.session.current",
                              "xterm.session.switch", "xterm.session.openSsh",
                              "xterm.session.openSerial", "xterm.session.close" })
        QVERIFY2(names.contains(QString::fromLatin1(name)), name);

    // meta 完整性抽查：switch 的 kind 是必填，openSsh 的 host 有描述
    for (const QJsonValue &v : tools) {
        const QJsonObject t = v.toObject();
        const QString name = t.value(QStringLiteral("name")).toString();
        if (name == QLatin1String("xterm.session.switch")) {
            QVERIFY(t.value(QStringLiteral("params")).toObject().contains(QStringLiteral("kind")));
        } else if (name == QLatin1String("xterm.session.openSsh")) {
            QVERIFY(t.contains(QStringLiteral("description")));
            QVERIFY(t.value(QStringLiteral("params"))
                        .toObject()
                        .contains(QStringLiteral("host")));
        }
    }
}

void AgentBridgeSessionTest::listReturnsSessionsArray()
{
    const QJsonObject resp = call(QStringLiteral("xterm.session.list"));
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonArray sessions = resp.value(QStringLiteral("result")).toObject()
                                   .value(QStringLiteral("sessions"))
                                   .toArray();
    QCOMPARE(sessions.size(), 2);
    const QJsonObject s0 = sessions.at(0).toObject();
    QCOMPARE(s0.value(QStringLiteral("kind")).toString(), QStringLiteral("serial"));
    QCOMPARE(s0.value(QStringLiteral("state")).toString(), QStringLiteral("connected"));
    QCOMPARE(s0.value(QStringLiteral("summary")).toString(), QStringLiteral("COM3 @ 115200"));
    QCOMPARE(sessions.at(1).toObject().value(QStringLiteral("kind")).toString(),
             QStringLiteral("ssh"));
}

void AgentBridgeSessionTest::currentReturnsDetailObject()
{
    const QJsonObject resp = call(QStringLiteral("xterm.session.current"));
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonObject result = resp.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("mode")).toString(), QStringLiteral("serial"));
    QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("connected"));
}

void AgentBridgeSessionTest::switchRejectsUnknownKind()
{
    // 错误应答：顶层 ok=false + error（无 result 字段）
    const QJsonObject resp =
        call(QStringLiteral("xterm.session.switch"),
             QJsonObject{{QStringLiteral("kind"), QStringLiteral("telnet")}});
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.contains(QStringLiteral("error")));
    QCOMPARE(m_fake.switchKind, QStringLiteral("telnet"));
}

void AgentBridgeSessionTest::switchPassesKindAndReturnsData()
{
    const QJsonObject resp =
        call(QStringLiteral("xterm.session.switch"),
             QJsonObject{{QStringLiteral("kind"), QStringLiteral("ssh")}});
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonObject result = resp.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("mode")).toString(), QStringLiteral("ssh"));
    QCOMPARE(m_fake.switchKind, QStringLiteral("ssh"));
}

void AgentBridgeSessionTest::openSshPassesParamsThrough()
{
    const QJsonObject resp =
        call(QStringLiteral("xterm.session.openSsh"),
             QJsonObject{{QStringLiteral("host"), QStringLiteral("192.168.1.10")},
                         {QStringLiteral("port"), 2222},
                         {QStringLiteral("user"), QStringLiteral("root")}});
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonObject result = resp.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("connecting"));
    QCOMPARE(m_fake.openSshParams.value(QStringLiteral("host")).toString(),
             QStringLiteral("192.168.1.10"));
    QCOMPARE(m_fake.openSshParams.value(QStringLiteral("port")).toInt(), 2222);
    QCOMPARE(m_fake.openSshParams.value(QStringLiteral("user")).toString(),
             QStringLiteral("root"));
}

void AgentBridgeSessionTest::openSerialRejectsBadPort()
{
    const QJsonObject resp =
        call(QStringLiteral("xterm.session.openSerial"),
             QJsonObject{{QStringLiteral("port"), QStringLiteral("BAD")},
                         {QStringLiteral("baud"), 9600}});
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.contains(QStringLiteral("error")));
    QCOMPARE(m_fake.openSerialParams.value(QStringLiteral("port")).toString(),
             QStringLiteral("BAD"));
    QCOMPARE(m_fake.openSerialParams.value(QStringLiteral("baud")).toInt(), 9600);
}

void AgentBridgeSessionTest::closeDefaultsKindToEmpty()
{
    const QJsonObject resp = call(QStringLiteral("xterm.session.close"));
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonObject result = resp.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("closed")).toBool(), true);
    QCOMPARE(m_fake.closeKind, QString()); // req 无 kind → 空串（= 当前活动链路）
    QCOMPARE(m_fake.closeCalls, 1);
}

void AgentBridgeSessionTest::partialBackendSkipsRegistration()
{
    SessionBridge partial; // 只填一件，其余缺：整套不注册
    SessionBridge::SessionHooks h;
    h.list = [] { return QJsonArray(); };
    partial.setBackend(h);
    AgentBridge::Hooks hooks;
    partial.registerTools(&hooks);
    QVERIFY(hooks.tools.isEmpty());
    QVERIFY(hooks.toolMeta.isEmpty());
}

QTEST_MAIN(AgentBridgeSessionTest)
#include "test_agentbridge_session.moc"
