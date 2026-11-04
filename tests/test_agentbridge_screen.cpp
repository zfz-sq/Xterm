#include <QtTest>

#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>

#include "agentbridge.h"
#include "screenbridge.h"

/*
 * ScreenBridge（M2.d）：xterm.screen.snapshot / xterm.screen.find 两件工具。
 * 静态函数直接喂 TermScreen（真 VT 序列，验颜色段/OSC 8 链接/绝对行号）；
 * 工具级走独立管道，fake 后端回放预设文本。waitForPattern（AgentBridge
 * 内置）的正则挂起/超时在 test_agentbridge.cpp 里覆盖。
 */
class AgentBridgeScreenTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void snapshotParsesStyledLines();
    void snapshotTrimsToWindowWithHistory();
    void altScreenUsesOwnRows();
    void findMatchesCoversModes();

    void toolsListIncludesScreenTools();
    void snapshotToolReadsBackend();
    void snapshotToolFailsWithoutView();
    void findToolScansTail();
    void findToolRejectsBadPattern();

private:
    QJsonObject call(const QString &method, const QJsonObject &params = {});

    AgentBridge *m_bridge = nullptr;
    QTemporaryDir m_dir;
    QString m_pipe;
    QString m_token;
    ScreenBridge m_screenBridge; // 工具注册进 bridge 的注册表
    TermScreen m_screen;   // fake「活动视图」的屏幕模型（snapshot 工具数据源）
    bool m_hasView = true; // false 模拟无活动终端
    qint64 m_lineCount = 0;
    QString m_tailText;    // find 工具的扫描窗回放
};

void AgentBridgeScreenTest::initTestCase()
{
    m_pipe = QStringLiteral("XtermAgentScreenTest-%1").arg(qApp->applicationPid());
    m_bridge = new AgentBridge(this, m_pipe);

    AgentBridge::Hooks hooks; // Screen 用例只碰自己的后端，会话钩子留空
    ScreenBridge::ScreenHooks backend;
    backend.screen = [this]() -> const TermScreen * {
        return m_hasView ? &m_screen : nullptr;
    };
    backend.lineCount = [this] { return m_lineCount; };
    backend.textFrom = [this](qint64, int) { return m_tailText; };
    m_screenBridge.setBackend(backend);
    m_screenBridge.registerTools(&hooks);
    m_bridge->start(hooks, m_dir.filePath(QStringLiteral("agent.json")));

    QFile f(m_dir.filePath(QStringLiteral("agent.json")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    m_token = QJsonDocument::fromJson(f.readAll()).object()
                  .value(QStringLiteral("token")).toString();
    QVERIFY(!m_token.isEmpty());
}

QJsonObject AgentBridgeScreenTest::call(const QString &method, const QJsonObject &params)
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

// 静态 snapshot：SGR 颜色段 + OSC 8 链接都在结构里，默认样式行不带 runs
void AgentBridgeScreenTest::snapshotParsesStyledLines()
{
    TermScreen s;
    s.resize(20, 4);
    s.feed("\x1b[31;1mred\x1b[0m plain\r\n");
    s.feed("\x1b]8;;https://example.com\x1b\\docs\x1b]8;;\x1b\\ and more\r\n");
    s.feed("third line");

    const QJsonObject d = ScreenBridge::snapshot(s, 4);
    QCOMPARE(d.value(QStringLiteral("cols")).toInt(), 20);
    QCOMPARE(d.value(QStringLiteral("rows")).toInt(), 4);
    QVERIFY(!d.value(QStringLiteral("alt_screen")).toBool());
    QCOMPARE(d.value(QStringLiteral("total_lines")).toInt(), 4);
    QCOMPARE(d.value(QStringLiteral("start_abs")).toInt(), 0);

    const QJsonArray lines = d.value(QStringLiteral("lines")).toArray();
    QCOMPARE(lines.size(), 4);

    const QJsonObject row0 = lines.at(0).toObject();
    QCOMPARE(row0.value(QStringLiteral("text")).toString(), QStringLiteral("red plain"));
    const QJsonArray runs = row0.value(QStringLiteral("runs")).toArray();
    QCOMPARE(runs.size(), 1); // 只有红色加粗的 "red" 是非默认段
    const QJsonObject run = runs.at(0).toObject();
    QCOMPARE(run.value(QStringLiteral("start")).toInt(), 0);
    QCOMPARE(run.value(QStringLiteral("end")).toInt(), 2);
    QVERIFY(run.value(QStringLiteral("bold")).toBool());
    QVERIFY(run.value(QStringLiteral("fg")).toString().startsWith(QLatin1Char('#')));
    QVERIFY(!run.contains(QStringLiteral("bg"))); // 默认背景不记录

    const QJsonObject row1 = lines.at(1).toObject();
    QCOMPARE(row1.value(QStringLiteral("text")).toString(), QStringLiteral("docs and more"));
    const QJsonArray links = row1.value(QStringLiteral("links")).toArray();
    QCOMPARE(links.size(), 1);
    const QJsonObject link = links.at(0).toObject();
    QCOMPARE(link.value(QStringLiteral("start")).toInt(), 0);
    QCOMPARE(link.value(QStringLiteral("end")).toInt(), 3); // "docs" 四格
    QCOMPARE(link.value(QStringLiteral("uri")).toString(),
             QStringLiteral("https://example.com"));
    QVERIFY(!row1.contains(QStringLiteral("runs"))); // 全默认样式不带 runs

    QCOMPARE(lines.at(2).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("third line"));
    // 全空行：lineText 的去尾空白保留一格（与 lineToPlain 同款），判空白用 trimmed
    QVERIFY(lines.at(3).toObject().value(QStringLiteral("text")).toString()
                .trimmed().isEmpty());
}

// 滚动历史：max_lines 截尾窗口，行号是 dropped+history+rows 的绝对行号。
// 注意 TermScreen::resize 的 rows 有最小 4 行钳制，滚屏至少要写 5 行
void AgentBridgeScreenTest::snapshotTrimsToWindowWithHistory()
{
    TermScreen s;
    s.resize(10, 4);
    s.feed("l1\r\nl2\r\nl3\r\nl4\r\nl5"); // l1 滚进历史，屏上 l2..l5

    QCOMPARE(s.historySize(), 1);
    const QJsonObject d = ScreenBridge::snapshot(s, 2);
    QCOMPARE(d.value(QStringLiteral("total_lines")).toInt(), 5);
    QCOMPARE(d.value(QStringLiteral("start_abs")).toInt(), 3); // 尾部两行 = abs 3,4
    const QJsonArray lines = d.value(QStringLiteral("lines")).toArray();
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0).toObject().value(QStringLiteral("abs")).toInt(), 3);
    QCOMPARE(lines.at(0).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("l4"));
    QCOMPARE(lines.at(1).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("l5"));
}

