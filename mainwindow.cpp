#include "mainwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStatusBar>
#include <QTimer>
#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbt.h>
#endif

#include "agentbridge.h"
#include "commandbar.h"
#include "hotkeymanager.h"
#include "mcpmainhook.h"
#include "screenview.h"
#include "serialsession.h"
#include "sftpclient.h"
#include "sshsession.h"
#include "thememanager.h"
#include "transport.h"
#include "uibuilder.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    // 参数栏渐进精简的最小宽度兜底（用户定稿 2026-11-16）：折叠态布局的最小需求，
    // 低于此宽标题栏会真正放不下；渐进裁剪档位见 UiBuilder::updateConnectionBarCompact
    setMinimumWidth(720);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    // ---- 协作类创建（顺序即依赖：M1.a–M1.d，见各头文件）----
    m_store = new CommandStore(this);
    m_theme = new ThemeManager(m_store, this);
    // MCP 挂钩协作类：拥有桥接、角标按钮、状态/锁定标签；
    // MCP 状态标签等仍由主窗口挂状态栏；MCP 提示词内容现住侧边栏第三页（UiBuilder 注入），
    // 原收起态侧边按钮组已废弃，不再挂载 MCP 按钮。
    m_mcp = new McpMainHook(ui->statusbar, this);
    // 全局快捷键协作类（F5 连接 / F9 切模式 / F10 文件管理，行为源归 HotkeyManager）
    m_hotkey = new HotkeyManager(this);

    m_uiBuilder = new UiBuilder(this, ui, m_store, m_theme, m_hotkey, m_mcp, this);

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
    // 恢复上次保存的侧边栏显隐（标题栏按钮勾态同步）、终端字号/字体族、
    // 关键词高亮规则（均为全局设置，两页一致）
    m_uiBuilder->applySidebarVisible(m_store->sidebarVisible());
    m_uiBuilder->applyStatusBarVisible(m_store->statusBarVisible());
    m_uiBuilder->restoreTerminalFontSettings();
    m_uiBuilder->applyKeywords(m_store->keywords());
    // OSC 52 策略（全局，两页一致）：ask / allow / deny
    const QString osc52 = m_store->osc52Policy();
    const auto osc52Policy = (osc52 == QStringLiteral("allow"))
                                 ? ScreenView::Osc52Policy::Allow
                                 : ((osc52 == QStringLiteral("deny"))
                                        ? ScreenView::Osc52Policy::Deny
                                        : ScreenView::Osc52Policy::Ask);
    m_uiBuilder->serialView()->setOsc52Policy(osc52Policy);
    m_uiBuilder->sshView()->setOsc52Policy(osc52Policy);
    // 选区即复制（全局，两页一致，默认关）
    for (ScreenView *view : { m_uiBuilder->serialView(), m_uiBuilder->sshView() })
        view->setSelectToCopy(m_store->selectToCopy());
    // 滚动历史环形上限（全局，两页一致，默认 10000 行）
    for (ScreenView *view : { m_uiBuilder->serialView(), m_uiBuilder->sshView() })
        view->setMaxHistory(m_store->maxScrollback());
    ui->advancedBar->hide(); // 高级参数默认收起
    m_session->refreshPorts();
    m_session->reloadProfiles(m_store->lastProfile());
    m_session->setMode(m_store->mode() == QStringLiteral("ssh") ? Mode::Ssh : Mode::Serial,
                       false);
    m_uiBuilder->serialView()->setSlowPaste(m_store->serialSlowPaste());
    m_uiBuilder->serialView()->setSlowPasteDelay(m_store->serialSlowPasteDelay());
    m_session->setAutoReconnect(m_store->serialAutoReconnect());

    // ---- MCP 桥：数据适配器归 SessionController，组装成 Hooks 注入 ----
    AgentBridge::Hooks hooks;
    hooks.status = [this] { return m_session->agentStatus(); };
    hooks.recentText = [this](int maxLines) { return m_session->agentRecentText(maxLines); };
    // wait 的绝对行锚点（M3.4）：环形丢行后也不漂移，命中只看新增行
    hooks.recentLineCount = [this] { return m_session->agentRecentLineCount(); };
    hooks.recentTextFrom = [this](qint64 fromLine, int maxLines) {
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
    // ---- M2.b：xterm.fs.* 工具后端。独立 SftpClient 挂终端的 SshSession
    // （共享 SFTP 通道，与文件管理器互不干扰）；沙箱根随 SSH 连接状态解析/清空 ----
    auto *agentFs = new SftpClient(m_session->ssh(), this);
    m_fs.setBackend(FsBridge::hooksForSftp(agentFs));
    m_fs.setAllowWrite(m_store->mcpAllowWrite());
    m_fs.registerTools(&hooks);
    connect(m_session, &SessionController::connectionStateChanged, this, [this] {
        if (m_session->ssh()->state() == Transport::Connected)
            m_fs.refreshRoot(); // 解析失败（通道未就绪）会清空根，工具不可用
        else
            m_fs.clearRoot();
    });
    // ---- M2.c：xterm.host.* 工具后端。cpu/mem 走 stats 通道缓存帧
    // （HostStats 挂在 SessionController），disk/uptime/top 按需远端命令
    // （独立 SftpClient 适配器，与文件管理器/FsBridge 互不干扰）----
    auto *agentHostExec = new SftpClient(m_session->ssh(), this);
    m_host.setBackend(HostBridge::hooksForStats(m_session->hostStats(), agentHostExec));
    m_host.registerTools(&hooks);
    // ---- M2.d：xterm.screen 扩展工具。snapshot 直读当前活动终端的
    // 屏幕模型，find 复用 wait 同源的绝对行号文本源 ----
    m_screen.setBackend({
        [this] { return m_session->agentActiveScreen(); },
        [this] { return m_session->agentRecentLineCount(); },
        [this](qint64 fromLine, int maxLines) {
            return m_session->agentRecentTextFrom(fromLine, maxLines);
        },
    });
    m_screen.registerTools(&hooks);
    // ---- M2.f：xterm.session 扩展工具。动作直接落在 SessionController
    // 的 agent* 适配器上（参数写回参数栏，UI 是唯一真相源）----
    m_sessionTools.setBackend({
        [this] { return m_session->agentSessionList(); },
        [this] { return m_session->agentSessionCurrent(); },
        [this](const QString &kind, QJsonObject *out) {
            return m_session->agentSwitchSession(kind, out);
        },
        [this](const QJsonObject &params, QJsonObject *out) {
            return m_session->agentOpenSsh(params, out);
        },
        [this](const QJsonObject &params, QJsonObject *out) {
            return m_session->agentOpenSerial(params, out);
        },
        [this](const QString &kind, QJsonObject *out) {
            return m_session->agentCloseSession(kind, out);
        },
    });
    m_sessionTools.registerTools(&hooks);
    m_mcp->start(hooks, m_store->mcpAllowWrite());

    // 无边框窗口四边四角缩放(Qt 层,见 windowEdgeFlagsAt/onEdgeCursorTick):
    // 事件过滤只接管边缘按下缩放,光标反馈由轮询维护(悬停 MouseMove 不派发到
    // 未开 tracking 的控件,纯事件方案在「同控件内从中心移向边缘」时有盲区)
    qApp->installEventFilter(this);
    auto *edgeCursorTimer = new QTimer(this);
    edgeCursorTimer->setInterval(60);
    connect(edgeCursorTimer, &QTimer::timeout, this, &MainWindow::onEdgeCursorTick);
    edgeCursorTimer->start();
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
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

    // OSC 8 超链接与 OSC 52 授权（M3.2）：链接分发归会话层（http(s) 交系统、
    // file:// 先本地后 SFTP）；「以后自动允许」落配置并同步本页策略
    for (ScreenView *view : { m_uiBuilder->serialView(), m_uiBuilder->sshView() }) {
        connect(view, &ScreenView::linkActivated, m_session, &SessionController::openLink);
        connect(view, &ScreenView::osc52AlwaysAllowRequested, this, [this, view] {
            m_store->setOsc52Policy(QStringLiteral("allow"));
            m_store->save();
            view->setOsc52Policy(ScreenView::Osc52Policy::Allow);
        });
        // 右键菜单切「选区即复制」：落配置并同步两页
        connect(view, &ScreenView::selectToCopyToggled, this, [this](bool on) {
            m_store->setSelectToCopy(on);
            m_store->save();
            m_uiBuilder->serialView()->setSelectToCopy(on);
            m_uiBuilder->sshView()->setSelectToCopy(on);
        });
    }

    // 串口慢速粘贴进度与取消状态提示
    connect(m_uiBuilder->serialView(), &ScreenView::slowPasteProgress, this,
            [this](int current, int total) {
                ui->statusbar->showMessage(
                    tr("正在慢速粘贴：%1/%2 行（按 Esc 或 Ctrl+C 取消）").arg(current).arg(total));
            });
    connect(m_uiBuilder->serialView(), &ScreenView::slowPasteFinished, this, [this] {
        ui->statusbar->showMessage(tr("慢速粘贴已完成"), 3000);
    });
    connect(m_uiBuilder->serialView(), &ScreenView::slowPasteCancelled, this, [this] {
        ui->statusbar->showMessage(tr("慢速粘贴已取消"), 3000);
    });

    // 静默检测与任务栏闪烁提醒（串口与 SSH 均支持）
    connect(m_session->serial(), &SerialSession::received, this, &MainWindow::onDataReceivedForAlert);
    connect(m_session->ssh(), &SshSession::received, this, &MainWindow::onDataReceivedForAlert);
    connect(m_session->serial(), &SerialSession::opened, this, [this] {
        m_lastDataTimeMs = QDateTime::currentMSecsSinceEpoch();
    });
    connect(m_session->ssh(), &SshSession::opened, this, [this] {
        m_lastDataTimeMs = QDateTime::currentMSecsSinceEpoch();
    });
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // 无边框窗口边缘按下缩放(光标反馈见 onEdgeCursorTick);
    // 其余事件走原有 AI 锁定键盘拦截逻辑
    if (event->type() == QEvent::MouseButtonPress) {
        // 过滤器挂在 qApp 上,会看到全部顶层窗口的事件:边缘缩放只接管
        // 发生在本窗口树内的按下。否则其他窗口(SFTP 文件管理/传输队列/
        // 弹窗)只要坐标落在本窗口边缘带或窗口外(windowEdgeFlagsAt 的
        // 单向判定无下界),点击就被吞掉换成本窗口 resize——表现为
        // SFTP 窗口的关闭按钮点不动
        const auto *widget = qobject_cast<QWidget *>(watched);
        const auto *mouse = static_cast<const QMouseEvent *>(event);
        if (widget && widget->window() == this && mouse->button() == Qt::LeftButton) {
            const Qt::Edges edges = windowEdgeFlagsAt(mouse->globalPosition().toPoint());
            QWindow *win = windowHandle();
            if (edges != 0 && win && win->startSystemResize(edges))
                return true; // 系统接管 resize 循环,期间光标由系统维护
        }
    }
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

// 无边框窗口边缘命中:窗口外围 8 逻辑像素带,按落点返回对应边/角组合;
// 伪最大化期间返回 0(自维护几何不被 resize 破坏)
Qt::Edges MainWindow::windowEdgeFlagsAt(const QPoint &globalPos) const
{
    if (!m_uiBuilder || m_uiBuilder->isFakeMaximized())
        return {};
    const QRect frame = frameGeometry();
    constexpr int kResizeBorder = 8; // 逻辑像素命中带,与系统边框拖拽带同量级
    const bool atLeft = globalPos.x() - frame.left() < kResizeBorder;
    const bool atRight = frame.right() - globalPos.x() < kResizeBorder;
    const bool atTop = globalPos.y() - frame.top() < kResizeBorder;
    const bool atBottom = frame.bottom() - globalPos.y() < kResizeBorder;
    if (!atLeft && !atRight && !atTop && !atBottom)
        return {};
    Qt::Edges edges;
    if (atLeft) edges.setFlag(Qt::LeftEdge);
    if (atRight) edges.setFlag(Qt::RightEdge);
    if (atTop) edges.setFlag(Qt::TopEdge);
    if (atBottom) edges.setFlag(Qt::BottomEdge);
    return edges;
}

// 边缘缩放光标轮询(60ms):悬停 MouseMove 不派发到未开 mouseTracking 的控件,
// 纯事件方案在「同控件内部从中心移向边缘」时收不到任何事件,故轮询
// QCursor::pos 维护:带内给命中控件设双向缩放光标,出带恢复(Windows 光标
// 形状在 setCursor 后立即生效,无需等待鼠标移动)
void MainWindow::onEdgeCursorTick()
{
    const Qt::Edges edges = windowEdgeFlagsAt(QCursor::pos());
    if (edges == 0) { // 出带/伪最大化:恢复宿主控件原光标
        if (m_edgeCursorHost) {
            m_edgeCursorHost->unsetCursor();
            m_edgeCursorHost = nullptr;
            m_edgeCursorShape = -1;
            kickCursorRecalc();
        }
        return;
    }
    // 光标下的宿主控件:用纯 Qt 树的 childAt 逐层下探。QApplication::widgetAt
    // 在 Windows 上内部走原生 WindowFromPoint,当浏览器/IDE 等其他进程窗口
    // 盖在本窗口上方时命中跨进程窗口返回 null(实测),不能作为宿主来源。
    // 首层就探不到控件 = 鼠标在本窗口外或被其他窗口盖住:不设光标不抖鼠标,
    // 否则会把缩放光标挂到本窗口并每 tick 全局挪动光标,干扰上层窗口操作
    QWidget *cur = childAt(mapFromGlobal(QCursor::pos()));
    if (cur == nullptr) {
        if (m_edgeCursorHost) {
            m_edgeCursorHost->unsetCursor();
            m_edgeCursorHost = nullptr;
            m_edgeCursorShape = -1;
        }
        return;
    }
    for (QWidget *child = cur->childAt(cur->mapFromGlobal(QCursor::pos())); child;
         child = cur->childAt(cur->mapFromGlobal(QCursor::pos()))) {
        cur = child;
    }
    Qt::CursorShape shape = Qt::SizeHorCursor; // 仅左/右
    if (edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge)) {
        if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge))
            shape = (edges == (Qt::TopEdge | Qt::LeftEdge)
                         || edges == (Qt::BottomEdge | Qt::RightEdge))
                        ? Qt::SizeFDiagCursor  // NWSE:左上/右下角
                        : Qt::SizeBDiagCursor; // NESW:右上/左下角
        else
            shape = Qt::SizeVerCursor; // 仅上/下
    }
    if (m_edgeCursorHost == cur && m_edgeCursorShape == int(shape))
        return; // 宿主与形状均未变,避免每 tick 抖动光标
    if (m_edgeCursorHost && m_edgeCursorHost != cur)
        m_edgeCursorHost->unsetCursor();
    m_edgeCursorHost = cur;
    cur->setCursor(shape);
    m_edgeCursorShape = int(shape);
    kickCursorRecalc();
}

