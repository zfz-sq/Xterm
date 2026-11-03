#include "mainwindow.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFontComboBox>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHeaderView>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QtMath>
#include <QVBoxLayout>

#include "agentbridge.h"
#include "commandbar.h"
#include "filebrowserdialog.h"
#include "framelessdialog.h"
#include "hoststats.h"
#include "icons.h"
#include "screenview.h"
#include "serialsession.h"
#include "sshsession.h"
#include "titlebar.h"
#include "transport.h"
#include "ui_mainwindow.h"

// 只为「关于」框取 libssh 运行库版本号（实际加载的 DLL 版本），libssh 头不进主窗口
extern "C" const char *ssh_version(int req_version);

namespace {
// 设备管理器里是「通信端口 (COM1)」「USB-SERIAL CH340 (COM17)」，
// 下拉框改成「COM1: 通信端口」「COM17: USB-SERIAL CH340」。
QString formatPortLabel(const QString &portName, QString description)
{
    description = description.trimmed();
    const QRegularExpression suffix(
        QStringLiteral("\\s*[\\(（]\\s*%1\\s*[\\)）]\\s*$")
            .arg(QRegularExpression::escape(portName)),
        QRegularExpression::CaseInsensitiveOption);
    description.remove(suffix);
    description = description.trimmed();
    if (description.isEmpty() || description.compare(portName, Qt::CaseInsensitive) == 0)
        return portName;
    return QStringLiteral("%1: %2").arg(portName, description);
}

// 可编辑下拉的统一防卡顿设置：默认补全会在弹出时再滤一遍，列表一长就顿。
void tuneEditableCombo(QComboBox *combo, int maxVisible = 20)
{
    combo->setCompleter(nullptr);
    combo->setMaxVisibleItems(maxVisible);
    if (auto *list = qobject_cast<QListView *>(combo->view()))
        list->setUniformItemSizes(true);
}

// 端口下拉只保留这几项。手输的端口留在编辑框里，不写进列表。
const QStringList &fixedSshPorts()
{
    static const QStringList ports = {
        QStringLiteral("22"),
        QStringLiteral("2222"),
        QStringLiteral("22022"),
        QStringLiteral("8022"),
        QStringLiteral("443"),
    };
    return ports;
}

void pruneCustomPortItems(QComboBox *combo)
{
    static bool pruning = false;
    if (pruning)
        return;
    pruning = true;
    const QString typed = combo->currentText();
    for (int i = combo->count() - 1; i >= 0; --i) {
        if (!fixedSshPorts().contains(combo->itemText(i)))
            combo->removeItem(i);
    }
    if (combo->currentText() != typed)
        combo->setEditText(typed);
    pruning = false;
}

// 新增关键词高亮的默认色在几个醒目色间轮换（设置对话框与右键「高亮此词」共用）
QColor nextKeywordColor(int used)
{
    static const QColor palette[] = {
        QColor(0xE0, 0x6C, 0x75), QColor(0xE5, 0xC0, 0x7A), QColor(0x7C, 0xD0, 0x77),
        QColor(0x61, 0xAF, 0xEF), QColor(0xC6, 0x78, 0xDD),
    };
    return palette[used % 5];
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    // 外围留白随窗口状态初始化（窗口化 6px / 最大化 0）
    {
        const int m = isMaximized() ? 0 : 6;
        ui->mainLayout->setContentsMargins(m, m, m, m);
        ui->terminalLayout->setContentsMargins(m, m, m, m);
        // 注意: m_serialView/m_sshView 在 setupTerminal() 创建，pad 随 changeEvent
        // 或 setupTerminal 后统一设置
    }
    setupTitleBar();

    m_serial = new SerialSession(this);
    m_ssh = new SshSession(this);
    m_store = new CommandStore(this);
    // 主机状态采样走共享 SSH 会话的 stats 通道（任务 8）。
    m_hostStats = new HostStats(m_ssh, this);
    connect(m_hostStats, &HostStats::statsUpdated, this,
            [this](bool cpuOk, int cpuPercent, bool memOk, bool memPercentOk, int memPercent,
                   qint64 usedKiB, qint64 totalKiB) {
                if (m_ssh->state() != Transport::Connected)
                    return;
                m_hostCpuOk = cpuOk;
                m_hostCpu = cpuPercent;
                m_hostMemOk = memOk;
                m_hostMemPercentOk = memPercentOk;
                m_hostMemPercent = memPercent;
                m_hostUsedKiB = usedKiB;
                m_hostTotalKiB = totalKiB;
                if (m_mode == Mode::Ssh)
                    updatePersistentStatus();
            });

    // 配置落盘防抖：浏览下拉/连续输入每一步都会触发保存信号，同步写盘
    // （含 Defender 扫描）会把 UI 拍出一顿一顿的卡顿；内存立即更新，
    // 写盘合并到用户停下来之后一次完成。
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { m_store->save(); });

    setupIcons();
    setupConnectionCombos();
    setupSshInputs();
    setupTerminal();
    setupCompletion();

    m_commandBar = new CommandBar(this);
    ui->commandBarLayout->addWidget(m_commandBar);

    // 右侧功能按钮（MCP 等）挂在命令栏收起态的侧边按钮组里：位于“展开命令栏”
    // 按钮下方，命令栏展开时整组隐藏。只显示图标，尺寸统一 28×28。
    m_mcpBtn = new QToolButton(this);
    m_mcpBtn->setIcon(themedIcon(QStringLiteral("mcp.png")));
    m_mcpBtn->setToolTip(tr("显示 MCP 提示词"));
    m_commandBar->addSideButton(m_mcpBtn);

    // 永久状态: 端口/波特率/连接状态
    m_statusLabel = new QLabel(tr("未连接"), this);
    ui->statusbar->addPermanentWidget(m_statusLabel, 1);

    // XOFF 提示：发出 Ctrl+S(0x13)后设备 tty 可能按软件流控挂起输出，
    // 琥珀色常亮提示按 Ctrl+Q(0x11)恢复；发出 Ctrl+Q 或断开连接时熄灭。
    // 放 permanent 区不会被其他临时消息（错误提示等）挤掉。
    m_xoffLabel = new QLabel(tr("XOFF · Ctrl+Q 恢复"), this);
    m_xoffLabel->setStyleSheet(QStringLiteral("color: #D89B2C; font-weight: 600;"));
    m_xoffLabel->setToolTip(tr("按 Ctrl+S（XOFF）后设备输出可能被挂起：画面不更新，"
                               "但按键仍在发送。按 Ctrl+Q（XON）恢复。"));
    m_xoffLabel->hide();
    ui->statusbar->addPermanentWidget(m_xoffLabel, 0);

    // 「内核日志静音」开关：常驻状态栏最右（连接状态旁），仅串口页显示。
    // 设备侧 printk 会异步直写串口控制台，插进 vi/top 画面的字节流里，
    // 终端层无法分离——唯一根治是降低 console_loglevel，这里做成一键开关。
    m_kmsgMuteBtn = new QToolButton(this);
    m_kmsgMuteBtn->setAutoRaise(true);
    connect(m_kmsgMuteBtn, &QToolButton::clicked, this, &MainWindow::toggleKmsgMute);
    ui->statusbar->addPermanentWidget(m_kmsgMuteBtn, 0);
    updateKmsgMuteUi();

    connectSignals();

    m_store->load();
    applyTheme();
    // 恢复上次保存的命令栏收起状态（侧边按钮组形态）
    m_commandBar->setCollapsed(m_store->commandBarCollapsed());
    // 恢复上次保存的终端字号（全局，两页一致）
    if (const int savedPt = m_store->fontSize(); savedPt > 0) {
        m_serialView->setFontSize(savedPt);
        m_sshView->setFontSize(savedPt);
    }
    // 恢复上次保存的终端字体族（空 = 用内置默认，构造函数已设好）
    if (const QString savedFont = m_store->terminalFont(); !savedFont.isEmpty()) {
        m_serialView->setFontFamily(savedFont);
        m_sshView->setFontFamily(savedFont);
    }
    // 恢复关键词高亮规则（全局，两页一致）
    applyKeywords(m_store->keywords());
    ui->advancedBar->hide(); // 高级参数默认收起
    refreshPorts();
    reloadProfiles(m_store->lastProfile());
    setMode(m_store->mode() == QStringLiteral("ssh") ? Mode::Ssh : Mode::Serial, false);

    m_agent = new AgentBridge(this);
    m_agent->start({
        [this] { return agentStatus(); },
        [this](int maxLines) { return agentRecentText(maxLines); },
        [this](const QString &text) { return agentSendLine(text); },
        [this](const QByteArray &bytes) { return agentSendBytes(bytes); },
        // 打字机用的链路行尾与「整行发完」回调（记输入历史）
        [this] { return m_mode == Mode::Serial ? '\n' : '\r'; },
        [this](const QString &line) {
            ScreenView *view = (m_mode == Mode::Serial) ? m_serialView : m_sshView;
            if (view)
                view->rememberSentLine(line);
        },
    });