// 备用屏：历史不参与，行号就是屏内行号（alt_screen=true 披露）
void AgentBridgeScreenTest::altScreenUsesOwnRows()
{
    TermScreen s;
    s.resize(20, 5); // rows 最小 4，取 5 避开钳制语义
    s.feed("\x1b[?1049h"); // 进备用屏
    s.feed("altline");

    const QJsonObject d = ScreenBridge::snapshot(s, 10);
    QVERIFY(d.value(QStringLiteral("alt_screen")).toBool());
    QCOMPARE(d.value(QStringLiteral("total_lines")).toInt(), 5);
    const QJsonArray lines = d.value(QStringLiteral("lines")).toArray();
    QCOMPARE(lines.at(0).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("altline"));
}

// 静态 findMatches：子串/正则/大小写不敏感/空匹配不计/非法正则报出
void AgentBridgeScreenTest::findMatchesCoversModes()
{
    bool bad = true;
    const QString text = QStringLiteral("alpha beta\nerror 42\nfatal: retry\nno err");

    // 子串，大小写不敏感
    bad = true;
    QJsonArray hits = ScreenBridge::findMatches(text, 100, QStringLiteral("BETA"),
                                                false, 10, &bad);
    QVERIFY(!bad);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.at(0).toObject().value(QStringLiteral("abs")).toInt(), 100);
    QCOMPARE(hits.at(0).toObject().value(QStringLiteral("col")).toInt(), 6);
    QCOMPARE(hits.at(0).toObject().value(QStringLiteral("match")).toString(),
             QStringLiteral("beta"));

    // 正则：err|fatal 命中三处（error、fatal、err）
    hits = ScreenBridge::findMatches(text, 100, QStringLiteral("err(or)?|fatal"),
                                     true, 10, &bad);
    QVERIFY(!bad);
    QCOMPARE(hits.size(), 3);
    QCOMPARE(hits.at(1).toObject().value(QStringLiteral("abs")).toInt(), 102);

    // 空匹配不计次（"x*" 型防误报）
    hits = ScreenBridge::findMatches(text, 100, QStringLiteral("x*"), true, 10, &bad);
    QVERIFY(!bad);
    QCOMPARE(hits.size(), 0);

    // 非法正则：badPattern 出参
    hits = ScreenBridge::findMatches(text, 100, QStringLiteral("([oops"), true, 10, &bad);
    QVERIFY(bad);
    QCOMPARE(hits.size(), 0);

    // 空模式：直接空结果（不报错，工具层早已拦截）
    bad = true;
    hits = ScreenBridge::findMatches(text, 100, QString(), false, 10, &bad);
    QVERIFY(!bad);
    QCOMPARE(hits.size(), 0);

    // maxHits 截断
    hits = ScreenBridge::findMatches(text, 100, QStringLiteral("e"), false, 2, &bad);
    QCOMPARE(hits.size(), 2);
}

