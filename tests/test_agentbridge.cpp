#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSignalSpy>
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
    void sendTypewriterMatchesSendLine();
    void interruptToolStopsTypingAndSendsCtrlC();
    void typingWaits();
    void waitTimesOutWhenNeedleNotNew();
    void waitForPatternMatchesNewText();
    void waitForPatternRejectsInvalidRegex();
    void clearAIsInstant();
    void auditJsonlRecordsCallsAndSignals();
    void auditRecordsFailures();
    void rateLimitRejectsBurst();

private:
    QJsonObject waitReply(QLocalSocket *sock);
    void sendRequest(QLocalSocket *sock, int id, const QString &method, const QJsonObject &params);
    // 读临时 agent.json 同目录 logs/ 下的审计 jsonl，返回全部条目
    QJsonArray readAuditEntries() const;

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
                              "xterm.transport.sendControl",
                              "xterm.transport.sendTypewriter",
                              "xterm.transport.interrupt" })
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

// B 组正名：sendTypewriter 是 sendLine 的正式命名入口，行为完全一致
void AgentBridgeTest::sendTypewriterMatchesSendLine()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    m_sink.clear();
    m_sentLines.clear();
    QJsonObject params;
    params.insert(QStringLiteral("text"), QStringLiteral("dmesg"));
    params.insert(QStringLiteral("interval_ms"), 5);
    sendRequest(&sock, 21, QStringLiteral("xterm.transport.sendTypewriter"), params);

    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 21);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QCOMPARE(m_sink, QByteArray("dmesg\n")); // 同 sendLine：逐字汇聚 + 链路行尾
    QCOMPARE(m_sentLines, QStringList{ QStringLiteral("dmesg") });
    sock.disconnectFromServer();
}

// B 组正名：interrupt 工具 = 停打字机（那次调用收到 interrupted）+ \x03 直达链路
void AgentBridgeTest::interruptToolStopsTypingAndSendsCtrlC()
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
    sendRequest(&sock, 22, QStringLiteral("xterm.transport.sendLine"), params);

    QTest::qWait(70); // 只打出去一小截
    QVERIFY(m_bridge->isTyping());
    sendRequest(&sock, 23, QStringLiteral("xterm.transport.interrupt"), {});

    // 先到打字机那次的 interrupted 应答，后到 interrupt 本次的成功应答
    QJsonObject typingResp;
    QJsonObject interruptResp;
    for (int i = 0; i < 2; ++i) {
        const QJsonObject resp = waitReply(&sock);
        const int id = resp.value(QStringLiteral("id")).toInt();
        if (id == 22)
            typingResp = resp;
        else if (id == 23)
            interruptResp = resp;
    }
    QVERIFY(!typingResp.isEmpty());
    QVERIFY(typingResp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("interrupted")));
    QVERIFY(interruptResp.value(QStringLiteral("ok")).toBool());
    const QJsonObject data = interruptResp.value(QStringLiteral("result")).toObject();
    QCOMPARE(data.value(QStringLiteral("sent")).toString(), QStringLiteral("^C"));
    QCOMPARE(data.value(QStringLiteral("typing_interrupted")).toBool(), true);

    QVERIFY(!m_bridge->isTyping());
    QVERIFY(m_sentLines.isEmpty());           // 被打断不算完整一行
    QVERIFY(m_sink.endsWith(QByteArray(1, char(0x03)))); // \x03 已发链路
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

QJsonArray AgentBridgeTest::readAuditEntries() const
{
    QJsonArray out;
    QFile f(m_dir.filePath(QStringLiteral("logs/agent-audit.jsonl")));
    if (!f.open(QIODevice::ReadOnly))
        return out;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (!line.isEmpty())
            out.append(QJsonDocument::fromJson(line).object());
    }
    return out;
}

// M2.e 审计：调用落 jsonl（method/ok/duration_ms/params/ts），
// 同时发 agentToolCall 信号（McpMainHook 三态文案的数据源）
void AgentBridgeTest::auditJsonlRecordsCallsAndSignals()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    QSignalSpy spy(m_bridge, &AgentBridge::agentToolCall);
    QJsonObject params;
    params.insert(QStringLiteral("max_lines"), 5);
    sendRequest(&sock, 30, QStringLiteral("xterm.session.status"), params);
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    sock.disconnectFromServer();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("xterm.session.status"));
    QCOMPARE(spy.at(0).at(1).toBool(), true);
    QVERIFY(spy.at(0).at(2).toLongLong() >= 0);

    const QJsonArray entries = readAuditEntries();
    bool found = false;
    for (const QJsonValue &v : entries) {
        const QJsonObject e = v.toObject();
        if (e.value(QStringLiteral("event")).toString() == QLatin1String("call")
            && e.value(QStringLiteral("method")).toString()
                   == QLatin1String("xterm.session.status")) {
            found = true;
            QVERIFY(e.value(QStringLiteral("ok")).toBool());
            QVERIFY(e.contains(QStringLiteral("duration_ms")));
            QVERIFY(e.value(QStringLiteral("params")).toObject()
                        .contains(QStringLiteral("max_lines")));
            QVERIFY(!e.value(QStringLiteral("ts")).toString().isEmpty());
        }
    }
    QVERIFY(found);
}

// 失败调用也审计：未知工具的拒绝带 error 摘要
void AgentBridgeTest::auditRecordsFailures()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    sendRequest(&sock, 31, QStringLiteral("xterm.nope.nothere"), {});
    const QJsonObject resp = waitReply(&sock);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    sock.disconnectFromServer();

    const QJsonArray entries = readAuditEntries();
    bool found = false;
    for (const QJsonValue &v : entries) {
        const QJsonObject e = v.toObject();
        if (e.value(QStringLiteral("method")).toString()
                == QLatin1String("xterm.nope.nothere")) {
            found = true;
            QVERIFY(!e.value(QStringLiteral("ok")).toBool());
            QVERIFY(e.value(QStringLiteral("error")).toString()
                        .contains(QStringLiteral("未知工具")));
        }
    }
    QVERIFY(found);
}

// M2.e 限流：令牌桶容量 2、每秒 1 个时，连发第 3 个被拒（防循环刷屏）；
// 结束恢复默认，不影响其他用例
void AgentBridgeTest::rateLimitRejectsBurst()
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    QVERIFY(sock.waitForConnected(2000));

    m_bridge->setRateLimit(2, 1);
    int rejected = 0;
    int accepted = 0;
    for (int i = 0; i < 4; ++i) {
        sendRequest(&sock, 40 + i, QStringLiteral("xterm.session.status"), {});
        const QJsonObject resp = waitReply(&sock);
        if (resp.value(QStringLiteral("ok")).toBool())
            ++accepted;
        else if (resp.value(QStringLiteral("error")).toString()
                     .contains(QStringLiteral("rate limited")))
            ++rejected;
    }
    m_bridge->setRateLimit(30, 10); // 恢复默认，后续用例不受影响
    QCOMPARE(accepted, 2);
    QVERIFY(rejected >= 1);

    // 被拒事件也进审计（可观测）
    const QJsonArray entries = readAuditEntries();
    bool sawLimited = false;
    for (const QJsonValue &v : entries) {
        if (v.toObject().value(QStringLiteral("event")).toString()
                == QLatin1String("rate_limited"))
            sawLimited = true;
    }
    QVERIFY(sawLimited);
    sock.disconnectFromServer();
}

QTEST_MAIN(AgentBridgeTest)
#include "test_agentbridge.moc"