    // MCP 在线角标: 有合法请求时点亮，30 秒无活动自动熄灭
    // （管道连接按请求建立，没有“断开”事件可依赖）
    m_mcpActiveTimer = new QTimer(this);
    m_mcpActiveTimer->setSingleShot(true);
    m_mcpActiveTimer->setInterval(30 * 1000);
    connect(m_mcpActiveTimer, &QTimer::timeout, this, [this] { setMcpActive(false); });
    connect(m_agent, &AgentBridge::agentActive, this, [this] {
        setMcpActive(true);
        m_mcpActiveTimer->start();
    });
    // AI 打字机：状态栏实时显示正在输入的命令，打完/被打断时清除
    connect(m_agent, &AgentBridge::agentTypingChanged, this,
            [this](bool active, const QString &text) {
        if (active) {
            setMcpActive(true);
            m_mcpActiveTimer->stop(); // 打字期间角标常亮，不因 30 秒超时熄灭
            const QString shown = text.size() > 40
                                      ? text.left(40) + QStringLiteral("…") : text;
            ui->statusbar->showMessage(
                tr("AI 正在输入: %1（你的任何按键都会打断它）").arg(shown));
        } else {
            ui->statusbar->showMessage(QString());
            m_mcpActiveTimer->start(); // 恢复 30 秒无活动自动熄灭
        }
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}

// ---------------------------------------------------------------- 初始化

void MainWindow::setupTitleBar()
{
    // 无边框窗口 + 自定义标题栏：暗色主题下标题栏不再是系统白条，
    // 且标题栏升级为 VSCode 式工具条（菜单 / 主题切换 / 置顶 / 系统按钮）。
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);

    m_titleBar = new TitleBar(this);
    setMenuWidget(m_titleBar); // QMainWindow 顶部专属位，不占 centralWidget

    // VSCode 式菜单栏：设置 / 工具 / 帮助（复用已有按钮/快捷键行为，避免另起一套逻辑）
    auto *setMenu = new QMenu(m_titleBar);
    setMenu->addAction(tr("切换主题"), this, [this] {
        m_store->setDarkTheme(!m_store->isDarkTheme());
        m_store->save();
        applyTheme();
    });
    setMenu->addAction(tr("关键词高亮"), this, [this] { openKeywordSettings(); });
    setMenu->addAction(tr("字体"), this, [this] { openFontSettings(); });
    setMenu->addSeparator();
    auto *quitAction = setMenu->addAction(tr("退出"), this, &MainWindow::close);
    quitAction->setShortcut(QKeySequence::Quit);
    m_titleBar->addMenuButton(tr("设置"))->setMenu(setMenu);

    auto *toolMenu = new QMenu(m_titleBar);
    // F5/F9/F10 都注册在菜单 action 上（快捷键唯一，见下方 connectSignals 的说明）。
    // 文案跟随连接状态（updateConnectionUi 同步），与连接按钮同一动作
    m_connectAction = toolMenu->addAction(tr("连接"), this, [this] {
        ui->connectBtn->click();
    });
    m_connectAction->setShortcut(QKeySequence(Qt::Key_F5));
    auto *filesAction = toolMenu->addAction(tr("SFTP"), this, [this] {
        if (m_ssh->state() == Transport::Connected) {
            onFilesClicked();
        } else {
            appendActiveNote(tr("── SSH 未连接，无法打开 SFTP 文件管理 ──"));
        }
    });
    filesAction->setShortcut(QKeySequence(Qt::Key_F10));
    // 文案跟随当前模式（updateModeUi 同步），点谁就切到谁
    m_switchModeAction = toolMenu->addAction(tr("切换到 SSH"), this, [this] {
        setMode(m_mode == Mode::Serial ? Mode::Ssh : Mode::Serial);
    });
    m_switchModeAction->setShortcut(QKeySequence(Qt::Key_F9));
    m_titleBar->addMenuButton(tr("工具"))->setMenu(toolMenu);

    auto *helpMenu = new QMenu(m_titleBar);
    helpMenu->addAction(tr("文档"), this, [] {
        // 优先打开项目自带 docs 目录（打包环境无 docs 时静默失败即可）
        const QString docs = QCoreApplication::applicationDirPath()
                             + QStringLiteral("/docs");
        if (QDir(docs).exists())
            QDesktopServices::openUrl(QUrl::fromLocalFile(docs));
    });
    helpMenu->addAction(tr("关于 Qt"), this, [this] {
        // QApplication::aboutQt 弹的是原生 QMessageBox，换成公共对话框保持主题一致
        FramelessDialog dlg(this);
        dlg.setDialogTitle(tr("关于 Qt"));
        auto *content = new QWidget;
        auto *lay = new QVBoxLayout(content);
        lay->setContentsMargins(24, 18, 24, 16);
        lay->setSpacing(8);
        auto *logo = new QLabel(
            tr("<span style=\"font-size:26pt; font-weight:600; color:#41CD52\">Qt</span>"));
        auto *text = new QLabel(
            tr("<b>关于 Qt</b><br><br>"
               "本程序使用了 Qt %1。<br><br>"
               "Qt 是面向 C++ 的跨平台应用开发框架，"
               "提供图形界面、网络、多媒体等模块，"
               "Qt 是 The Qt Company Ltd. 的注册商标。")
                .arg(QString::fromLatin1(qVersion())));
        text->setWordWrap(true);
        auto *row = new QHBoxLayout;
        row->setSpacing(16);
        row->addWidget(logo, 0, Qt::AlignTop);
        row->addWidget(text, 0, Qt::AlignTop);
        lay->addLayout(row);
        lay->addSpacing(8);
        auto *btnRow = new QHBoxLayout;
        btnRow->addStretch(1);
        auto *ok = new QPushButton(tr("确定"));
        ok->setDefault(true); // 回车确认
        ok->setFixedWidth(96);
        connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
        btnRow->addWidget(ok);
        lay->addLayout(btnRow);
        dlg.setContentLayout(lay);
        dlg.applyTheme(m_store->isDarkTheme());
        dlg.exec();
    });
    helpMenu->addAction(tr("关于 Xterm"), this, [this] {
        // 公共无边框对话框，主题随全局 QSS，暗色下不再露原生样式。
        // 版式: 居中品牌区 → 简介 → 三张信息卡片 → 分隔线 → 页脚(链接/版权/按钮)
        FramelessDialog dlg(this);
        dlg.setDialogTitle(tr("关于 Xterm"));
        dlg.setMinimumWidth(500);
        const bool dark = m_store->isDarkTheme();
        const QString dim = dark ? QStringLiteral("#9A9A9A") : QStringLiteral("#6E6E6E");
        const QString lineColor = dark ? QStringLiteral("#3B3B40") : QStringLiteral("#C9C9CD");
        auto hline = [&lineColor]() {
            auto *line = new QWidget;
            line->setFixedHeight(1);
            line->setStyleSheet(QStringLiteral("background-color:%1").arg(lineColor));
            return line;
        };

        auto *content = new QWidget;
        auto *lay = new QVBoxLayout(content);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        // ---- 品牌区: 图标 / 名称 / 副标题，整体居中 ----
        auto *brand = new QWidget;
        auto *brandLay = new QVBoxLayout(brand);
        brandLay->setContentsMargins(28, 24, 28, 0);
        brandLay->setSpacing(4);
        auto *icon = new QLabel;
        icon->setPixmap(QPixmap(QStringLiteral(":/icons/app.png"))
                            .scaled(64, 64, Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation));
        icon->setAlignment(Qt::AlignHCenter);
        auto *name = new QLabel(
            tr("<span style=\"font-size:18pt; font-weight:600;\">Xterm</span>"));
        name->setAlignment(Qt::AlignHCenter);
        auto *tagline = new QLabel(
            tr("<span style=\"font-size:9.5pt; color:%1;\">串口 / SSH 双链路终端 · v%2</span>")
                .arg(dim, QCoreApplication::applicationVersion()));
        tagline->setAlignment(Qt::AlignHCenter);
        brandLay->addWidget(icon);
        brandLay->addWidget(name);
        brandLay->addWidget(tagline);
        lay->addWidget(brand);

        // ---- 正文: 简介 + 信息卡片 ----
        auto *body = new QWidget;
        auto *bodyLay = new QVBoxLayout(body);
        bodyLay->setContentsMargins(28, 18, 28, 20);
        bodyLay->setSpacing(14);
        auto *desc = new QLabel(
            tr("面向嵌入式调试：串口与 SSH 两条链路独立存活、一键切换互不打断；"
               "SFTP 双栏文件管理共享同一条 SSH 连接；终端支持 vim / htop 等"
               "全屏程序、关键词高亮与滚动历史查找；内置 MCP 桥，"
               "可让 AI 直接操作已连接的终端。"));
        desc->setWordWrap(true);
        bodyLay->addWidget(desc);
        lay->addWidget(body);

        lay->addWidget(hline());

        // ---- 页脚: 第三方组件 / 版权 / 按钮 ----
        auto *foot = new QWidget;
        auto *footLay = new QVBoxLayout(foot);
        footLay->setContentsMargins(28, 14, 28, 16);
        footLay->setSpacing(10);

        // 组件名挂上游源码主页，点击走默认浏览器；QSS 管不到富文本 <a>，
        // 链接色用调色板 Link 角色按主题上色
        auto *legal = new QLabel(
            tr("<span style=\"color:%1;\">第三方组件：</span>"
               "<a href=\"https://www.qt.io/\">Qt</a>"
               "<span style=\"color:%1;\">（GPL/LGPL）· </span>"
               "<a href=\"https://www.libssh.org/\">libssh</a>"
               "<span style=\"color:%1;\">（LGPL-2.1）· </span>"
               "<a href=\"https://www.openssl.org/\">OpenSSL</a>"
               "<span style=\"color:%1;\">（Apache-2.0）· </span>"
               "<a href=\"https://www.mingw-w64.org/\">mingw-w64</a>")
                .arg(dim));
        legal->setOpenExternalLinks(true);
        legal->setWordWrap(true);
        QPalette linkPal = legal->palette();
        const QColor linkColor = dark ? QColor(0x4F, 0xC1, 0xFF) : QColor(0x09, 0x69, 0xDA);
        linkPal.setColor(QPalette::Link, linkColor);
        linkPal.setColor(QPalette::LinkVisited, linkColor);
        legal->setPalette(linkPal);
        footLay->addWidget(legal);

        auto *copy = new QLabel(
            tr("<span style=\"font-size:8.5pt; color:%1;\">版权所有 © 2026</span>").arg(dim));
        copy->setAlignment(Qt::AlignHCenter);
        footLay->addWidget(copy);

        auto *row = new QHBoxLayout;
        row->addStretch(1);
        auto *ok = new QPushButton(tr("确定"));
        ok->setDefault(true); // 回车确认
        ok->setFixedWidth(96);
        connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
        row->addWidget(ok);
        footLay->addLayout(row);
        lay->addWidget(foot);

        dlg.setContentLayout(lay);
        dlg.applyTheme(dark);
        dlg.exec();
    });
    m_titleBar->addMenuButton(tr("帮助"))->setMenu(helpMenu);

    // 标题不显示在标题栏（VSCode 式），系统任务栏/Alt+Tab 仍显示 windowTitle

    // 主题 / 置顶 / 系统按钮
    connect(m_titleBar, &TitleBar::themeClicked, this, [this] {
        m_store->setDarkTheme(!m_store->isDarkTheme());
        m_store->save();
        applyTheme();
    });
    connect(m_titleBar, &TitleBar::minimizedRequested, this,
            [this] { setWindowState(windowState() | Qt::WindowMinimized); });
    connect(m_titleBar, &TitleBar::maximizedToggled, this, [this] {
        setWindowState(isMaximized() ? windowState() & ~Qt::WindowMaximized
                                     : windowState() | Qt::WindowMaximized);
    });
    connect(m_titleBar, &TitleBar::closedRequested, this, &MainWindow::close);

    applyWin11RoundedCorners(this);
}

void MainWindow::setupIcons()
{
    ui->refreshPortBtn->setIcon(themedIcon(QStringLiteral("refresh.png")));
    ui->settingsBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    ui->settingsBtn->setIconSize(QSize(16, 16));
    ui->sshSettingsBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    ui->sshSettingsBtn->setIconSize(QSize(16, 16));
    const bool passwordHidden = ui->passwordEdit->echoMode() == QLineEdit::Password;
    ui->pwToggleBtn->setIcon(themedIcon(passwordHidden ? QStringLiteral("eye.png")
                                                       : QStringLiteral("eye_off.png")));
    ui->pwToggleBtn->setIconSize(QSize(16, 16));
    ui->pwToggleBtn->setText(QString());
    ui->keyBrowseBtn->setIconSize(QSize(16, 16));
    ui->filesBtn->setIcon(themedIcon(QStringLiteral("folder.png")));
}

void MainWindow::setupConnectionCombos()
{
    m_loading = true;

    ui->baudCombo->setEditable(true);
    tuneEditableCombo(ui->baudCombo);
    ui->baudCombo->setValidator(new QIntValidator(1, 4000000, this));
    ui->baudCombo->addItems({
        QStringLiteral("1200"), QStringLiteral("2400"), QStringLiteral("4800"),
        QStringLiteral("9600"), QStringLiteral("19200"), QStringLiteral("38400"),
        QStringLiteral("57600"), QStringLiteral("115200"), QStringLiteral("230400"),
        QStringLiteral("460800"), QStringLiteral("921600"), QStringLiteral("1000000"),
        QStringLiteral("1500000"), QStringLiteral("2000000")});
    ui->baudCombo->setCurrentText(QStringLiteral("115200")); // 默认 115200 8N1

    for (int bits : {5, 6, 7, 8})
        ui->dataBitsCombo->addItem(QString::number(bits), bits);
    ui->dataBitsCombo->setCurrentText(QStringLiteral("8"));

    ui->parityCombo->addItem(tr("无"), QStringLiteral("none"));
    ui->parityCombo->addItem(tr("偶校验"), QStringLiteral("even"));
    ui->parityCombo->addItem(tr("奇校验"), QStringLiteral("odd"));

    ui->stopBitsCombo->addItem(QStringLiteral("1"), 1.0);
    ui->stopBitsCombo->addItem(QStringLiteral("1.5"), 1.5);
    ui->stopBitsCombo->addItem(QStringLiteral("2"), 2.0);

    ui->flowCombo->addItem(tr("无"), QStringLiteral("none"));
    ui->flowCombo->addItem(tr("硬件 RTS/CTS"), QStringLiteral("hardware"));
    ui->dtrCheck->setChecked(true); // 默认与 Windows 打开串口后的电平一致，不改变现有行为
    ui->rtsCheck->setChecked(true);

    m_loading = false;
}

void MainWindow::setupSshInputs()
{
    m_loading = true;

    ui->sshPortCombo->setEditable(true);
    ui->sshPortCombo->setInsertPolicy(QComboBox::NoInsert);
    tuneEditableCombo(ui->sshPortCombo, 8);
    ui->sshPortCombo->setValidator(new QIntValidator(1, 65535, this));
    ui->sshPortCombo->addItems(fixedSshPorts());
    ui->sshPortCombo->setCurrentText(QStringLiteral("22"));
    // 样式表给可编辑下拉算的编辑区比箭头靠前一截，5 位数字会被裁掉，中间还空着。
    // 宽度按「左内边距 + 5 位数字 + 箭头」定死，再把编辑框拉到箭头边上。
    const QFontMetrics portFm(ui->sshPortCombo->font());
    ui->sshPortCombo->setFixedWidth(8 + portFm.horizontalAdvance(QStringLiteral("65535")) + 22);
    ui->sshPortCombo->installEventFilter(this);
    fitSshPortEdit();
    ui->sshPortCombo->setToolTip(tr("常用端口直接选。手输的端口只留在框里，不会加进下拉列表"));
    // 即便插入策略是 NoInsert，也再扫一遍，把任何多出来的项丢掉，编辑框文字保留。
    connect(ui->sshPortCombo->model(), &QAbstractItemModel::rowsInserted, this, [this] {
        QTimer::singleShot(0, this, [this] { pruneCustomPortItems(ui->sshPortCombo); });
    });

    for (int sec : {5, 10, 15, 30, 60})
        ui->timeoutCombo->addItem(QString::number(sec));
    ui->timeoutCombo->setCurrentText(QStringLiteral("10"));

    m_loading = false;
}

void MainWindow::fitSshPortEdit()
{
    QComboBox *combo = ui->sshPortCombo;
    QLineEdit *edit = combo->lineEdit();
    if (!edit || combo->width() < 8)
        return;
    constexpr int arrow = 16;
    constexpr int frame = 1;
    const QRect next(frame, frame, combo->width() - arrow - frame * 2, combo->height() - frame * 2);
    if (next.width() > 0 && next.height() > 0 && edit->geometry() != next)
        edit->setGeometry(next);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->sshPortCombo
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::StyleChange)) {
        // 下拉框自己的 resize 会把编辑框缩回去，等它排完再拉满到箭头前。
        QTimer::singleShot(0, this, [this] { fitSshPortEdit(); });
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setupTerminal()
{
    // 两个终端各占一页：各自的缓冲区、ANSI 状态、滚动位置都留在原地，
    // 切模式只是换一页显示，另一侧继续收发，内容不丢、连接不断。
    // 两页都是格子屏幕模型：串口设备的 top/vim 和 SSH 一样能还原。
    m_serialView = new ScreenView(this);
    // 默认终端档（对齐 SSH）；开关状态与配置在 applyCurrentProfile 里同步
    m_serialView->setLocalEcho(false);
    ui->serialTerminalPageLayout->addWidget(m_serialView);

    m_sshView = new ScreenView(this);
    ui->sshTerminalPageLayout->addWidget(m_sshView);

    // 最大化启动时终端网格内边距同步为 0（窗口化默认 6px）
    if (isMaximized()) {
        m_serialView->setPad(0);
        m_sshView->setPad(0);
    }

    // 面板外观由全局主题 QSS（QFrame#terminalPanel）定义
}

void MainWindow::setupCompletion()
{
    m_completion = new CompletionEngine; // 非 QObject，不接受 parent；生命周期由 m_completion 指针托管
    // 调试档才挂补全；终端档没有本地行编辑，Tab 交给设备（onSerialModeToggled 同步）
    m_serialView->setCompletionEngine(ui->serialModeBtn->isChecked() ? nullptr : m_completion);
    // 把当前配置里的命令名注入补全源
    const Profile p = m_store->profile(m_currentProfile);
    QStringList names;
    for (const CommandItem &c : p.commands)
        names << c.text;
    m_completion->setUserCommands(names);
}

void MainWindow::connectSignals()
{
    connect(ui->refreshPortBtn, &QToolButton::clicked, this, &MainWindow::refreshPorts);
    connect(ui->connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);

    connect(ui->portCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->baudCombo, &QComboBox::currentTextChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->serialModeBtn, &QToolButton::toggled, this,
            &MainWindow::onSerialModeToggled);
    connect(ui->dataBitsCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->parityCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->stopBitsCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->flowCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->dtrCheck, &QCheckBox::toggled, this, &MainWindow::onConnectionParamChanged);
    connect(ui->rtsCheck, &QCheckBox::toggled, this, &MainWindow::onConnectionParamChanged);

    connect(ui->hostEdit, &QLineEdit::textChanged, this, &MainWindow::onConnectionParamChanged);
    connect(ui->hostEdit, &QLineEdit::returnPressed, this, &MainWindow::onConnectClicked);
    connect(ui->sshPortCombo, &QComboBox::currentTextChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->userEdit, &QLineEdit::textChanged, this, &MainWindow::onConnectionParamChanged);
    connect(ui->userEdit, &QLineEdit::returnPressed, this, &MainWindow::onConnectClicked);
    connect(ui->timeoutCombo, &QComboBox::currentTextChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->passwordEdit, &QLineEdit::textChanged, this, [this] {
        if (!m_loading)
            syncCurrentProfile();
    });
    // 密码输完直接回车 = 连接，符合最常见操作习惯
    connect(ui->passwordEdit, &QLineEdit::returnPressed, this, &MainWindow::onConnectClicked);
    connect(ui->pwToggleBtn, &QToolButton::clicked, this, [this] {
        const bool hidden = ui->passwordEdit->echoMode() == QLineEdit::Password;
        ui->passwordEdit->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
        ui->pwToggleBtn->setIcon(themedIcon(hidden ? QStringLiteral("eye_off.png")
                                                   : QStringLiteral("eye.png")));
        ui->pwToggleBtn->setToolTip(hidden ? tr("隐藏密码") : tr("显示密码"));
    });
    connect(ui->keyEdit, &QLineEdit::textChanged, this, [this] {
        if (!m_loading)
            syncCurrentProfile();
    });

    // 终端键盘直通 / 预设按钮
    // 用户亲自输入（键盘/粘贴）优先于 AI：先打断 AI 打字机，再发送自己的字节
    connect(m_serialView, &ScreenView::sendData, this, [this](const QByteArray &bytes) {
        if (m_agent && m_agent->interruptTyping())
            ui->statusbar->showMessage(tr("已打断 AI 输入"), 3000);
        sendRaw(bytes);
    });
    connect(m_sshView, &ScreenView::sendData, this, [this](const QByteArray &bytes) {
        if (m_agent && m_agent->interruptTyping())
            ui->statusbar->showMessage(tr("已打断 AI 输入"), 3000);
        sendRaw(bytes);
    });
    connect(m_sshView, &ScreenView::terminalSizeChanged, this,
            [this](int cols, int rows) { m_ssh->setPtySize(cols, rows); });

    // 字号全局联动：任一页改字号两页同步并持久化。
    // setFontSize 字号相同则不发信号，联动不会回环。
    connect(m_serialView, &ScreenView::fontSizeChanged, this, [this](int pt) {
        m_sshView->setFontSize(pt);
        m_store->setFontSize(pt);
        m_store->save();
    });
    connect(m_sshView, &ScreenView::fontSizeChanged, this, [this](int pt) {
        m_serialView->setFontSize(pt);
        m_store->setFontSize(pt);
        m_store->save();
    });
    // 字体族同样两页联动并落盘（setFontFamily 族相同则不发信号，不会回环）。
    // 选回默认族时存空串：配置文件里保留“未设置”语义，以后系统等宽字体变了也跟着走
    connect(m_serialView, &ScreenView::fontFamilyChanged, this, [this](const QString &family) {
        m_sshView->setFontFamily(family);
        m_store->setTerminalFont(family == ScreenView::defaultFontFamily() ? QString() : family);
        m_store->save();
    });
    connect(m_sshView, &ScreenView::fontFamilyChanged, this, [this](const QString &family) {
        m_serialView->setFontFamily(family);
        m_store->setTerminalFont(family == ScreenView::defaultFontFamily() ? QString() : family);
        m_store->save();
    });

    // 模式切换 / 高级参数收展（主题切换已移到自定义标题栏）
    connect(m_mcpBtn, &QToolButton::clicked, this, &MainWindow::onMcpClicked);
    connect(ui->modeBtn, &QToolButton::clicked, this, [this] {
        setMode(m_mode == Mode::Serial ? Mode::Ssh : Mode::Serial);
    });

    // 命令栏收起状态持久化: 下次启动直接恢复侧边按钮组形态
    connect(m_commandBar, &CommandBar::collapsedChanged, this, [this](bool collapsed) {
        m_store->setCommandBarCollapsed(collapsed);
        m_store->save();
    });

    // 快捷键说明: F5 连接/断开、F9 切串口/SSH、F10 SFTP，
    // 全部注册在工具菜单的 action 上（快捷键保持唯一注册，重复注册会被
    // Qt 判为歧义、两个都不触发——F9 曾因此失效）。
    // 注意: 焦点在终端时 F5/F9/F10 由快捷键优先消费，不再透传给远端
    // （F1-F8、F11、F12 的 ANSI 序列不受影响）。

    // 选中词右键「高亮此词」：两个终端页共用同一套规则
    for (ScreenView *view : { m_serialView, m_sshView })
        connect(view, &ScreenView::highlightRequested, this, &MainWindow::onHighlightRequested);
    // settingsBtn 非 checkable，clicked(false) 会让 setVisible(false) 永远收起，改用 lambda 切换
    connect(ui->settingsBtn, &QToolButton::clicked, this, [this] {
        ui->advancedStack->setCurrentIndex(0);
        ui->advancedBar->setVisible(!ui->advancedBar->isVisible()
                                    || ui->advancedStack->currentIndex() != 0);
    });
    connect(ui->sshSettingsBtn, &QToolButton::clicked, this, [this] {
        ui->advancedStack->setCurrentIndex(1);
        ui->advancedBar->setVisible(!ui->advancedBar->isVisible()
                                    || ui->advancedStack->currentIndex() != 1);
    });
    connect(ui->keyBrowseBtn, &QToolButton::clicked, this, &MainWindow::browsePrivateKey);

    connect(m_commandBar, &CommandBar::commandRequested, this, &MainWindow::sendLine);
    connect(m_commandBar, &CommandBar::commandsChanged, this, [this] {
        if (m_loading)
            return;
        // 同步给补全引擎（取每条命令的第一个 token 作为命令名）
        QStringList names;
        for (const CommandItem &c : m_commandBar->commands())
            names << c.text;
        m_completion->setUserCommands(names);
        syncCurrentProfile();
    });

    // ---- 串口链路 ----
    connect(m_serial, &SerialSession::opened, this, [this] {
        m_serialView->appendSystemMessage(tr("── 已连接 %1 ──").arg(connectionSummary()));
        m_kmsgMuted = false; // printk 回到设备默认级别，静音状态重新计
        updateKmsgMuteUi();
        updateConnectionUi();
        updatePersistentStatus();
        if (m_mode == Mode::Serial)
            m_serialView->setFocus();
    });
    connect(m_serial, &SerialSession::closed, this, [this] {
        m_serialView->appendSystemMessage(tr("── 连接已断开 ──"));
        m_kmsgMuted = false; // 断开后设备侧状态未知，回到「正常」
        updateKmsgMuteUi();
        m_xoffLabel->hide(); // 设备侧流控状态已不可知，提示熄灭
        updateConnectionUi();
        updatePersistentStatus();
    });
    connect(m_serial, &SerialSession::errorOccurred, this, [this](const QString &msg) {
        ui->statusbar->showMessage(msg, 8000);
        m_serialView->appendSystemMessage(tr("── %1 ──").arg(msg));
    });
    connect(m_serial, &SerialSession::received, m_serialView, &ScreenView::receiveData);
    // Ctrl+S(XOFF)可能挂起设备输出：状态栏亮提示，Ctrl+Q(XON)熄灭
    for (ScreenView *view : { m_serialView, m_sshView })
        connect(view, &ScreenView::xoffChanged, this,
                [this](bool paused) { m_xoffLabel->setVisible(paused); });
    connect(m_serial, &SerialSession::stateChanged, this,
            [this](Transport::State) { updateConnectionUi(); updatePersistentStatus(); });

    // ---- SSH 链路 ----
    connect(m_ssh, &SshSession::opened, this, [this] {
        m_sshView->appendSystemMessage(tr("── 已连接 %1 ──").arg(m_ssh->summary()));
        clearHostStats();
        m_hostStats->resetDelta();
        m_ssh->startStats();
        updateConnectionUi();
        updatePersistentStatus();
        if (m_mode == Mode::Ssh)
            m_sshView->setFocus();
    });
    connect(m_ssh, &SshSession::closed, this, [this] {
        m_ssh->stopStats();
        clearHostStats();
        m_xoffLabel->hide(); // 设备侧流控状态已不可知，提示熄灭
        updateConnectionUi();
        updatePersistentStatus();
    });
    connect(m_ssh, &SshSession::sessionEnded, this,
            [this](int exitCode, bool userRequested) {
                if (userRequested)
                    m_sshView->appendSystemMessage(tr("── SSH 连接已断开 ──"));
                else
                    m_sshView->appendSystemMessage(
                        tr("── SSH 会话结束（退出码 %1），重新连接即可 ──").arg(exitCode));
            });
    connect(m_ssh, &SshSession::errorOccurred, this, [this](const QString &msg) {
        ui->statusbar->showMessage(msg, 8000);
        m_sshView->appendSystemMessage(tr("── %1 ──").arg(msg));
    });
    connect(m_ssh, &SshSession::received, m_sshView, &ScreenView::receiveData);
    connect(m_ssh, &SshSession::hostKeyDecisionRequired, this,
            &MainWindow::promptHostKeyDecision);
    connect(m_ssh, &SshSession::unknownHostKeyRecorded, this,
            [this](const QString &host) {
                m_sshView->appendSystemMessage(
                    tr("── 已记录主机 %1 的新密钥。首次信任无法防止中间人攻击 ──")
                        .arg(host));
            });
    connect(m_ssh, &SshSession::authAnswersRequired, this,
            &MainWindow::promptAuthAnswers);
    connect(m_sshView, &ScreenView::shellTitleChanged, this, [this](const QString &title) {
        m_shellTitle = title;
        if (m_mode == Mode::Ssh)
            updateModeUi();
    });
    connect(m_ssh, &SshSession::stateChanged, this,
            [this](Transport::State) { updateConnectionUi(); updatePersistentStatus(); });

    connect(ui->filesBtn, &QToolButton::clicked, this, &MainWindow::onFilesClicked);
}

// ---------------------------------------------------------------- 连接栏

void MainWindow::refreshPorts()
{
    const QString keep = selectedPortName();
    m_loading = true;
    ui->portCombo->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        const QString port = info.portName();
        ui->portCombo->addItem(formatPortLabel(port, info.description()), port);
        const QString tip = QStringLiteral("%1 %2 %3")
                                .arg(info.description(), info.manufacturer(),
                                     info.serialNumber())
                                .simplified();
        ui->portCombo->setItemData(ui->portCombo->count() - 1, tip, Qt::ToolTipRole);
    }
    const int keepIndex = keep.isEmpty() ? -1 : ui->portCombo->findData(keep);
    if (keepIndex >= 0)
        ui->portCombo->setCurrentIndex(keepIndex);
    else if (ui->portCombo->count() > 0)
        ui->portCombo->setCurrentIndex(0);
    // 收起时固定 150；下拉列表按最长一项撑开，避免长名称被裁掉后无法辨认
    const QFontMetrics fm(ui->portCombo->font());
    int popupWidth = ui->portCombo->width();
    for (int i = 0; i < ui->portCombo->count(); ++i)
        popupWidth = qMax(popupWidth, fm.horizontalAdvance(ui->portCombo->itemText(i)) + 28);
    ui->portCombo->view()->setMinimumWidth(popupWidth);
    m_loading = false;

    if (ui->portCombo->count() == 0)
        ui->statusbar->showMessage(tr("未检测到可用串口设备"), 5000);
}