void AgentBridgeScreenTest::toolsListIncludesScreenTools()
{
    const QJsonObject resp = call(QStringLiteral("tools.list"));
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonArray tools = resp.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    QStringList names;
    for (const QJsonValue &v : tools)
        names << v.toObject().value(QStringLiteral("name")).toString();
    QVERIFY(names.contains(QStringLiteral("xterm.screen.snapshot")));
    QVERIFY(names.contains(QStringLiteral("xterm.screen.find")));
}

// 工具级 snapshot：fake 后端的 TermScreen 原样进结果（max_lines 默认屏行数）
void AgentBridgeScreenTest::snapshotToolReadsBackend()
{
    m_screen = TermScreen{};
    m_screen.resize(20, 4);
    m_screen.feed("hello screen");
    m_hasView = true;

    const QJsonObject resp = call(QStringLiteral("xterm.screen.snapshot"));
    QVERIFY2(resp.value(QStringLiteral("ok")).toBool(),
             qPrintable(resp.value(QStringLiteral("error")).toString()));
    const QJsonObject d = resp.value(QStringLiteral("result")).toObject();
    QCOMPARE(d.value(QStringLiteral("rows")).toInt(), 4);
    const QJsonArray lines = d.value(QStringLiteral("lines")).toArray();
    QCOMPARE(lines.size(), 4);
    QCOMPARE(lines.at(0).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("hello screen"));
}

// 无活动视图（比如还没建过终端）：ok=false 报「没有活动终端」
void AgentBridgeScreenTest::snapshotToolFailsWithoutView()
{
    m_hasView = false;

    const QJsonObject resp = call(QStringLiteral("xterm.screen.snapshot"));
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("活动终端")));
    m_hasView = true;
}

// 工具级 find：扫描窗取尾部（总行数超窗口时从 total-400 起），
// 命中带绝对行号/列/整行
void AgentBridgeScreenTest::findToolScansTail()
{
    m_lineCount = 500; // > 默认扫描窗 400，窗口起点 = 500-400 = 100
    m_tailText = QStringLiteral("line 100\nline 101 gamma\nline 102 beta");

    QJsonObject params;
    params.insert(QStringLiteral("pattern"), QStringLiteral("gamma|beta"));
    params.insert(QStringLiteral("regex"), true);
    const QJsonObject resp = call(QStringLiteral("xterm.screen.find"), params);
    QVERIFY2(resp.value(QStringLiteral("ok")).toBool(),
             qPrintable(resp.value(QStringLiteral("error")).toString()));
    const QJsonObject d = resp.value(QStringLiteral("result")).toObject();
    const QJsonArray hits = d.value(QStringLiteral("hits")).toArray();
    QCOMPARE(hits.size(), 2);
    const QJsonObject h0 = hits.at(0).toObject();
    QCOMPARE(h0.value(QStringLiteral("abs")).toInt(), 101); // startLine=100 + 行 1
    QCOMPARE(h0.value(QStringLiteral("match")).toString(), QStringLiteral("gamma"));
    QCOMPARE(h0.value(QStringLiteral("line")).toString(), QStringLiteral("line 101 gamma"));
    QCOMPARE(hits.at(1).toObject().value(QStringLiteral("abs")).toInt(), 102);
    QVERIFY(!d.value(QStringLiteral("truncated")).toBool());
}

// 非法正则：当场报错（不吞成空命中），错误里带 pattern
void AgentBridgeScreenTest::findToolRejectsBadPattern()
{
    m_lineCount = 5;
    m_tailText = QStringLiteral("x");

    QJsonObject params;
    params.insert(QStringLiteral("pattern"), QStringLiteral("([bad"));
    params.insert(QStringLiteral("regex"), true);
    const QJsonObject resp = call(QStringLiteral("xterm.screen.find"), params);
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("正则无效")));
}

QTEST_MAIN(AgentBridgeScreenTest)
#include "test_agentbridge_screen.moc"
