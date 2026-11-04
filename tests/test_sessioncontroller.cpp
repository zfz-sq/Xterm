// M1.d SessionController 协作类测试：模式切换（栈页/按钮文案/状态栏/信号）、
// 连接入口守卫（未选串口提示）、发送汇聚点的未连接提示、MCP 数据适配器、
// SSH 状态文本占位、内核日志静音开关的未连接守卫。
// 认证/主机密钥弹窗是模态 exec()，无头环境不驱动（行为由 promptAuth* 保持
// 原实现迁移），真实链路连接/断开由既有 test_sshsession / test_serial 类覆盖。
// 组织名隔离：写盘类用例（setMode persist / applyCurrentProfile）落
// XtermM1dTest 独立配置目录，不碰真实 config.json。
#include <QComboBox>
#include <QCoreApplication>
#include <QJsonObject>
#include <QMainWindow>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTest>
#include <QToolButton>

#include "commandstore.h"
#include "hotkeymanager.h"
#include "kmsgmutetoggle.h"
#include "mcpmainhook.h"
#include "screenview.h"
#include "serialsession.h"
#include "sessioncontroller.h"
#include "sshsession.h"
#include "thememanager.h"
#include "transport.h"
#include "uibuilder.h"
#include "ui_mainwindow.h"

class TestSessionController : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void modeSwitchUpdatesUiAndSignals();
    void modePersistWritesStore();
    void connectWithoutPortShowsHint();
    void connectionSummarySerialFormat();
    void connectionSummarySshFormat();
    void sendRawDisconnectedNotifies();
    void agentStatusReflectsMode();
    void agentLineEndingPerMode();
    void sshStatusTextPlaceholders();
    void kmsgToggleGuardsDisconnectedClick();
    void kmsgVisibleOnlyInSerialMode();
    void agentSessionListShape();
    void agentSwitchSessionValidatesKind();
    void agentOpenSshEmptyHostRejected();
    void agentOpenSerialUnknownPortRejected();
    void agentCloseSessionValidatesKind();

private:
    Ui::MainWindow *m_ui = nullptr;
    QMainWindow *m_window = nullptr;
    CommandStore *m_store = nullptr;
    ThemeManager *m_theme = nullptr;
    McpMainHook *m_mcp = nullptr;
    HotkeyManager *m_hotkey = nullptr;
    UiBuilder *m_uiBuilder = nullptr;
    SessionController *m_session = nullptr;
};

void TestSessionController::initTestCase()
{
    // 写盘隔离：CommandStore::save() 走 AppConfigLocation，改应用名即可
    QCoreApplication::setApplicationName(QStringLiteral("XtermM1dTest"));
    QCoreApplication::setOrganizationName(QStringLiteral("Xterm"));
}

void TestSessionController::init()
{
    // 与 MainWindow 相同的创建/注入顺序（M1.a–M1.d 协作类接线）
    m_window = new QMainWindow;
    m_ui = new Ui::MainWindow;
    m_ui->setupUi(m_window);
    m_store = new CommandStore(m_window);
    m_theme = new ThemeManager(m_store, m_window);
    m_mcp = new McpMainHook(m_ui->statusbar, m_window);
    m_hotkey = new HotkeyManager(m_window);
    m_uiBuilder = new UiBuilder(m_window, m_ui, m_store, m_theme, m_hotkey, m_mcp,
                                m_window);
    m_session = new SessionController(m_window, m_ui, m_store, m_mcp, m_window);
    m_session->attachTerminal(m_uiBuilder->serialView(), m_uiBuilder->sshView(),
                              m_uiBuilder->commandBar());
    m_uiBuilder->setSessionController(m_session);
}

void TestSessionController::cleanup()
{
    delete m_window; // 协作类/部件均挂其下，一并销毁
    m_window = nullptr;
    m_ui = nullptr;
}

void TestSessionController::modeSwitchUpdatesUiAndSignals()
{
    using Mode = SessionController::Mode;
    QSignalSpy modeSpy(m_session, &SessionController::modeChanged);

    QCOMPARE(m_session->mode(), Mode::Serial); // 默认串口
    m_session->setMode(Mode::Ssh, false);

    // 只切显示：三个栈都翻到 SSH 页
    QCOMPARE(m_ui->terminalStack->currentIndex(), 1);
    QCOMPARE(m_ui->modeStack->currentIndex(), 1);
    QCOMPARE(m_ui->advancedStack->currentIndex(), 1);
    QCOMPARE(m_session->mode(), Mode::Ssh);

    // 模式按钮显示「将要切到」的方向（SSH 页显示「串口」，点击切回）
    QCOMPARE(m_ui->modeBtn->text(), QStringLiteral("串口"));
    // 状态栏随 connectionStateChanged/modeChanged 刷新为未连接文案
    QVERIFY(m_uiBuilder->kmsgButton()->isHidden()); // 静音开关只在串口页显示
    QCOMPARE(modeSpy.count(), 1);

    m_session->setMode(Mode::Serial, false);
    QCOMPARE(m_ui->terminalStack->currentIndex(), 0);
    QCOMPARE(m_ui->modeBtn->text(), QStringLiteral("SSH"));
    QVERIFY(!m_uiBuilder->kmsgButton()->isHidden());
    QCOMPARE(modeSpy.count(), 2);
}

