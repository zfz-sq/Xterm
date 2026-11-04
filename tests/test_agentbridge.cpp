#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>

#include "agentbridge.h"

/*
 * AgentBridge 打字机行为测试：独立管道名 + 临时 agent.json，不碰真实配置，
 * 也不与正在运行的 Xterm 抢管道。
 */
class AgentBridgeTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void toolsListDiscoversNamespacedTools();
    void legacyShortNamesAreRejected();
    void typingCompletes();
    void typingInterrupted();
    void typingWaits();
    void waitTimesOutWhenNeedleNotNew();
    void waitForPatternMatchesNewText();
    void waitForPatternRejectsInvalidRegex();
    void clearAIsInstant();

private:
    QJsonObject waitReply(QLocalSocket *sock);
    void sendRequest(QLocalSocket *sock, int id, const QString &method, const QJsonObject &params);

    AgentBridge *m_bridge = nullptr;
    QTemporaryDir m_dir;
    QString m_pipe;
    QString m_agentFile;
    QString m_token;
    QByteArray m_sink;         // sendBytes 汇聚（打字机逐段 + 控制字符）
    QStringList m_sentLines;   // lineSent 记录（打字机完整发完的行）
    QStringList m_directLines; // sendLine 记录（clear -a 整行直达）
    QString m_recentText;      // recentText 可变内容
    qint64 m_lineCount = 0;   // recentLineCount 可变（绝对行锚点）
    QString m_freshText;       // recentTextFrom 可变（锚点之后的新增）
};

void AgentBridgeTest::initTestCase()
{
    m_pipe = QStringLiteral("XtermAgentTest-%1").arg(qApp->applicationPid());
    m_agentFile = m_dir.filePath(QStringLiteral("agent.json"));
    m_bridge = new AgentBridge(this, m_pipe);
    m_bridge->start({
        [] { return QJsonObject(); },
        [this](int) { return m_recentText; },
        [this] { return m_lineCount; },
        [this](qint64, int) { return m_freshText; },
        [this](const QString &text) { m_directLines.append(text); return QString(); },
        [this](const QByteArray &bytes) { m_sink += bytes; return QString(); },
        [] { return '\n'; },
        [this](const QString &line) { m_sentLines.append(line); },
    }, m_agentFile);
    QCOMPARE(m_bridge->pipeName(), m_pipe);

    QFile f(m_agentFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    m_token = QJsonDocument::fromJson(f.readAll()).object()
                  .value(QStringLiteral("token")).toString();
    QVERIFY(!m_token.isEmpty());
}

QJsonObject AgentBridgeTest::waitReply(QLocalSocket *sock)
{
    for (int i = 0; i < 300 && !sock->canReadLine(); ++i)
        QTest::qWait(10);
    if (!sock->canReadLine())
        return {};
    return QJsonDocument::fromJson(sock->readLine()).object();
}

void AgentBridgeTest::sendRequest(QLocalSocket *sock, int id, const QString &method,
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

// tools.list（M2.a）：客户端可自发现工具清单——五个内置工具都带 xterm.*
// 命名空间，且 sendLine 带参数描述
void AgentBridgeTest::toolsListDiscoversNamespacedTools()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    sendRequest(&sock, 10, QStringLiteral("tools.list"), {});
    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 10);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonArray tools = resp.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    QStringList names;
    bool sendLineHasParams = false;
    for (const QJsonValue &v : tools) {
        const QJsonObject t = v.toObject();
        names << t.value(QStringLiteral("name")).toString();
        if (t.value(QStringLiteral("name")).toString()
                == QLatin1String("xterm.transport.sendLine")) {
            sendLineHasParams = t.value(QStringLiteral("params")).toObject()
                                    .contains(QStringLiteral("text"));
        }
    }
    for (const char *name : { "xterm.session.status", "xterm.screen.read",
                              "xterm.screen.wait", "xterm.screen.waitForPattern",
                              "xterm.transport.sendLine",
                              "xterm.transport.sendControl" })
        QVERIFY2(names.contains(QString::fromLatin1(name)), name);
    QVERIFY(sendLineHasParams);
    sock.disconnectFromServer();
}

// 旧短名直接废弃（M2.1 总原则 5：不保留别名），报错里引导 tools.list
void AgentBridgeTest::legacyShortNamesAreRejected()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    sendRequest(&sock, 11, QStringLiteral("send_line"), {}); // 旧短名，应被拒
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("tools.list")));
    sock.disconnectFromServer();
}

void AgentBridgeTest::typingCompletes()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    m_sink.clear();
    m_sentLines.clear();
    QJsonObject params;
    params.insert(QStringLiteral("text"), QStringLiteral("dmesg"));
    params.insert(QStringLiteral("interval_ms"), 5);
    sendRequest(&sock, 1, QStringLiteral("xterm.transport.sendLine"), params);

    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 1);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    // 打字机逐段发送，汇聚后应是「命令 + 链路行尾」
    QCOMPARE(m_sink, QByteArray("dmesg\n"));
    QCOMPARE(m_sentLines, QStringList{ QStringLiteral("dmesg") });
    sock.disconnectFromServer();
}