QString MainWindow::selectedPortName() const
{
    const QString port = ui->portCombo->currentData().toString();
    return port.isEmpty() ? ui->portCombo->currentText() : port;
}

void MainWindow::onConnectClicked()
{
    if (m_mode == Mode::Serial) {
        if (m_serial->isOpen() || m_serial->state() == Transport::Connecting) {
            m_serial->close();
            return;
        }
        const QString port = selectedPortName();
        if (port.isEmpty()) {
            ui->statusbar->showMessage(tr("请先选择串口端口"), 5000);
            return;
        }
        const qint32 baud = ui->baudCombo->currentText().toInt();
        const auto dataBits = static_cast<QSerialPort::DataBits>(
            ui->dataBitsCombo->currentData().toInt());
        const QString parityStr = ui->parityCombo->currentData().toString();
        const QSerialPort::Parity parity =
            (parityStr == QStringLiteral("even")) ? QSerialPort::EvenParity
            : (parityStr == QStringLiteral("odd"))  ? QSerialPort::OddParity
                                                    : QSerialPort::NoParity;
        const double sb = ui->stopBitsCombo->currentData().toDouble();
        const QSerialPort::StopBits stopBits = (sb > 1.9)   ? QSerialPort::TwoStop
                                               : (sb > 1.4) ? QSerialPort::OneAndHalfStop
                                                            : QSerialPort::OneStop;
        const QSerialPort::FlowControl flow =
            (ui->flowCombo->currentData().toString() == QStringLiteral("hardware"))
                ? QSerialPort::HardwareControl
                : QSerialPort::NoFlowControl;

        // 失败原因由 errorOccurred 统一上报（状态栏 + 终端），这里不再重复提示
        m_serial->open(port, baud, dataBits, parity, stopBits, flow,
                       ui->dtrCheck->isChecked(), ui->rtsCheck->isChecked());
        updateConnectionUi();
        return;
    }

    // SSH
    if (m_ssh->isOpen() || m_ssh->state() == Transport::Connecting
        || m_ssh->state() == Transport::Disconnecting) {
        m_ssh->close();
        updateConnectionUi();
        return;
    }
    if (const QString err = SshSession::availabilityError(); !err.isEmpty()) {
        ui->statusbar->showMessage(err, 8000);
        m_sshView->appendSystemMessage(tr("── %1 ──").arg(err));
        return;
    }
    const SshTarget target = sshTargetFromUi();
    if (m_ssh->open(target).isEmpty())
        m_sshView->appendSystemMessage(tr("── 正在连接 %1 ──").arg(m_ssh->summary()));
    updateConnectionUi();
}

