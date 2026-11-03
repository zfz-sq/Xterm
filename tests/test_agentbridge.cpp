#include <QtTest>

#include <QCoreApplication>
#include <QFile>
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
    void typingCompletes();
    void typingInterrupted();
    void typingWaits();
    void waitTimesOutWhenNeedleNotNew();
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
    int m_lineCount = 0;       // recentLineCount 可变（行锚点）
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
        [this](int, int) { return m_freshText; },
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
    sendRequest(&sock, 1, QStringLiteral("send_line"), params);

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
    sendRequest(&sock, 2, QStringLiteral("send_line"), params);

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
    sendRequest(&sock, 3, QStringLiteral("send_line"), params);

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
    sendRequest(&sock, 5, QStringLiteral("send_line"), params);

    const QJsonObject resp = waitReply(&sock);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("timed_out")).toBool());
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
    sendRequest(&sock, 4, QStringLiteral("send_line"), params);

    const QJsonObject resp = waitReply(&sock);
    QCOMPARE(resp.value(QStringLiteral("id")).toInt(), 4);
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QCOMPARE(m_directLines, QStringList{ QStringLiteral("clear -a") });
    QVERIFY(m_sink.isEmpty()); // 本地命令不进打字机、不出字节
    sock.disconnectFromServer();
}

QTEST_MAIN(AgentBridgeTest)
#include "test_agentbridge.moc"