// Windows 的光标形状显示由 WM_SETCURSOR 驱动,光标静止后系统不再发送该消息,
// widget 上 set/unsetCursor 不会立即反映到屏幕;原地抖动 1 逻辑像素触发一对
// WM_MOUSEMOVE/WM_SETCURSOR 让 Qt 按新光标立即重算,视觉上不可感
void MainWindow::kickCursorRecalc()
{
    const QPoint p = QCursor::pos();
    QCursor::setPos(p.x() + 1, p.y());
    QCursor::setPos(p);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // 窗口宽度驱动的参数栏渐进精简（分档阈值见 UiBuilder::updateConnectionBarCompact）；
    // 构造早期 m_uiBuilder 尚未创建，setupUi 触发的同步 resize 直接跳过
    if (m_uiBuilder)
        m_uiBuilder->updateConnectionBarCompact(width());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 配置同步落盘 + 两条链路有序拆除（原 closeEvent 编排迁入）
    m_session->shutdownForClose();
    event->accept();
}

void MainWindow::onDataReceivedForAlert(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_store->silenceAlert() && m_lastDataTimeMs > 0) {
        const qint64 elapsedSec = (now - m_lastDataTimeMs) / 1000;
        if (elapsedSec >= m_store->silenceAlertSeconds()) {
            if (!isActiveWindow() || isMinimized()) {
                QApplication::alert(this, 0); // 持续闪烁直到用户切回窗口
            }
        }
    }
    m_lastDataTimeMs = now;
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        const auto *msg = static_cast<const MSG *>(message);
        if (msg->message == WM_DEVICECHANGE) {
            if (msg->wParam == DBT_DEVICEARRIVAL || msg->wParam == DBT_DEVICEREMOVECOMPLETE) {
                if (m_session)
                    m_session->onDeviceChanged();
            }
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}