SshTarget MainWindow::sshTargetFromUi() const
{
    SshTarget t;
    t.host = ui->hostEdit->text().trimmed();
    t.port = ui->sshPortCombo->currentText().toInt();
    if (t.port <= 0)
        t.port = 22;
    t.user = ui->userEdit->text().trimmed();
    t.password = ui->passwordEdit->text();
    t.privateKey = ui->keyEdit->text().trimmed();
    t.connectTimeoutSec = ui->timeoutCombo->currentText().toInt();
    if (t.connectTimeoutSec <= 0)
        t.connectTimeoutSec = 10;
    return t;
}

void MainWindow::onConnectionParamChanged()
{
    if (m_loading)
        return;
    syncCurrentProfile();
}

// 串口终端模式切换。调试：为 STM32 CDC 这类无回显设备做的本地代画
// 代编辑（含远端画屏让路）；终端：对齐 SSH——设备回显，按键逐个裸发，
// vi 的单键提示、全屏重画都按 SSH 的路数走。只改本地行为，不动链路参数，
// 连接中也能切，所以不参与 updateConnectionUi 的锁定。
void MainWindow::onSerialModeToggled(bool pure)
{
    ui->serialModeBtn->setText(pure ? tr("终端") : tr("调试"));
    m_serialView->setLocalEcho(!pure);
    // Tab 补全是行编辑的周边（读屏上输入行、发差量），纯终端没有本地
    // 行编辑，交给设备自己处理 Tab。
    m_serialView->setCompletionEngine(pure ? nullptr : m_completion);
    if (!m_loading)
        syncCurrentProfile();
}