void TestSessionController::modePersistWritesStore()
{
    m_session->setMode(SessionController::Mode::Ssh, true);
    QCOMPARE(m_store->mode(), QStringLiteral("ssh"));
    m_session->setMode(SessionController::Mode::Serial, true);
    QCOMPARE(m_store->mode(), QStringLiteral("serial"));
    // persist=false 只切显示，不动存储
    m_session->setMode(SessionController::Mode::Ssh, false);
    QCOMPARE(m_store->mode(), QStringLiteral("serial"));
}

void TestSessionController::connectWithoutPortShowsHint()
{
    // 不枚举串口（端口下拉为空），连接应提示选择端口且不产生副作用
    m_session->toggleConnect();
    QVERIFY(m_ui->statusbar->currentMessage().contains(QStringLiteral("请先选择串口端口")));
    QCOMPARE(m_session->serial()->state(), Transport::Disconnected);
    // 连接按钮仍处「连接」态（未进入 Connecting）
    QCOMPARE(m_ui->connectBtn->text(), QStringLiteral("连接"));
}

void TestSessionController::connectionSummarySerialFormat()
{
    // UiBuilder::setupConnectionCombos 已预填标准参数项且默认 115200 8N1，
    // 此处只补端口下拉（正常路径由 SessionController::refreshPorts 填）
    m_ui->portCombo->addItem(QStringLiteral("COM3: USB 串口"), QStringLiteral("COM3"));
    m_ui->portCombo->setCurrentIndex(0);

    QCOMPARE(m_session->connectionSummary(), QStringLiteral("COM3 @ 115200 8N1"));
}

void TestSessionController::connectionSummarySshFormat()
{
    m_session->setMode(SessionController::Mode::Ssh, false);
    m_ui->hostEdit->setText(QStringLiteral(" 192.168.11.150 "));
    m_ui->userEdit->setText(QStringLiteral("root"));
    // 端口框为空 → 按默认 22 处理，摘要不附带 :port
    QCOMPARE(m_session->connectionSummary(),
             QStringLiteral("root@192.168.11.150"));
}

void TestSessionController::sendRawDisconnectedNotifies()
{
    // 未连接时明确提示（状态栏 + 终端系统消息），不静默丢弃
    m_session->sendRaw(QByteArrayLiteral("ls\n"));
    QVERIFY(m_ui->statusbar->currentMessage().contains(QStringLiteral("未连接")));
    QVERIFY(m_uiBuilder->serialView()->recentText(50)
                .contains(QStringLiteral("未连接，命令未发送")));
}

void TestSessionController::agentStatusReflectsMode()
{
    QJsonObject st = m_session->agentStatus();
    QCOMPARE(st.value(QStringLiteral("mode")).toString(), QStringLiteral("serial"));
    QCOMPARE(st.value(QStringLiteral("connected")).toBool(), false);
    QCOMPARE(st.value(QStringLiteral("state")).toString(), QStringLiteral("disconnected"));
    QVERIFY(st.contains(QStringLiteral("summary")));

    m_session->setMode(SessionController::Mode::Ssh, false);
    st = m_session->agentStatus();
    QCOMPARE(st.value(QStringLiteral("mode")).toString(), QStringLiteral("ssh"));
}

void TestSessionController::agentLineEndingPerMode()
{
    QCOMPARE(m_session->agentLineEnding(), '\n'); // 串口：\n
    m_session->setMode(SessionController::Mode::Ssh, false);
    QCOMPARE(m_session->agentLineEnding(), '\r'); // SSH pty：\r
}

void TestSessionController::sshStatusTextPlaceholders()
{
    m_session->setMode(SessionController::Mode::Ssh, false);
    // 无样本/未连接：各读数回落 "--"
    const QString text = m_session->sshStatusText();
    QVERIFY(text.contains(QStringLiteral("CPU --")));
    QVERIFY(text.contains(QStringLiteral("--/--")));
}

void TestSessionController::kmsgToggleGuardsDisconnectedClick()
{
    SerialSession serial;
    KmsgMuteToggle toggle(&serial);
    QSignalSpy statusSpy(&toggle, &KmsgMuteToggle::statusMessageRequested);
    QSignalSpy cmdSpy(&toggle, &KmsgMuteToggle::commandRequested);

    toggle.click();
    // 未连接：只给状态栏提示，不翻转状态、不请求命令
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(cmdSpy.count(), 0);
    QVERIFY(!toggle.isMuted());
    QVERIFY(toggle.text().contains(QStringLiteral("正常")));
}