void AgentBridgeTest::typingInterrupted()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QString longCmd;
    for (int i = 0; i < 80; ++i)
        longCmd += QChar(char('a' + (i % 26)));
    m_sink.clear();
    m_sentLines.clear();
    QJsonObject params;
    params.insert(QStringLiteral("text"), longCmd);
    params.insert(QStringLiteral("interval_ms"), 30);
    sendRequest(&sock, 2, QStringLiteral("xterm.transport.sendLine"), params);

    QTest::qWait(70); // 只打出去一小截
    QVERIFY(m_sink.size() < longCmd.size());
    QVERIFY(m_bridge->isTyping()); // 锁定状态依据
    QVERIFY(m_bridge->interruptTyping());
    QVERIFY(!m_bridge->isTyping());

    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 2);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString().contains(QStringLiteral("interrupted")));
    QVERIFY(m_sink.size() < longCmd.size() + 1); // 剩余字符没有继续发出
    QVERIFY(m_sentLines.isEmpty());              // 被打断不算完整一行
    QVERIFY(!m_bridge->interruptTyping());       // 已无打字可打断
    sock.disconnectFromServer();
}

void AgentBridgeTest::typingWaits()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    // 模拟高频刷屏设备：recentText（120 行滑动窗）里 needle 一直都在，
    // 但行锚点之后真正「新增」的文本一开始没有 needle——旧实现按窗口内
    // 次数差判新增，会既不命中也不增长；新实现只看锚点后的新增。
    m_recentText = QStringLiteral("CoreBoard send\nCoreBoard recv\nCoreBoard send");
    m_lineCount = 100;
    m_freshText = QString();
    QJsonObject params;
    params.insert(QStringLiteral("text"), QStringLiteral("uptime"));
    params.insert(QStringLiteral("wait"), QStringLiteral("CoreBoard"));
    params.insert(QStringLiteral("interval_ms"), 5);
    params.insert(QStringLiteral("timeout_ms"), 3000);
    sendRequest(&sock, 3, QStringLiteral("xterm.transport.sendLine"), params);

    QTest::qWait(80); // 打字已结束、wait 已挂上（此时新增里还没有 needle）
    m_lineCount = 115; // 设备刷了 15 行新内容，其中出现 needle
    m_freshText = QStringLiteral("CoreBoard send\nvol_data: 1786");
    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 3);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("matched")).toBool());
    sock.disconnectFromServer();
}

void AgentBridgeTest::waitTimesOutWhenNeedleNotNew()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    // needle 只存在于锚点之前的历史里：新增为空 → 超时，不误命中
    m_recentText = QStringLiteral("old CoreBoard lines");
    m_lineCount = 100;
    m_freshText = QString();
    QJsonObject params;
    params.insert(QStringLiteral("text"), QStringLiteral("uptime"));
    params.insert(QStringLiteral("wait"), QStringLiteral("CoreBoard"));
    params.insert(QStringLiteral("interval_ms"), 5);
    params.insert(QStringLiteral("timeout_ms"), 200);
    sendRequest(&sock, 5, QStringLiteral("xterm.transport.sendLine"), params);

    const QJsonObject resp = waitReply(&sock);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("timed_out")).toBool());
    sock.disconnectFromServer();
}

// M2.d waitForPattern：正则在新增文本里命中（锚点后），大小写不敏感
void AgentBridgeTest::waitForPatternMatchesNewText()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    // 命令回显里就带 pattern 字样，min_new=2 要求真实输出再出现一次才命中
    m_recentText = QStringLiteral("$ dmesg | grep error");
    m_lineCount = 200;
    m_freshText = QStringLiteral("$ dmesg | grep error"); // 新增第 1 次（回显）
    QJsonObject params;
    params.insert(QStringLiteral("pattern"), QStringLiteral("ERROR|FAIL"));
    params.insert(QStringLiteral("min_new"), 2);
    params.insert(QStringLiteral("timeout_ms"), 3000);
    sendRequest(&sock, 6, QStringLiteral("xterm.screen.waitForPattern"), params);

    QTest::qWait(80); // 已挂起；此时新增里只有回显那一次，不够 min_new=2
    m_lineCount = 205;
    m_freshText += QStringLiteral("\n[  12.3] FAIL: timeout"); // 真实输出第 2 次
    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 6);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("matched")).toBool());
    sock.disconnectFromServer();
}

// 非法正则当场拒绝（不挂起），错误里带 pattern 供 AI 自查
void AgentBridgeTest::waitForPatternRejectsInvalidRegex()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QJsonObject params;
    params.insert(QStringLiteral("pattern"), QStringLiteral("([unclosed"));
    sendRequest(&sock, 7, QStringLiteral("xterm.screen.waitForPattern"), params);

    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 7);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("正则无效")));
    sock.disconnectFromServer();
}

void AgentBridgeTest::clearAIsInstant()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    m_sink.clear();
    m_directLines.clear();
    QJsonObject params;
    params.insert(QStringLiteral("text"), QStringLiteral("clear -a"));
    sendRequest(&sock, 4, QStringLiteral("xterm.transport.sendLine"), params);

    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 4);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QCOMPARE(m_directLines, QStringList{ QStringLiteral("clear -a") });
    QVERIFY(m_sink.isEmpty()); // 本地命令不进打字机、不出字节
    sock.disconnectFromServer();
}

QTEST_MAIN(AgentBridgeTest)
#include "test_agentbridge.moc"