void MainWindow::browsePrivateKey()
{
    const QString dir = QDir::homePath() + QStringLiteral("/.ssh");
    const QString start = QDir(dir).exists() ? dir : QDir::homePath();
    // 只取路径，不自动聚焦任何输入框：键盘/输入法保持原样
    const QString file = QFileDialog::getOpenFileName(
        this, tr("选择 SSH 私钥"), start,
        QStringLiteral("所有文件 (*);;私钥 (*.pem *.key id_* id_ed25519 id_rsa)"));
    if (!file.isEmpty())
        ui->keyEdit->setText(QDir::toNativeSeparators(file));
}

void MainWindow::onFilesClicked()
{
    if (m_ssh->state() != Transport::Connected)
        return; // 按钮只在已连接时可用，这里再挡一层
    if (m_fileBrowser) {
        m_fileBrowser->show();
        m_fileBrowser->raise();
        m_fileBrowser->activateWindow();
        return;
    }
    // 直接复用终端的 SshSession（同一条 SSH 连接上的共享 SFTP 通道）；
    // 关闭窗口只取消文件请求，终端会话不受影响。
    m_fileBrowser = new FileBrowserDialog(m_ssh, this);
    m_fileBrowser->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_fileBrowser, &QObject::destroyed, this, [this] { m_fileBrowser = nullptr; });
    m_fileBrowser->show();
}

// ---------------------------------------------------------------- 模式切换

Transport *MainWindow::activeTransport() const
{
    return m_mode == Mode::Serial ? static_cast<Transport *>(m_serial)
                                  : static_cast<Transport *>(m_ssh);
}

void MainWindow::focusActiveTerminal()
{
    if (m_mode == Mode::Serial)
        m_serialView->setFocus();
    else
        m_sshView->setFocus();
}

void MainWindow::appendActiveNote(const QString &text)
{
    if (m_mode == Mode::Serial)
        m_serialView->appendSystemMessage(text);
    else
        m_sshView->appendSystemMessage(text);
}

void MainWindow::setMode(Mode mode, bool persist)
{
    const bool changed = (mode != m_mode);
    m_mode = mode;

    // 只切显示：两条链路、两份终端内容都原样留在后台
    ui->modeStack->setCurrentIndex(mode == Mode::Serial ? 0 : 1);
    ui->advancedStack->setCurrentIndex(mode == Mode::Serial ? 0 : 1);
    ui->terminalStack->setCurrentIndex(mode == Mode::Serial ? 0 : 1);

    if (changed && persist) {
        m_store->setMode(mode == Mode::Serial ? QStringLiteral("serial")
                                              : QStringLiteral("ssh"));
        m_store->save();
    }
    if (changed && ui->advancedBar->isVisible()) {
        // 高级栏开着时切模式，跟随显示当前模式的参数，不改变收起/展开状态
        ui->advancedBar->setVisible(true);
    } else if (changed) {
        ui->advancedBar->hide();
    }

    updateModeUi();
    updateConnectionUi();
    updatePersistentStatus();
    if (changed)
        focusActiveTerminal();
}

void MainWindow::updateModeUi()
{
    const bool serial = (m_mode == Mode::Serial);
    ui->modeBtn->setIcon(themedIcon(serial ? QStringLiteral("ssh.png")
                                           : QStringLiteral("serial.png")));
    ui->modeBtn->setText(serial ? tr("SSH") : tr("串口"));
    ui->modeBtn->setToolTip(serial ? tr("切换到 SSH 终端（F9；串口连接与内容保持不断开）")
                                   : tr("切换回串口终端（F9；SSH 连接与内容保持不断开）"));
    // 工具菜单项与模式按钮同一动作，文案同步（串口页显示「切换到 SSH」，反之亦然）
    if (m_switchModeAction)
        m_switchModeAction->setText(serial ? tr("切换到 SSH") : tr("切换到串口"));
    // SFTP 走 SSH 通道：串口页不显示文件管理按钮（连同其左侧分隔线）
    ui->vline1->setVisible(!serial);
    ui->filesBtn->setVisible(!serial);
    // 内核日志静音开关只对串口控制台有意义（printk 刷不进 SSH 的 pty）
    if (m_kmsgMuteBtn)
        m_kmsgMuteBtn->setVisible(serial);
    if (!serial && !m_shellTitle.isEmpty())
        setWindowTitle(tr("Xterm — %1").arg(m_shellTitle));
    else
        setWindowTitle(serial ? tr("Xterm — 串口终端") : tr("Xterm — SSH 终端"));
}

void MainWindow::updateConnectionUi()
{
    Transport *t = activeTransport();
    const Transport::State st = t->state();
    const bool open = (st == Transport::Connected);
    const bool busy = (st == Transport::Connecting || st == Transport::Disconnecting);

    QString text;
    QString tip;
    if (st == Transport::Connecting) {
        text = tr("取消");
        tip = tr("中止本次连接");
    } else if (st == Transport::Disconnecting) {
        text = tr("断开中…");
        tip = tr("正在结束会话");
    } else if (open) {
        text = tr("断开");
        tip = m_mode == Mode::Serial ? tr("断开串口") : tr("断开 SSH 连接");
    } else {
        text = tr("连接");
        tip = m_mode == Mode::Serial ? tr("连接串口") : tr("连接 SSH 主机");
    }
    ui->connectBtn->setText(text);
    ui->connectBtn->setToolTip(tip);
    // 工具菜单项触发的是同一个按钮，文案同步（断开时显示「连接」，连接时「断开」）
    if (m_connectAction)
        m_connectAction->setText(text);
    ui->connectBtn->setIcon(themedIcon(open ? QStringLiteral("disconnect.png")
                                             : QStringLiteral("connect.png")));
    ui->connectBtn->setEnabled(st != Transport::Disconnecting);
    ui->connectBtn->setProperty("connected", open);
    ui->connectBtn->style()->unpolish(ui->connectBtn);
    ui->connectBtn->style()->polish(ui->connectBtn);

    // 参数在连接期间锁定；另一侧（后台模式）的参数不在界面上，不参与禁用逻辑
    const bool serial = (m_mode == Mode::Serial);
    const bool idle = !open && !busy;
    ui->refreshPortBtn->setEnabled(serial && idle);
    for (QComboBox *combo : {ui->portCombo, ui->baudCombo, ui->dataBitsCombo,
                             ui->parityCombo, ui->stopBitsCombo, ui->flowCombo})
        combo->setEnabled(serial && idle);
    ui->dtrCheck->setEnabled(serial && idle);
    ui->rtsCheck->setEnabled(serial && idle);
    ui->hostEdit->setEnabled(!serial && idle);
    ui->userEdit->setEnabled(!serial && idle);
    ui->sshPortCombo->setEnabled(!serial && idle);
    ui->timeoutCombo->setEnabled(!serial && idle);
    ui->keyEdit->setEnabled(!serial && idle);
    ui->keyBrowseBtn->setEnabled(!serial && idle);

    // 文件管理按钮只在 SSH 页显示（见 updateModeUi），启用与否看 SSH 链路状态（含后台）
    ui->filesBtn->setEnabled(m_ssh->state() == Transport::Connected);
}