void TestSessionController::kmsgVisibleOnlyInSerialMode()
{
    // 默认串口页可见；SSH 页隐藏；切回恢复（updateModeUi 驱动）
    QVERIFY(!m_uiBuilder->kmsgButton()->isHidden());
    m_session->setMode(SessionController::Mode::Ssh, false);
    QVERIFY(m_uiBuilder->kmsgButton()->isHidden());
    m_session->setMode(SessionController::Mode::Serial, false);
    QVERIFY(!m_uiBuilder->kmsgButton()->isHidden());
}

// —— M2.f 会话适配器：list/switch/open/close 的本地可验路径（不真发起
// 连接：openSsh 验空 host 拒绝，openSerial 验不存在端口拒绝）——

void TestSessionController::agentSessionListShape()
{
    const QJsonArray sessions = m_session->agentSessionList();
    QCOMPARE(sessions.size(), 2);
    const QJsonObject s0 = sessions.at(0).toObject();
    QCOMPARE(s0.value(QStringLiteral("kind")).toString(), QStringLiteral("serial"));
    QCOMPARE(s0.value(QStringLiteral("active")).toBool(), true);
    QCOMPARE(s0.value(QStringLiteral("state")).toString(), QStringLiteral("disconnected"));
    QVERIFY(s0.contains(QStringLiteral("summary")));
    const QJsonObject s1 = sessions.at(1).toObject();
    QCOMPARE(s1.value(QStringLiteral("kind")).toString(), QStringLiteral("ssh"));
    QCOMPARE(s1.value(QStringLiteral("active")).toBool(), false);
    QVERIFY(s1.contains(QStringLiteral("summary")));
}

void TestSessionController::agentSwitchSessionValidatesKind()
{
    QJsonObject out;
    QVERIFY(!m_session->agentSwitchSession(QStringLiteral("telnet"), &out).isEmpty());
    QCOMPARE(m_session->mode(), SessionController::Mode::Serial); // 拒绝不改状态

    QVERIFY(m_session->agentSwitchSession(QStringLiteral("ssh"), &out).isEmpty());
    QCOMPARE(m_session->mode(), SessionController::Mode::Ssh);
    QCOMPARE(out.value(QStringLiteral("mode")).toString(), QStringLiteral("ssh"));
    QVERIFY(out.contains(QStringLiteral("sessions")));

    // current 详情：mode + 两条链路
    const QJsonObject cur = m_session->agentSessionCurrent();
    QCOMPARE(cur.value(QStringLiteral("mode")).toString(), QStringLiteral("ssh"));
    QCOMPARE(cur.value(QStringLiteral("sessions")).toArray().size(), 2);
}

void TestSessionController::agentOpenSshEmptyHostRejected()
{
    m_ui->hostEdit->clear();
    m_ui->userEdit->clear();
    QJsonObject out;
    const QString err = m_session->agentOpenSsh({}, &out);
    // 未配置 host：直接拒绝（若 ssh 栈不可用则报 availability，同样非空）
    QVERIFY(!err.isEmpty());
    QCOMPARE(m_ui->hostEdit->text(), QString()); // 未污染参数栏
    QCOMPARE(m_session->mode(), SessionController::Mode::Serial);
}

void TestSessionController::agentOpenSerialUnknownPortRejected()
{
    QJsonObject out;
    const QString err = m_session->agentOpenSerial(
        QJsonObject{{QStringLiteral("port"), QStringLiteral("NOPE_XYZ")}}, &out);
    QVERIFY(err.contains(QStringLiteral("不存在")));
    QVERIFY(err.contains(QStringLiteral("NOPE_XYZ")));
    // 可用端口清单里一定找不到它，选择保持未变（未选中任何项）
    QCOMPARE(m_session->mode(), SessionController::Mode::Serial);
}

void TestSessionController::agentCloseSessionValidatesKind()
{
    QJsonObject out;
    QVERIFY(!m_session->agentCloseSession(QStringLiteral("telnet"), &out).isEmpty());

    // 未连接时 close 是无害空操作（不报错，closed=false）
    QVERIFY(m_session->agentCloseSession(QString(), &out).isEmpty());
    QCOMPARE(out.value(QStringLiteral("closed")).toBool(), false);
    QVERIFY(out.contains(QStringLiteral("sessions")));

    QVERIFY(m_session->agentCloseSession(QStringLiteral("serial"), &out).isEmpty());
    QCOMPARE(out.value(QStringLiteral("closed")).toBool(), false);
}

QTEST_MAIN(TestSessionController) // 组装 QMainWindow 及全套部件，需 QApplication
#include "test_sessioncontroller.moc"
