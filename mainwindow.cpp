#include "mainwindow.h"

#include <QCloseEvent>
#include <QEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QStatusBar>

#include "agentbridge.h"
#include "commandbar.h"
#include "hotkeymanager.h"
#include "mcpmainhook.h"
#include "screenview.h"
#include "sshsession.h"
#include "thememanager.h"
#include "uibuilder.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    // ---- 协作类创建（顺序即依赖：M1.a–M1.d，见各头文件）----
    m_store = new CommandStore(this);
    m_theme = new ThemeManager(m_store, this);
    // MCP 挂钩协作类：拥有桥接、角标按钮、状态/锁定标签；
    // 按钮稍后挂命令栏收起态的侧边按钮组（挂载点归主窗口）。
    m_mcp = new McpMainHook(ui->statusbar, this);
    // 全局快捷键协作类须在标题栏之前创建：标题栏「工具」菜单
    // 直接取用其 action（同 M1.a 的连接/创建顺序教训）
    m_hotkey = new HotkeyManager(this);

    m_uiBuilder = new UiBuilder(this, ui, m_store, m_theme, m_hotkey, m_mcp, this);
    m_uiBuilder->commandBar()->addSideButton(m_mcp->button());

    m_session = new SessionController(this, ui, m_store, m_mcp, this);
    m_session->attachTerminal(m_uiBuilder->serialView(), m_uiBuilder->sshView(),
                              m_uiBuilder->commandBar());
    m_uiBuilder->setSessionController(m_session);

    connectSignals();

    // 主题切换后的级联刷新（终端页/图标/命令栏）由 UiBuilder 承接
    connect(m_theme, &ThemeManager::themeChanged, m_uiBuilder, &UiBuilder::onThemeToggled);

    // ---- 启动恢复 ----
    m_store->load();
    // 首启两步：applyNow 刷全局 QSS/图标主题，onThemeToggled 刷部件级联
    // （终端两页底色、标题栏系统按钮字形、命令栏/MCP 图标）——缺第二步
    // 会出现外壳已切亮、终端仍旧黑的错位
    m_theme->applyNow();
    m_uiBuilder->onThemeToggled(m_theme->isDark());
    // 恢复上次保存的命令栏收起状态（侧边按钮组形态）、终端字号/字体族、
    // 关键词高亮规则（均为全局设置，两页一致）
    m_uiBuilder->commandBar()->setCollapsed(m_store->commandBarCollapsed());
    m_uiBuilder->restoreTerminalFontSettings();
    m_uiBuilder->applyKeywords(m_store->keywords());
    ui->advancedBar->hide(); // 高级参数默认收起
    m_session->refreshPorts();
    m_session->reloadProfiles(m_store->lastProfile());
    m_session->setMode(m_store->mode() == QStringLiteral("ssh") ? Mode::Ssh : Mode::Serial,
                       false);

    // ---- MCP 桥：数据适配器归 SessionController，组装成 Hooks 注入 ----
    AgentBridge::Hooks hooks;
    hooks.status = [this] { return m_session->agentStatus(); };
    hooks.recentText = [this](int maxLines) { return m_session->agentRecentText(maxLines); };
    // wait 的行锚点：命中只看「挂起之后新增」的行，高频刷屏下才不会误判
    hooks.recentLineCount = [this] { return m_session->agentRecentLineCount(); };
    hooks.recentTextFrom = [this](int fromLine, int maxLines) {
        return m_session->agentRecentTextFrom(fromLine, maxLines);
    };
    hooks.sendLine = [this](const QString &text) { return m_session->agentSendLine(text); };
    hooks.sendBytes = [this](const QByteArray &bytes) { return m_session->agentSendBytes(bytes); };
    // 打字机用的链路行尾与「整行发完」回调（记输入历史）
    hooks.lineEnding = [this] { return m_session->agentLineEnding(); };
    hooks.lineSent = [this](const QString &line) {
        ScreenView *view = (m_session->mode() == Mode::Serial)
                               ? m_uiBuilder->serialView() : m_uiBuilder->sshView();
        if (view)
            view->rememberSentLine(line);
    };
    m_mcp->start(hooks);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::connectSignals()
{
    // 终端键盘直通 / 预设按钮
    // 键盘在 eventFilter 里按「AI 锁定」策略整体拦截；能走到这里的
    // 用户输入只剩粘贴等异步路径——AI 打字期间同样锁定并提示。
    for (ScreenView *view : { m_uiBuilder->serialView(), m_uiBuilder->sshView() }) {
        view->installEventFilter(this);
        connect(view, &ScreenView::sendData, this, [this](const QByteArray &bytes) {
            if (m_mcp && m_mcp->isTyping()) {
                ui->statusbar->showMessage(tr("AI 正在操作，输入已锁定 —— 按 Ctrl+C 打断"), 3000);
                return;
            }
            m_session->sendRaw(bytes);
        });
    }
    // SSH 终端尺寸变化 → pty 大小同步
    connect(m_uiBuilder->sshView(), &ScreenView::terminalSizeChanged, this,
            [this](int cols, int rows) { m_session->ssh()->setPtySize(cols, rows); });
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // AI 打字机输入期间锁定终端键盘：比 ScreenView 更早拦截，
    // 只有 Ctrl+C 放行为「打断 AI」（在这里直接处理，不转发成 0x03，
    // 也不会被 ScreenView 的「有选区 Ctrl+C=复制」分支吃掉）。
    if (m_mcp && (watched == m_uiBuilder->serialView() || watched == m_uiBuilder->sshView())
        && event->type() == QEvent::KeyPress && m_mcp->isTyping()) {
        const auto *key = static_cast<const QKeyEvent *>(event);
        const bool ctrlC = (key->modifiers() & Qt::ControlModifier)
                           && !(key->modifiers() & Qt::ShiftModifier)
                           && !(key->modifiers() & Qt::AltModifier)
                           && key->key() == Qt::Key_C;
        if (ctrlC) {
            if (m_mcp->interruptTyping())
                ui->statusbar->showMessage(tr("已打断 AI 输入，恢复人工操作"), 3000);
            return true; // 不发给设备：用户意图是打断 AI，不是杀远端进程
        }
        if (key->key() != Qt::Key_Control && key->key() != Qt::Key_Shift
            && key->key() != Qt::Key_Alt && key->key() != Qt::Key_Meta) {
            // 修饰键本身放行（避免组合键状态错乱），其余全部吞掉
            ui->statusbar->showMessage(
                tr("AI 正在操作，输入已锁定 —— 按 Ctrl+C 打断"), 2000);
        }
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 配置同步落盘 + 两条链路有序拆除（原 closeEvent 编排迁入）
    m_session->shutdownForClose();
    event->accept();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange) {
        // 窗口化 6px 留白 / 最大化贴满（外围与终端网格内边距一并处理）
        m_uiBuilder->applyWindowMargins(isMaximized());
    }
    QMainWindow::changeEvent(event);
}