void MainWindow::applyTheme()
{
    const bool dark = m_store->isDarkTheme();
    setIconThemeDark(dark);
    QFile f(dark ? QStringLiteral(":/style-dark.qss") : QStringLiteral(":/style-light.qss"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromUtf8(f.readAll()));
    m_serialView->applyTheme(dark);
    m_sshView->applyTheme(dark);
    setupIcons();
    updateModeUi();
    updateConnectionUi();
    // 主题切换入口在自定义标题栏，图标显示"将要切换到"的主题
    if (m_titleBar) {
        m_titleBar->setThemeIcon(
            themedIcon(dark ? QStringLiteral("sun.png") : QStringLiteral("moon.png")),
            dark ? tr("切换到亮色主题") : tr("切换到深色主题"));
        m_titleBar->applyCaptionTheme(dark); // 系统按钮字形颜色跟主题
    }
    // 公共对话框实例化时自动取用（message/question 这类静态弹窗拿不到主窗口引用）
    FramelessDialog::setThemeHint(dark);
    if (m_commandBar)
        m_commandBar->applyTheme();
    // 侧边按钮组里的 MCP 按钮也要跟主题换图标（构造时首次调用已创建）
    if (m_mcpBtn)
        refreshMcpIcon();
    fitSshPortEdit();
}

QString MainWindow::connectionSummary() const
{
    if (m_mode == Mode::Ssh) {
        // 有会话（含连接中）时以会话实际参数为准，否则回显用户当前填的内容
        const SshTarget t = (m_ssh->state() == Transport::Disconnected)
            ? sshTargetFromUi() : m_ssh->target();
        QString s;
        if (!t.user.isEmpty())
            s = t.user + QLatin1Char('@');
        s += t.host.isEmpty() ? tr("未填主机") : t.host;
        if (t.port != 22)
            s += QStringLiteral(":%1").arg(t.port);
        return s;
    }
    const QString parityChar = [](const QString &p) {
        if (p == QStringLiteral("even"))
            return QStringLiteral("E");
        if (p == QStringLiteral("odd"))
            return QStringLiteral("O");
        return QStringLiteral("N");
    }(ui->parityCombo->currentData().toString());
    const double sb = ui->stopBitsCombo->currentData().toDouble();
    return QStringLiteral("%1 @ %2 %3%4%5")
        .arg(selectedPortName(), ui->baudCombo->currentText(),
             ui->dataBitsCombo->currentText(), parityChar,
             sb == 1.5 ? QStringLiteral("1.5") : QString::number(sb));
}

void MainWindow::updatePersistentStatus()
{
    Transport *t = activeTransport();
    QString text;
    const char *tint = nullptr; // 状态色：亮/暗主题下都读得清的中性高饱和色
    switch (t->state()) {
    case Transport::Connected:
        text = (m_mode == Mode::Ssh) ? sshStatsStatusText()
                                     : tr("● %1 · 已连接").arg(connectionSummary());
        tint = "#3EA854";
        break;
    case Transport::Connecting:
        text = tr("◐ %1 · 连接中…").arg(connectionSummary());
        tint = "#D89B2C";
        break;
    case Transport::Disconnecting:
        text = tr("◌ 正在断开…");
        tint = "#9A9A9A";
        break;
    case Transport::Disconnected:
        text = tr("○ 未连接");
        break;
    }

    // 后台那条链路若仍连着，附一个简短提示，避免用户以为切没了
    Transport *other = (m_mode == Mode::Serial) ? static_cast<Transport *>(m_ssh)
                                                 : static_cast<Transport *>(m_serial);
    if (other->isOpen()) {
        const QString label = other->kind() == Transport::SshLink
            ? tr("SSH %1").arg(m_ssh->summary())
            : tr("串口 %1").arg(m_serial->portName());
        text += tr("  ·  后台 %1 已连接").arg(label);
    }

    m_statusLabel->setText(text);
    // 未连接时清空内联样式，交回主题 QSS 的中性色，不让它一直显眼
    m_statusLabel->setStyleSheet(tint
        ? QStringLiteral("color: %1; font-weight: 600;").arg(QLatin1String(tint))
        : QString());
}

QString MainWindow::sshStatsStatusText() const
{
    const SshTarget t = m_ssh->target();
    QString id;
    if (!t.user.isEmpty())
        id = t.user + QLatin1Char('@');
    id += t.host;
    id += QStringLiteral(":%1").arg(t.port > 0 ? t.port : 22);

    const QString cpu = m_hostCpuOk ? QString::number(m_hostCpu) : QStringLiteral("--");
    const QString memPct = m_hostMemPercentOk ? QString::number(m_hostMemPercent)
                                              : QStringLiteral("--");
    QString mem = QStringLiteral("--/--");
    if (m_hostMemOk && m_hostTotalKiB > 0) {
        if (m_hostTotalKiB >= 1024 * 1024) {
            const auto gib = [](qint64 kib) {
                return QString::number(double(kib) / (1024.0 * 1024.0), 'f', 1);
            };
            mem = QStringLiteral("%1/%2G").arg(gib(m_hostUsedKiB), gib(m_hostTotalKiB));
        } else {
            const auto mib = [](qint64 kib) {
                return QString::number(qRound(double(kib) / 1024.0));
            };
            mem = QStringLiteral("%1/%2M").arg(mib(m_hostUsedKiB), mib(m_hostTotalKiB));
        }
    }
    return id + QStringLiteral("      ·     CPU ") + cpu
           + QStringLiteral("% · 内存 ") + memPct
           + QStringLiteral("% · ") + mem;
}

void MainWindow::clearHostStats()
{
    m_hostCpuOk = false;
    m_hostCpu = 0;
    m_hostMemOk = false;
    m_hostMemPercentOk = false;
    m_hostMemPercent = 0;
    m_hostUsedKiB = 0;
    m_hostTotalKiB = 0;
}

bool MainWindow::confirmHostKeyChange(const QString &host, const QString &stored, const QString &current)
{
    const auto show = [](const QString &hex) {
        if (hex.contains(QLatin1Char('\n')))
            return hex;
        const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
        if (raw.isEmpty())
            return hex;
        QString b64 = QString::fromLatin1(raw.toBase64());
        while (b64.endsWith(QLatin1Char('=')))
            b64.chop(1);
        return QStringLiteral("SHA256:") + b64;
    };

    FramelessDialog dialog(this);
    dialog.setDialogTitle(tr("主机密钥已变化"));
    auto *layout = new QVBoxLayout;
    auto *intro = new QLabel(tr("主机 %1 的密钥指纹和上次记下的不一致。").arg(host));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto addLine = [&](const QString &title, const QString &hex) {
        layout->addWidget(new QLabel(title));
        auto *value = new QLabel(show(hex));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(value);
    };
    addLine(tr("上次记录"), stored);
    addLine(tr("这次连接"), current);

    auto *hint = new QLabel(tr("确认这台机器重装过或换过密钥后，再更新记录并继续。如果这台机器不该换密钥，请拒绝。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("更新并连接"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("拒绝"));
    buttons->button(QDialogButtonBox::Ok)->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Ok)->setDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.setMinimumWidth(460);
    dialog.setContentLayout(layout);
    return dialog.exec() == QDialog::Accepted;
}

void MainWindow::promptHostKeyDecision(const SshHostKeyChange &change)
{
    const bool accept = confirmHostKeyChange(
        change.host, change.oldFingerprint, change.newFingerprint);
    m_ssh->answerHostKey(change.requestId, accept);
}

void MainWindow::promptAuthAnswers(SshRequestId requestId,
                                   const QList<SshAuthPrompt> &prompts)
{
    FramelessDialog dialog(this);
    dialog.setDialogTitle(tr("SSH 认证"));
    auto *layout = new QVBoxLayout;
    auto *form = new QFormLayout;
    QList<QLineEdit *> edits;
    for (const SshAuthPrompt &prompt : prompts) {
        auto *edit = new QLineEdit;
        edit->setEchoMode(prompt.echo ? QLineEdit::Normal : QLineEdit::Password);
        form->addRow(prompt.text, edit);
        edits.append(edit);
    }
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.setMinimumWidth(420);
    dialog.setContentLayout(layout);

    if (dialog.exec() != QDialog::Accepted) {
        m_ssh->close();
        return;
    }

    QStringList answers;
    answers.reserve(edits.size());
    for (QLineEdit *edit : edits)
        answers.append(edit->text());
    m_ssh->answerAuthentication(requestId, answers);
}

// ---------------------------------------------------------------- 本机 MCP

void MainWindow::setMcpActive(bool on)
{
    if (m_mcpActive == on)
        return;
    m_mcpActive = on;
    refreshMcpIcon();
}

void MainWindow::refreshMcpIcon()
{
    if (!m_mcpBtn)
        return;
    QPixmap pm = themedIcon(QStringLiteral("mcp.png")).pixmap(16, 16);
    if (m_mcpActive) {
        // 右下角绿色小圆点: AI 正在通过 MCP 访问本窗口
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(76, 175, 80));
        p.drawEllipse(QRectF(9.5, 9.5, 6.0, 6.0));
        p.end();
    }
    m_mcpBtn->setIcon(QIcon(pm));
    m_mcpBtn->setToolTip(m_mcpActive ? tr("MCP 在线（AI 正在访问本窗口），点击查看提示词")
                                     : tr("显示 MCP 提示词"));
}

QJsonObject MainWindow::agentStatus() const
{
    Transport *t = activeTransport();
    const ScreenView *view = (m_mode == Mode::Serial) ? m_serialView : m_sshView;
    QString state = QStringLiteral("disconnected");
    switch (t->state()) {
    case Transport::Connected:
        state = QStringLiteral("connected");
        break;
    case Transport::Connecting:
        state = QStringLiteral("connecting");
        break;
    case Transport::Disconnecting:
        state = QStringLiteral("disconnecting");
        break;
    case Transport::Disconnected:
        break;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("mode"),
               m_mode == Mode::Serial ? QStringLiteral("serial") : QStringLiteral("ssh"));
    obj.insert(QStringLiteral("connected"), t->state() == Transport::Connected);
    obj.insert(QStringLiteral("state"), state);
    obj.insert(QStringLiteral("summary"), connectionSummary());
    obj.insert(QStringLiteral("alt_screen"), view && view->usingAltScreen());
    return obj;
}

QString MainWindow::agentRecentText(int maxLines) const
{
    const ScreenView *view = (m_mode == Mode::Serial) ? m_serialView : m_sshView;
    return view ? view->recentText(maxLines) : QString();
}

QString MainWindow::agentSendBytes(const QByteArray &bytes)
{
    Transport *t = activeTransport();
    if (!t || t->state() != Transport::Connected)
        return tr("未连接");
    return t->send(bytes);
}

QString MainWindow::agentSendLine(const QString &text)
{
    if (text.trimmed() == QLatin1String("clear -a")) {
        ScreenView *view = (m_mode == Mode::Serial) ? m_serialView : m_sshView;
        if (view) {
            view->rememberSentLine(text.trimmed());
            view->clearLocalDisplay();
        }
        return QString();
    }
    Transport *t = activeTransport();
    if (!t || t->state() != Transport::Connected)
        return tr("未连接");
    QByteArray bytes = text.toUtf8();
    while (bytes.endsWith('\n') || bytes.endsWith('\r'))
        bytes.chop(1);
    bytes.append(m_mode == Mode::Serial ? '\n' : '\r');
    ScreenView *view = (m_mode == Mode::Serial) ? m_serialView : m_sshView;
    if (view) {
        view->rememberSentLine(QString::fromUtf8(bytes.chopped(1)));
        view->showOutbound(bytes);
    }
    return t->send(bytes);
}

void MainWindow::onMcpClicked()
{
    QString script = QDir(QCoreApplication::applicationDirPath())
                         .absoluteFilePath(QStringLiteral("../../tools/xterm_mcp/server.py"));
    script = QDir::cleanPath(script);
    if (!QFileInfo::exists(script))
        script = QStringLiteral("tools/xterm_mcp/server.py");
    script = QDir::fromNativeSeparators(script);

    const QString config = QStringLiteral(
                               "{\n"
                               "  \"mcpServers\": {\n"
                               "    \"xterm\": {\n"
                               "      \"command\": \"python\",\n"
                               "      \"args\": [\"%1\"]\n"
                               "    }\n"
                               "  }\n"
                               "}")
                               .arg(script);

    FramelessDialog dlg(this);
    dlg.setDialogTitle(tr("MCP 提示词"));
    dlg.resize(680, 520);
    auto *lay = new QVBoxLayout;
    auto *hint = new QLabel(tr("把提示词交给已经接上 Xterm MCP 的 AI。服务程序只连接本机正在运行的这个窗口。"));
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto *edit = new QPlainTextEdit(AgentBridge::instructions());
    edit->setReadOnly(true);
    lay->addWidget(edit, 1);
    lay->addWidget(new QLabel(tr("MCP 配置：")));
    auto *cfg = new QPlainTextEdit(config);
    cfg->setReadOnly(true);
    cfg->setFixedHeight(140);
    lay->addWidget(cfg);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton(tr("复制提示词"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [edit] {
        QGuiApplication::clipboard()->setText(edit->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.setContentLayout(lay);
    dlg.exec();
}

// ---------------------------------------------------------------- 关键词高亮

void MainWindow::applyKeywords(const QList<KeywordRule> &rules)
{
    m_store->setKeywords(rules);
    m_serialView->setKeywords(rules);
    m_sshView->setKeywords(rules);
}

void MainWindow::onHighlightRequested(const QString &word)
{
    if (word.isEmpty())
        return;
    QList<KeywordRule> rules = m_store->keywords();
    for (KeywordRule &rule : rules) {
        // 已有同名规则（不区分大小写）就只点亮它
        if (rule.text.compare(word, Qt::CaseInsensitive) == 0) {
            rule.enabled = true;
            applyKeywords(rules);
            m_store->save();
            ui->statusbar->showMessage(tr("已高亮「%1」").arg(word), 3000);
            return;
        }
    }
    KeywordRule rule;
    rule.text = word;
    rule.color = nextKeywordColor(rules.size());
    rules.append(rule);
    applyKeywords(rules);
    m_store->save();
    ui->statusbar->showMessage(tr("已高亮「%1」").arg(word), 3000);
}

void MainWindow::openKeywordSettings()
{
    FramelessDialog dlg(this);
    dlg.setDialogTitle(tr("关键词高亮"));
    dlg.resize(440, 360);

    const QList<KeywordRule> original = m_store->keywords(); // 取消时回滚
    auto *content = new QWidget;
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(12, 10, 12, 12);

    auto *table = new QTableWidget;
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({ tr("关键词"), tr("颜色"), tr("启用") });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setColumnWidth(1, 110);
    table->setColumnWidth(2, 52);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);

    // 颜色列 = 终端里的真实效果预览：关键词字样（跟随第 0 列的输入）用规则色
    // 画在终端底色上（暗 #1E1E1E / 亮 #FFFFFF，与 TermScreen 调色板一致）
    const bool dark = m_store->isDarkTheme();
    auto swatchStyle = [dark](const QColor &color) {
        return QStringLiteral(
                   "QPushButton { background-color: %1; color: %2; border: 1px solid %3;"
                   " border-radius: 3px; font-weight: 600; }")
            .arg(dark ? QStringLiteral("#1E1E1E") : QStringLiteral("#FFFFFF"),
                 color.name(),
                 dark ? QStringLiteral("#3B3B40") : QStringLiteral("#C9C9CD"));
    };
    // 表格 -> 规则列表 -> 立即预览（确定才落盘，取消回滚）
    auto applyFromTable = [&]() {
        QList<KeywordRule> rules;
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto *textItem = table->item(row, 0);
            const auto *colorItem = table->item(row, 1);
            const auto *checkItem = table->item(row, 2);
            KeywordRule rule;
            rule.text = textItem ? textItem->text().trimmed() : QString();
            rule.color = colorItem ? QColor(colorItem->text()) : KeywordRule().color;
            rule.enabled = !checkItem || checkItem->checkState() == Qt::Checked;
            if (!rule.text.isEmpty() && rule.color.isValid())
                rules.append(rule);
        }
        applyKeywords(rules);
    };
    auto addRow = [&](const QString &text, const QColor &color, bool enabled) {
        const int row = table->rowCount();
        table->insertRow(row);
        auto *textItem = new QTableWidgetItem(text);
        table->setItem(row, 0, textItem);
        // 颜色存在 item 文本里（applyFromTable 读它），显示交给覆盖在上面的色块按钮
        auto *colorItem = new QTableWidgetItem(color.name());
        colorItem->setFlags(colorItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 1, colorItem);
        auto *swatch = new QPushButton(text);
        swatch->setCursor(Qt::PointingHandCursor);
        swatch->setStyleSheet(swatchStyle(color));
        // cellWidget 会吃掉表格的双击事件，取色器直接挂在按钮上，单击即改。
        // swatch 必须按值捕获：它是 addRow 的局部变量，按引用捕获在
        // addRow 返回后即悬垂（栈槽被下一次 addRow 复用，样式会刷错行）
        connect(swatch, &QPushButton::clicked, this, [&, row, swatch] {
            const QColor picked = FramelessDialog::getColor(
                &dlg, QColor(table->item(row, 1)->text()), tr("关键词颜色"));
            if (!picked.isValid())
                return;
            table->item(row, 1)->setText(picked.name());
            swatch->setStyleSheet(swatchStyle(picked));
            applyFromTable();
        });
        table->setCellWidget(row, 1, swatch);
        auto *checkItem = new QTableWidgetItem;
        checkItem->setFlags((checkItem->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        checkItem->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
        table->setItem(row, 2, checkItem);
    };
    for (const KeywordRule &rule : original)
        addRow(rule.text, rule.color, rule.enabled);

    auto *addBtn = new QPushButton(tr("添加"));
    auto *delBtn = new QPushButton(tr("删除"));
    auto *defaultsBtn = new QPushButton(tr("默认规则"));
    auto *okBtn = new QPushButton(tr("确定"));
    okBtn->setDefault(true);
    auto *cancelBtn = new QPushButton(tr("取消"));
    connect(addBtn, &QPushButton::clicked, this, [&] {
        const QColor color = nextKeywordColor(table->rowCount());
        addRow(tr("新关键词"), color, true);
        applyFromTable();
        table->editItem(table->item(table->rowCount() - 1, 0));
    });
    connect(delBtn, &QPushButton::clicked, this, [&] {
        const int row = table->currentRow();
        if (row >= 0) {
            table->removeRow(row);
            applyFromTable();
        }
    });
    // 一键补齐内置日志惯例规则（error 红 / success 绿…），已有关键词不动
    connect(defaultsBtn, &QPushButton::clicked, this, [&] {
        const QList<KeywordRule> defaults = CommandStore::defaultKeywords(dark);
        for (const KeywordRule &rule : defaults) {
            bool exists = false;
            for (int row = 0; row < table->rowCount(); ++row) {
                const auto *item = table->item(row, 0);
                if (item && item->text().trimmed().compare(rule.text, Qt::CaseInsensitive) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists)
                addRow(rule.text, rule.color, rule.enabled);
        }
        applyFromTable();
    });
    connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    // 关键词编辑 / 启用勾选变化 -> 实时预览；关键词变了，色块上的预览字样跟着变
    connect(table, &QTableWidget::itemChanged, this, [&](QTableWidgetItem *item) {
        if (item->column() == 0) {
            if (auto *btn = qobject_cast<QPushButton *>(table->cellWidget(item->row(), 1)))
                btn->setText(item->text());
        }
        if (item->column() != 0 && item->column() != 2)
            return;
        applyFromTable();
    });

    auto *hint = new QLabel(tr("命中不区分大小写，终端与滚动历史同时生效。颜色列即终端实际效果，点击改色。"));
    hint->setWordWrap(true);

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(addBtn);
    btnRow->addWidget(delBtn);
    btnRow->addWidget(defaultsBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(okBtn);
    btnRow->addWidget(cancelBtn);

    lay->addWidget(table, 1);
    lay->addLayout(btnRow);
    lay->addWidget(hint);
    dlg.setContentLayout(lay);

    if (dlg.exec() == QDialog::Accepted)
        m_store->save();
    else
        applyKeywords(original); // 取消：还原打开前的规则
}

void MainWindow::openFontSettings()
{
    FramelessDialog dlg(this);
    dlg.setDialogTitle(tr("字体"));
    dlg.resize(480, 400);

    // 取消要回滚：记下打开前的字体族与字号
    const QString prevFamily = m_serialView->fontFamily();
    const int prevSize = m_serialView->fontSize();

    auto *content = new QWidget;
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(18, 14, 18, 14);
    lay->setSpacing(12);

    // 预览：与终端同底色，改字体/字号立刻看到真实效果
    const bool dark = m_store->isDarkTheme();
    auto *preview = new QLabel;
    preview->setMinimumHeight(100);
    preview->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    preview->setText(QStringLiteral("root@HPCS3900:~# uname -a\n"
                                    "Linux HPCS3900 5.10.0 aarch64 GNU/Linux\n"
                                    "root@HPCS3900:~# free -m"));
    preview->setStyleSheet(QStringLiteral(
                               "QLabel { background-color: %1; color: %2; border: 1px solid %3;"
                               " border-radius: 6px; padding: 10px 12px; }")
                               .arg(dark ? QStringLiteral("#1E1E1E") : QStringLiteral("#FFFFFF"),
                                    dark ? QStringLiteral("#D4D4D4") : QStringLiteral("#1F1F1F"),
                                    dark ? QStringLiteral("#3B3B40") : QStringLiteral("#C9C9CD")));

    // 只列等宽字体：比例字体在格子里画终端会错位
    auto *fontCombo = new QFontComboBox;
    fontCombo->setFontFilters(QFontComboBox::MonospacedFonts);
    fontCombo->setCurrentFont(QFont(m_serialView->fontFamily()));
    fontCombo->setMinimumWidth(250);

    auto *sizeSpin = new QSpinBox;
    sizeSpin->setRange(8, 36);
    sizeSpin->setSuffix(tr(" pt"));
    sizeSpin->setValue(m_serialView->fontSize());
    sizeSpin->setFixedWidth(96);

    auto *form = new QGridLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    form->addWidget(new QLabel(tr("字体")), 0, 0);
    form->addWidget(fontCombo, 0, 1, Qt::AlignLeft);
    form->addWidget(new QLabel(tr("字号")), 1, 0);
    form->addWidget(sizeSpin, 1, 1, Qt::AlignLeft);

    // 预览跟随：改动先落到两个终端页（信令再同步存储），预览单独设字
    auto applyLive = [&](const QString &family, int pt) {
        m_serialView->setFontFamily(family);
        m_sshView->setFontFamily(family);
        m_serialView->setFontSize(pt);
        m_sshView->setFontSize(pt);
        QFont f(m_serialView->fontFamily(), pt);
        f.setStyleHint(QFont::Monospace);
        f.setFixedPitch(true);
        f.setKerning(false);
        preview->setFont(f);
    };
    connect(fontCombo, &QFontComboBox::currentFontChanged, this,
            [&](const QFont &f) { applyLive(f.family(), sizeSpin->value()); });
    connect(sizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [&](int pt) { applyLive(fontCombo->currentFont().family(), pt); });
    applyLive(m_serialView->fontFamily(), m_serialView->fontSize());

    auto *resetBtn = new QPushButton(tr("恢复默认字体"));
    auto *okBtn = new QPushButton(tr("确定"));
    okBtn->setDefault(true);
    auto *cancelBtn = new QPushButton(tr("取消"));
    connect(resetBtn, &QPushButton::clicked, this, [&] {
        fontCombo->setCurrentFont(QFont(ScreenView::defaultFontFamily()));
    });
    connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    auto *hint = new QLabel(tr("只列出本机已装的等宽字体；字号也可以直接在终端里 Ctrl+滚轮 调整。"));
    hint->setWordWrap(true);

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(resetBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(okBtn);
    btnRow->addWidget(cancelBtn);

    lay->addWidget(preview);
    lay->addLayout(form);
    lay->addWidget(hint);
    lay->addStretch(1);
    lay->addLayout(btnRow);
    dlg.setContentLayout(lay);

    if (dlg.exec() == QDialog::Accepted) {
        // 显式写一次：用户没改动时也保证存储与当前显示一致。
        // 等于默认族时存空串，保留“未设置”语义
        const QString family = m_serialView->fontFamily();
        m_store->setTerminalFont(family == ScreenView::defaultFontFamily() ? QString() : family);
        m_store->setFontSize(m_serialView->fontSize());
        m_store->save();
    } else {
        m_serialView->setFontFamily(prevFamily);
        m_sshView->setFontFamily(prevFamily);
        m_serialView->setFontSize(prevSize);
        m_sshView->setFontSize(prevSize);
    }
}

// ---------------------------------------------------------------- 发送

void MainWindow::sendRaw(const QByteArray &bytes)
{
    Transport *t = activeTransport();
    if (t->state() != Transport::Connected) {
        // 明确提示而不是静默无反应
        const bool ssh = (m_mode == Mode::Ssh);
        const QString why = t->state() == Transport::Connecting
            ? (ssh ? tr("SSH 正在连接中") : tr("串口正在连接中"))
            : (ssh ? tr("SSH 未连接") : tr("串口未连接"));
        ui->statusbar->showMessage(tr("%1，无法发送 —— 请先点“连接”").arg(why), 5000);
        appendActiveNote(tr("── 未连接，命令未发送 ──"));
        ui->connectBtn->setFocus(); // 焦点移到"连接"按钮，引导用户
        return;
    }
    const QString err = t->send(bytes);
    if (!err.isEmpty())
        ui->statusbar->showMessage(err, 8000);
}

void MainWindow::sendLine(const QString &line)
{
    // 命令栏按钮也是用户操作：同样优先于 AI 打字机
    if (m_agent && m_agent->interruptTyping())
        ui->statusbar->showMessage(tr("已打断 AI 输入"), 3000);
    if (line.trimmed() == QLatin1String("clear -a")) {
        ScreenView *view = (m_mode == Mode::Serial) ? m_serialView : m_sshView;
        if (view) {
            view->rememberSentLine(line.trimmed());
            view->clearLocalDisplay();
        }
        return;
    }
    ScreenView *view = (m_mode == Mode::Serial) ? m_serialView : m_sshView;
    if (view)
        view->rememberSentLine(line);
    sendRaw(line.toUtf8() + QByteArrayLiteral("\n"));
}

// ------------------------------------------------------------ 内核日志静音开关
//
// 设备侧 printk（WiFi/蓝牙驱动刷屏）会异步直写串口控制台，插进 vi/top
// 画面字节流的任意位置，任何终端仿真都无法分离——唯一根治是降低
// console_loglevel。开关做的事只是发送：
//   静音: echo 1 > /proc/sys/kernel/printk（仅 KERN_EMERG 上串口）
//   恢复: echo 7 > /proc/sys/kernel/printk
// 客户端记住的是「期望状态」；设备重启/重连后 printk 回默认值，状态随之
// 重置（见 opened/closed），避免显示与实际脱节。
void MainWindow::toggleKmsgMute()
{
    if (m_serial->state() != Transport::Connected) {
        ui->statusbar->showMessage(tr("串口未连接，无法切换内核日志状态"), 5000);
        return;
    }
    m_kmsgMuted = !m_kmsgMuted;
    sendLine(QStringLiteral("echo %1 > /proc/sys/kernel/printk")
                 .arg(m_kmsgMuted ? 1 : 7));
    updateKmsgMuteUi();
    m_serialView->appendSystemMessage(m_kmsgMuted
        ? tr("── 内核日志已静音（仅 EMERG 级上串口，dmesg 仍可查看）──")
        : tr("── 内核日志已恢复输出 ──"));
}

void MainWindow::updateKmsgMuteUi()
{
    if (!m_kmsgMuteBtn)
        return;
    m_kmsgMuteBtn->setText(m_kmsgMuted ? tr("内核日志：静音") : tr("内核日志：正常"));
    m_kmsgMuteBtn->setToolTip(m_kmsgMuted
        ? tr("内核日志已静音（echo 1 > /proc/sys/kernel/printk）。\n"
             "点击恢复输出（echo 7）。设备重启后需重新静音。")
        : tr("静音内核日志刷屏（echo 1 > /proc/sys/kernel/printk）：\n"
             "WiFi/蓝牙驱动的 printk 会插进 vi/top 的画面，静音后仅紧急级\n"
             "日志上串口，dmesg 随时仍可查看。再次点击恢复（echo 7）。"));
    // 静音是「非默认」状态，用琥珀色提示（与「连接中」同色，两主题都读得清）
    m_kmsgMuteBtn->setStyleSheet(m_kmsgMuted
        ? QStringLiteral("color: #D89B2C; font-weight: 600;")
        : QString());
}

// ---------------------------------------------------------------- 配置

Profile MainWindow::currentProfileFromUi() const
{
    Profile p;
    p.name = m_currentProfile;
    p.connection.port = selectedPortName();
    p.connection.baud = ui->baudCombo->currentText().toInt();
    p.connection.dataBits = ui->dataBitsCombo->currentData().toInt();
    p.connection.parity = ui->parityCombo->currentData().toString();
    p.connection.stopBits = ui->stopBitsCombo->currentData().toDouble();
    p.connection.flowControl = ui->flowCombo->currentData().toString();
    p.connection.dtr = ui->dtrCheck->isChecked();
    p.connection.rts = ui->rtsCheck->isChecked();
    p.connection.pureTerminal = ui->serialModeBtn->isChecked();
    p.ssh = sshTargetFromUi();
    p.commands = m_commandBar->commands();
    return p;
}

void MainWindow::syncCurrentProfile()
{
    if (m_currentProfile.isEmpty() || !m_store->hasProfile(m_currentProfile))
        return;
    m_store->upsertProfile(currentProfileFromUi()); // 内存立即生效（切换配置/退出都不丢）
    scheduleProfileSave();                          // 写盘防抖合并
}

void MainWindow::scheduleProfileSave()
{
    // 400ms 内再有变化就重新计时，最终只写一次盘。退出路径 closeEvent()
    // 里有显式 save()，挂起中的定时器不会造成数据丢失。
    m_saveTimer->start(400);
}

void MainWindow::reloadProfiles(const QString &selectName)
{
    QString name = selectName;
    if (name.isEmpty() || !m_store->hasProfile(name)) {
        const QList<Profile> profiles = m_store->profiles();
        name = profiles.isEmpty() ? QString() : profiles.first().name;
    }
    if (name.isEmpty())
        return;
    m_currentProfile = name;
    m_store->setLastProfile(name);
    applyCurrentProfile();
}

void MainWindow::applyCurrentProfile()
{
    if (m_currentProfile.isEmpty())
        return;
    const Profile p = m_store->profile(m_currentProfile);

    m_loading = true;
    if (!m_serial->isOpen() && !p.connection.port.isEmpty()) {
        const int i = ui->portCombo->findData(p.connection.port);
        if (i >= 0)
            ui->portCombo->setCurrentIndex(i);
    }
    if (!m_serial->isOpen()) {
        ui->baudCombo->setCurrentText(QString::number(p.connection.baud));
        for (int i = 0; i < ui->dataBitsCombo->count(); ++i)
            if (ui->dataBitsCombo->itemData(i).toInt() == p.connection.dataBits)
                ui->dataBitsCombo->setCurrentIndex(i);
        for (int i = 0; i < ui->parityCombo->count(); ++i)
            if (ui->parityCombo->itemData(i).toString() == p.connection.parity)
                ui->parityCombo->setCurrentIndex(i);
        for (int i = 0; i < ui->stopBitsCombo->count(); ++i)
            if (qFuzzyCompare(ui->stopBitsCombo->itemData(i).toDouble(), p.connection.stopBits))
                ui->stopBitsCombo->setCurrentIndex(i);
        for (int i = 0; i < ui->flowCombo->count(); ++i)
            if (ui->flowCombo->itemData(i).toString() == p.connection.flowControl)
                ui->flowCombo->setCurrentIndex(i);
        ui->dtrCheck->setChecked(p.connection.dtr);
        ui->rtsCheck->setChecked(p.connection.rts);
    }
    // 终端模式只影响本地行为，不动串口链路，连接中也按配置生效
    ui->serialModeBtn->setChecked(p.connection.pureTerminal);
    // SSH 参数：已连接或正在连的时候不动，避免改动把用户从当前会话上带偏
    if (!m_ssh->isOpen() && m_ssh->state() == Transport::Disconnected) {
        ui->hostEdit->setText(p.ssh.host);
        ui->userEdit->setText(p.ssh.user);
        ui->sshPortCombo->setEditText(QString::number(p.ssh.port > 0 ? p.ssh.port : 22));
        pruneCustomPortItems(ui->sshPortCombo);
        ui->timeoutCombo->setCurrentText(QString::number(p.ssh.connectTimeoutSec));
        ui->passwordEdit->setText(p.ssh.password);
        ui->keyEdit->setText(p.ssh.privateKey);
    }
    m_loading = false;

    m_commandBar->setCommands(p.commands);
    if (m_completion) {
        QStringList names;
        for (const CommandItem &c : p.commands)
            names << c.text;
        m_completion->setUserCommands(names);
    }
    m_store->save();
}

// ---------------------------------------------------------------- 事件

void MainWindow::closeEvent(QCloseEvent *event)
{
    syncCurrentProfile();
    m_store->setMode(m_mode == Mode::Serial ? QStringLiteral("serial")
                                            : QStringLiteral("ssh"));
    m_store->save();
    m_serial->close();
    m_ssh->close();
    event->accept();
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange) {
        // 窗口化时外围留 6px 呼吸空间（配合面板圆角）；
        // 最大化时清零留白和终端网格内边距，让串口/SSH 终端贴满整个窗口。
        const bool max = isMaximized();
        const int m = max ? 0 : 6;
        ui->mainLayout->setContentsMargins(m, m, m, m);
        ui->terminalLayout->setContentsMargins(m, m, m, m);
        m_serialView->setPad(m);
        m_sshView->setPad(m);
    }
    QMainWindow::changeEvent(event);
}
