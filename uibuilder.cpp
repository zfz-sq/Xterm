#include "uibuilder.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFontComboBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "commandbar.h"
#include "framelessdialog.h"
#include "hotkeymanager.h"
#include "icons.h"
#include "kmsgmutetoggle.h"
#include "mcpmainhook.h"
#include "screenview.h"
#include "serialsession.h"
#include "sessioncontroller.h"
#include "sshsession.h"
#include "thememanager.h"
#include "titlebar.h"
#include "transport.h"
#include "ui_mainwindow.h"

namespace {
// 可编辑下拉的统一防卡顿设置：默认补全会在弹出时再滤一遍，列表一长就顿。
void tuneEditableCombo(QComboBox *combo, int maxVisible = 20)
{
    combo->setCompleter(nullptr);
    combo->setMaxVisibleItems(maxVisible);
    if (auto *list = qobject_cast<QListView *>(combo->view()))
        list->setUniformItemSizes(true);
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

UiBuilder::UiBuilder(QMainWindow *window, Ui::MainWindow *ui, CommandStore *store,
                     ThemeManager *theme, HotkeyManager *hotkey, McpMainHook *mcp,
                     QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_ui(ui)
    , m_store(store)
    , m_theme(theme)
    , m_hotkey(hotkey)
    , m_mcp(mcp)
{
    // 外围留白随窗口状态初始化（窗口化 6px / 最大化 0）
    const int m = window->isMaximized() ? 0 : 6;
    m_ui->mainLayout->setContentsMargins(m, m, m, m);
    m_ui->terminalLayout->setContentsMargins(m, m, m, m);

    setupTitleBar(); // 须在主窗口展示前：无边框 flags + menuWidget
    setupIcons();
    setupConnectionCombos();
    setupSshInputs();
    setupTerminal();

    m_commandBar = new CommandBar(m_window);
    m_ui->commandBarLayout->addWidget(m_commandBar);
    // 命令栏收起状态持久化: 下次启动直接恢复侧边按钮组形态
    connect(m_commandBar, &CommandBar::collapsedChanged, this, [this](bool collapsed) {
        m_store->setCommandBarCollapsed(collapsed);
        m_store->save();
    });

    setupStatusBar();

    // 密码明文/密文切换（纯显示行为，不涉会话）
    connect(m_ui->pwToggleBtn, &QToolButton::clicked, this, [this] {
        const bool hidden = m_ui->passwordEdit->echoMode() == QLineEdit::Password;
        m_ui->passwordEdit->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
        m_ui->pwToggleBtn->setIcon(themedIcon(hidden ? QStringLiteral("eye_off.png")
                                                     : QStringLiteral("eye.png")));
        m_ui->pwToggleBtn->setToolTip(hidden ? tr("隐藏密码") : tr("显示密码"));
    });
    // settingsBtn 非 checkable，clicked(false) 会让 setVisible(false) 永远收起，改用 lambda 切换
    connect(m_ui->settingsBtn, &QToolButton::clicked, this, [this] {
        m_ui->advancedStack->setCurrentIndex(0);
        m_ui->advancedBar->setVisible(!m_ui->advancedBar->isVisible()
                                      || m_ui->advancedStack->currentIndex() != 0);
    });
    connect(m_ui->sshSettingsBtn, &QToolButton::clicked, this, [this] {
        m_ui->advancedStack->setCurrentIndex(1);
        m_ui->advancedBar->setVisible(!m_ui->advancedBar->isVisible()
                                      || m_ui->advancedStack->currentIndex() != 1);
    });
}

void UiBuilder::setSessionController(SessionController *session)
{
    m_session = session;
    m_kmsgBtn->setSerial(session->serial()); // 创建时无会话，这里补注

    // ---- 连接参数控件 → 会话控制器 ----
    connect(m_ui->refreshPortBtn, &QToolButton::clicked, m_session,
            &SessionController::refreshPorts);
    connect(m_ui->connectBtn, &QPushButton::clicked, m_session,
            &SessionController::toggleConnect);
    connect(m_ui->portCombo, &QComboBox::currentIndexChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->baudCombo, &QComboBox::currentTextChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->serialModeBtn, &QToolButton::toggled, m_session,
            &SessionController::onSerialModeToggled);
    connect(m_ui->dataBitsCombo, &QComboBox::currentIndexChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->parityCombo, &QComboBox::currentIndexChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->stopBitsCombo, &QComboBox::currentIndexChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->flowCombo, &QComboBox::currentIndexChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->dtrCheck, &QCheckBox::toggled, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->rtsCheck, &QCheckBox::toggled, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->hostEdit, &QLineEdit::textChanged, m_session,
            &SessionController::onConnectionParamChanged);
    // 密码/主机/用户名输完直接回车 = 连接，符合最常见操作习惯
    connect(m_ui->hostEdit, &QLineEdit::returnPressed, m_session,
            &SessionController::toggleConnect);
    connect(m_ui->sshPortCombo, &QComboBox::currentTextChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->userEdit, &QLineEdit::textChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->userEdit, &QLineEdit::returnPressed, m_session,
            &SessionController::toggleConnect);
    connect(m_ui->passwordEdit, &QLineEdit::textChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->passwordEdit, &QLineEdit::returnPressed, m_session,
            &SessionController::toggleConnect);
    connect(m_ui->keyEdit, &QLineEdit::textChanged, m_session,
            &SessionController::onConnectionParamChanged);
    connect(m_ui->keyBrowseBtn, &QToolButton::clicked, m_session,
            &SessionController::browsePrivateKey);
    connect(m_ui->filesBtn, &QToolButton::clicked, m_session,
            &SessionController::openFileBrowser);
    connect(m_ui->modeBtn, &QToolButton::clicked, m_session,
            &SessionController::toggleMode);
    connect(m_commandBar, &CommandBar::commandRequested, m_session,
            &SessionController::sendLine);
    connect(m_commandBar, &CommandBar::commandsChanged, m_session,
            &SessionController::onCommandsChanged);

    // ---- 快捷键行为（F5 连接 / F9 切模式 / F10 SFTP，action 归 HotkeyManager）----
    connect(m_hotkey, &HotkeyManager::connectRequested, m_session,
            &SessionController::toggleConnect);
    connect(m_hotkey, &HotkeyManager::sftpRequested, m_session,
            &SessionController::onSftpRequested);
    connect(m_hotkey, &HotkeyManager::switchModeRequested, m_session,
            &SessionController::toggleMode);

    // ---- 会话状态 → 控件刷新 ----
    connect(m_session, &SessionController::modeChanged, this, [this] {
        updateModeUi();
        updateConnectionUi();
        updatePersistentStatus();
    });
    connect(m_session, &SessionController::connectionStateChanged, this, [this] {
        updateConnectionUi();
        updatePersistentStatus();
    });
    connect(m_session, &SessionController::statsChanged, this,
            &UiBuilder::updatePersistentStatus);

    // 两条链路断开即熄灭 XOFF 提示（设备侧流控状态已不可知）
    connect(m_session->serial(), &SerialSession::closed, m_xoffLabel, &QLabel::hide);
    connect(m_session->ssh(), &SshSession::closed, m_xoffLabel, &QLabel::hide);

    // ---- 内核日志静音开关：命令走主发送链路，状态随连接事件重置 ----
    connect(m_kmsgBtn, &KmsgMuteToggle::commandRequested, m_session,
            &SessionController::sendLine);
    connect(m_kmsgBtn, &KmsgMuteToggle::noticeRequested, m_serialView,
            &ScreenView::appendSystemMessage);
    connect(m_kmsgBtn, &KmsgMuteToggle::statusMessageRequested, this,
            [this](const QString &text, int timeoutMs) {
                m_ui->statusbar->showMessage(text, timeoutMs);
            });
    connect(m_session->serial(), &SerialSession::opened, m_kmsgBtn,
            &KmsgMuteToggle::reset);
    connect(m_session->serial(), &SerialSession::closed, m_kmsgBtn,
            &KmsgMuteToggle::reset);
}

// ---------------------------------------------------------------- 初始化

void UiBuilder::setupTitleBar()
{
    // 无边框窗口 + 自定义标题栏：暗色主题下标题栏不再是系统白条，
    // 且标题栏升级为 VSCode 式工具条（菜单 / 主题切换 / 置顶 / 系统按钮）。
    m_window->setWindowFlags(m_window->windowFlags() | Qt::FramelessWindowHint);

    m_titleBar = new TitleBar(m_window);
    m_window->setMenuWidget(m_titleBar); // QMainWindow 顶部专属位，不占 centralWidget

    // VSCode 式菜单栏：设置 / 工具 / 帮助（复用已有按钮/快捷键行为，避免另起一套逻辑）
    auto *setMenu = new QMenu(m_titleBar);
    setMenu->addAction(tr("切换主题"), this, [this] {
        m_theme->toggleTheme();
        m_store->save();
    });
    setMenu->addAction(tr("关键词高亮"), this, &UiBuilder::openKeywordSettings);
    setMenu->addAction(tr("字体"), this, &UiBuilder::openFontSettings);
    setMenu->addSeparator();
    auto *quitAction = setMenu->addAction(tr("退出"), m_window, &QMainWindow::close);
    quitAction->setShortcut(QKeySequence::Quit);
    m_titleBar->addMenuButton(tr("设置"))->setMenu(setMenu);

    auto *toolMenu = new QMenu(m_titleBar);
    // F5/F9/F10 由 HotkeyManager 统一注册，这里只把 action 挂进菜单；
    // 触发信号在 setSessionController 里接到 SessionController 的既有动作上
    //（文案联动见 updateConnectionUi/updateModeUi）
    toolMenu->addAction(m_hotkey->connectAction());
    toolMenu->addAction(m_hotkey->sftpAction());
    toolMenu->addAction(m_hotkey->switchModeAction());
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
        FramelessDialog dlg(m_window);
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
        dlg.applyTheme(m_theme->isDark());
        dlg.exec();
    });
    helpMenu->addAction(tr("关于 Xterm"), this, [this] {
        // 公共无边框对话框，主题随全局 QSS，暗色下不再露原生样式。
        // 版式: 居中品牌区 → 简介 → 三张信息卡片 → 分隔线 → 页脚(链接/版权/按钮)
        FramelessDialog dlg(m_window);
        dlg.setDialogTitle(tr("关于 Xterm"));
        dlg.setMinimumWidth(500);
        const bool dark = m_theme->isDark();
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
        m_theme->toggleTheme();
        m_store->save();
    });
    connect(m_titleBar, &TitleBar::minimizedRequested, this, [this] {
        m_window->setWindowState(m_window->windowState() | Qt::WindowMinimized);
    });
    connect(m_titleBar, &TitleBar::maximizedToggled, this, [this] {
        m_window->setWindowState(m_window->isMaximized()
                                     ? m_window->windowState() & ~Qt::WindowMaximized
                                     : m_window->windowState() | Qt::WindowMaximized);
    });
    connect(m_titleBar, &TitleBar::closedRequested, m_window, &QMainWindow::close);

    applyWin11RoundedCorners(m_window);
}

void UiBuilder::setupIcons()
{
    m_ui->refreshPortBtn->setIcon(themedIcon(QStringLiteral("refresh.png")));
    m_ui->settingsBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    m_ui->settingsBtn->setIconSize(QSize(16, 16));
    m_ui->sshSettingsBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    m_ui->sshSettingsBtn->setIconSize(QSize(16, 16));
    const bool passwordHidden = m_ui->passwordEdit->echoMode() == QLineEdit::Password;
    m_ui->pwToggleBtn->setIcon(themedIcon(passwordHidden ? QStringLiteral("eye.png")
                                                         : QStringLiteral("eye_off.png")));
    m_ui->pwToggleBtn->setIconSize(QSize(16, 16));
    m_ui->pwToggleBtn->setText(QString());
    m_ui->keyBrowseBtn->setIconSize(QSize(16, 16));
    m_ui->filesBtn->setIcon(themedIcon(QStringLiteral("folder.png")));
}

void UiBuilder::setupConnectionCombos()
{
    m_ui->baudCombo->setEditable(true);
    tuneEditableCombo(m_ui->baudCombo);
    m_ui->baudCombo->setValidator(new QIntValidator(1, 4000000, this));
    m_ui->baudCombo->addItems({
        QStringLiteral("1200"), QStringLiteral("2400"), QStringLiteral("4800"),
        QStringLiteral("9600"), QStringLiteral("19200"), QStringLiteral("38400"),
        QStringLiteral("57600"), QStringLiteral("115200"), QStringLiteral("230400"),
        QStringLiteral("460800"), QStringLiteral("921600"), QStringLiteral("1000000"),
        QStringLiteral("1500000"), QStringLiteral("2000000")});
    m_ui->baudCombo->setCurrentText(QStringLiteral("115200")); // 默认 115200 8N1

    for (int bits : {5, 6, 7, 8})
        m_ui->dataBitsCombo->addItem(QString::number(bits), bits);
    m_ui->dataBitsCombo->setCurrentText(QStringLiteral("8"));

    m_ui->parityCombo->addItem(tr("无"), QStringLiteral("none"));
    m_ui->parityCombo->addItem(tr("偶校验"), QStringLiteral("even"));
    m_ui->parityCombo->addItem(tr("奇校验"), QStringLiteral("odd"));

    m_ui->stopBitsCombo->addItem(QStringLiteral("1"), 1.0);
    m_ui->stopBitsCombo->addItem(QStringLiteral("1.5"), 1.5);
    m_ui->stopBitsCombo->addItem(QStringLiteral("2"), 2.0);

    m_ui->flowCombo->addItem(tr("无"), QStringLiteral("none"));
    m_ui->flowCombo->addItem(tr("硬件 RTS/CTS"), QStringLiteral("hardware"));
    m_ui->dtrCheck->setChecked(true); // 默认与 Windows 打开串口后的电平一致，不改变现有行为
    m_ui->rtsCheck->setChecked(true);
}

// 端口下拉只保留这几项。手输的端口留在编辑框里，不写进列表。
const QStringList &UiBuilder::fixedSshPorts()
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

void UiBuilder::pruneCustomPortItems(QComboBox *combo)
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

void UiBuilder::setupSshInputs()
{
    m_ui->sshPortCombo->setEditable(true);
    m_ui->sshPortCombo->setInsertPolicy(QComboBox::NoInsert);
    tuneEditableCombo(m_ui->sshPortCombo, 8);
    m_ui->sshPortCombo->setValidator(new QIntValidator(1, 65535, this));
    m_ui->sshPortCombo->addItems(fixedSshPorts());
    m_ui->sshPortCombo->setCurrentText(QStringLiteral("22"));
    // 样式表给可编辑下拉算的编辑区比箭头靠前一截，5 位数字会被裁掉，中间还空着。
    // 宽度按「左内边距 + 5 位数字 + 箭头」定死，再把编辑框拉到箭头边上。
    const QFontMetrics portFm(m_ui->sshPortCombo->font());
    m_ui->sshPortCombo->setFixedWidth(8 + portFm.horizontalAdvance(QStringLiteral("65535")) + 22);
    m_ui->sshPortCombo->installEventFilter(this);
    fitSshPortEdit();
    m_ui->sshPortCombo->setToolTip(tr("常用端口直接选。手输的端口只留在框里，不会加进下拉列表"));
    // 即便插入策略是 NoInsert，也再扫一遍，把任何多出来的项丢掉，编辑框文字保留。
    connect(m_ui->sshPortCombo->model(), &QAbstractItemModel::rowsInserted, this, [this] {
        QTimer::singleShot(0, this, [this] { pruneCustomPortItems(m_ui->sshPortCombo); });
    });

    for (int sec : {5, 10, 15, 30, 60})
        m_ui->timeoutCombo->addItem(QString::number(sec));
    m_ui->timeoutCombo->setCurrentText(QStringLiteral("10"));
}

void UiBuilder::fitSshPortEdit()
{
    QComboBox *combo = m_ui->sshPortCombo;
    QLineEdit *edit = combo->lineEdit();
    if (!edit || combo->width() < 8)
        return;
    constexpr int arrow = 16;
    constexpr int frame = 1;
    const QRect next(frame, frame, combo->width() - arrow - frame * 2,
                     combo->height() - frame * 2);
    if (next.width() > 0 && next.height() > 0 && edit->geometry() != next)
        edit->setGeometry(next);
}

bool UiBuilder::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_ui->sshPortCombo
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::StyleChange)) {
        // 下拉框自己的 resize 会把编辑框缩回去，等它排完再拉满到箭头前。
        QTimer::singleShot(0, this, [this] { fitSshPortEdit(); });
    }
    return QObject::eventFilter(watched, event);
}

void UiBuilder::setupTerminal()
{
    // 两个终端各占一页：各自的缓冲区、ANSI 状态、滚动位置都留在原地，
    // 切模式只是换一页显示，另一侧继续收发，内容不丢、连接不断。
    // 两页都是格子屏幕模型：串口设备的 top/vim 和 SSH 一样能还原。
    m_serialView = new ScreenView(m_window);
    // 默认终端档（对齐 SSH）；开关状态与配置在 applyCurrentProfile 里同步
    m_serialView->setLocalEcho(false);
    m_ui->serialTerminalPageLayout->addWidget(m_serialView);

    m_sshView = new ScreenView(m_window);
    m_ui->sshTerminalPageLayout->addWidget(m_sshView);

    // 最大化启动时终端网格内边距同步为 0（窗口化默认 6px）
    if (m_window->isMaximized()) {
        m_serialView->setPad(0);
        m_sshView->setPad(0);
    }
    // 面板外观由全局主题 QSS（QFrame#terminalPanel）定义

    // SSH 视图标题（OSC 0/2）跟随进窗口标题
    connect(m_sshView, &ScreenView::shellTitleChanged, this, [this](const QString &title) {
        m_shellTitle = title;
        if (m_session && m_session->mode() == SessionController::Mode::Ssh)
            updateModeUi();
    });
    // Ctrl+S(XOFF)可能挂起设备输出：状态栏亮提示，Ctrl+Q(XON)熄灭
    for (ScreenView *view : { m_serialView, m_sshView })
        connect(view, &ScreenView::xoffChanged, this,
                [this](bool paused) { m_xoffLabel->setVisible(paused); });
    // 选中词右键「高亮此词」：两个终端页共用同一套规则
    for (ScreenView *view : { m_serialView, m_sshView })
        connect(view, &ScreenView::highlightRequested, this, &UiBuilder::onHighlightRequested);

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
}

void UiBuilder::setupStatusBar()
{
    // 永久状态: 端口/波特率/连接状态
    m_statusLabel = new QLabel(tr("未连接"), m_window);
    m_ui->statusbar->addPermanentWidget(m_statusLabel, 1);

    // 常驻 MCP 状态（就绪/AI 在线/输入中/未启用）：标签由 McpMainHook 拥有，
    // 这里只按原顺序挂进状态栏
    m_ui->statusbar->addPermanentWidget(m_mcp->statusWidget(), 0);

    // XOFF 提示：发出 Ctrl+S(0x13)后设备 tty 可能按软件流控挂起输出，
    // 琥珀色常亮提示按 Ctrl+Q(0x11)恢复；发出 Ctrl+Q 或断开连接时熄灭。
    // 放 permanent 区不会被其他临时消息（错误提示等）挤掉。
    m_xoffLabel = new QLabel(tr("XOFF · Ctrl+Q 恢复"), m_window);
    m_xoffLabel->setStyleSheet(QStringLiteral("color: #D89B2C; font-weight: 600;"));
    m_xoffLabel->setToolTip(tr("按 Ctrl+S（XOFF）后设备输出可能被挂起：画面不更新，"
                               "但按键仍在发送。按 Ctrl+Q（XON）恢复。"));
    m_xoffLabel->hide();
    m_ui->statusbar->addPermanentWidget(m_xoffLabel, 0);

    // 「AI 锁定」提示：标签由 McpMainHook 拥有，按原顺序挂进状态栏
    m_ui->statusbar->addPermanentWidget(m_mcp->lockWidget(), 0);

    // 「内核日志静音」开关：常驻状态栏最右（连接状态旁），仅串口页显示
    //（printk 直写串口控制台的背景与对策见 KmsgMuteToggle 类注释）。
    // 创建时尚无会话，setSessionController 里补注 serial 并接行为。
    m_kmsgBtn = new KmsgMuteToggle(nullptr, m_window);
    m_ui->statusbar->addPermanentWidget(m_kmsgBtn, 0);
}

// ---------------------------------------------------------------- 刷新

void UiBuilder::updateModeUi()
{
    const bool serial = (m_session->mode() == SessionController::Mode::Serial);
    m_ui->modeBtn->setIcon(themedIcon(serial ? QStringLiteral("ssh.png")
                                             : QStringLiteral("serial.png")));
    m_ui->modeBtn->setText(serial ? tr("SSH") : tr("串口"));
    m_ui->modeBtn->setToolTip(serial ? tr("切换到 SSH 终端（F9；串口连接与内容保持不断开）")
                                     : tr("切换回串口终端（F9；SSH 连接与内容保持不断开）"));
    // 工具菜单项与模式按钮同一动作，文案同步（串口页显示「切换到 SSH」，反之亦然）
    m_hotkey->setSwitchModeText(serial ? tr("切换到 SSH") : tr("切换到串口"));
    // SFTP 走 SSH 通道：串口页不显示文件管理按钮（连同其左侧分隔线）
    m_ui->vline1->setVisible(!serial);
    m_ui->filesBtn->setVisible(!serial);
    // 内核日志静音开关只对串口控制台有意义（printk 刷不进 SSH 的 pty）
    if (m_kmsgBtn)
        m_kmsgBtn->setVisible(serial);
    if (!serial && !m_shellTitle.isEmpty())
        m_window->setWindowTitle(tr("Xterm — %1").arg(m_shellTitle));
    else
        m_window->setWindowTitle(serial ? tr("Xterm — 串口终端") : tr("Xterm — SSH 终端"));
}

void UiBuilder::updateConnectionUi()
{
    Transport *t = m_session->activeTransport();
    const Transport::State st = t->state();
    const bool open = (st == Transport::Connected);
    const bool busy = (st == Transport::Connecting || st == Transport::Disconnecting);
    const bool serial = (m_session->mode() == SessionController::Mode::Serial);

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
        tip = serial ? tr("断开串口") : tr("断开 SSH 连接");
    } else {
        text = tr("连接");
        tip = serial ? tr("连接串口") : tr("连接 SSH 主机");
    }
    m_ui->connectBtn->setText(text);
    m_ui->connectBtn->setToolTip(tip);
    // 工具菜单项触发的是同一个按钮，文案同步（断开时显示「连接」，连接时「断开」）
    m_hotkey->setConnectText(text);
    m_ui->connectBtn->setIcon(themedIcon(open ? QStringLiteral("disconnect.png")
                                              : QStringLiteral("connect.png")));
    m_ui->connectBtn->setEnabled(st != Transport::Disconnecting);
    m_ui->connectBtn->setProperty("connected", open);
    m_ui->connectBtn->style()->unpolish(m_ui->connectBtn);
    m_ui->connectBtn->style()->polish(m_ui->connectBtn);

    // 参数在连接期间锁定；另一侧（后台模式）的参数不在界面上，不参与禁用逻辑
    const bool idle = !open && !busy;
    m_ui->refreshPortBtn->setEnabled(serial && idle);
    for (QComboBox *combo : {m_ui->portCombo, m_ui->baudCombo, m_ui->dataBitsCombo,
                             m_ui->parityCombo, m_ui->stopBitsCombo, m_ui->flowCombo})
        combo->setEnabled(serial && idle);
    m_ui->dtrCheck->setEnabled(serial && idle);
    m_ui->rtsCheck->setEnabled(serial && idle);
    m_ui->hostEdit->setEnabled(!serial && idle);
    m_ui->userEdit->setEnabled(!serial && idle);
    m_ui->sshPortCombo->setEnabled(!serial && idle);
    m_ui->timeoutCombo->setEnabled(!serial && idle);
    m_ui->keyEdit->setEnabled(!serial && idle);
    m_ui->keyBrowseBtn->setEnabled(!serial && idle);

    // 文件管理按钮只在 SSH 页显示（见 updateModeUi），启用与否看 SSH 链路状态（含后台）
    m_ui->filesBtn->setEnabled(m_session->ssh()->state() == Transport::Connected);
}

void UiBuilder::onThemeToggled(bool dark)
{
    m_serialView->applyTheme(dark);
    m_sshView->applyTheme(dark);
    setupIcons();
    if (m_session)
        updateModeUi();
    if (m_session)
        updateConnectionUi();
    // 主题切换入口在自定义标题栏，图标显示"将要切换到"的主题
    if (m_titleBar) {
        m_titleBar->setThemeIcon(
            themedIcon(dark ? QStringLiteral("sun.png") : QStringLiteral("moon.png")),
            dark ? tr("切换到亮色主题") : tr("切换到深色主题"));
        m_titleBar->applyCaptionTheme(dark); // 系统按钮字形颜色跟主题
    }
    if (m_commandBar)
        m_commandBar->applyTheme();
    // 侧边按钮组里的 MCP 按钮也要跟主题换图标（构造时首次调用已创建）
    m_mcp->refreshIcon();
    fitSshPortEdit();
}

void UiBuilder::updatePersistentStatus()
{
    Transport *t = m_session->activeTransport();
    const bool serial = (m_session->mode() == SessionController::Mode::Serial);
    QString text;
    const char *tint = nullptr; // 状态色：亮/暗主题下都读得清的中性高饱和色
    switch (t->state()) {
    case Transport::Connected:
        text = serial ? tr("● %1 · 已连接").arg(m_session->connectionSummary())
                      : m_session->sshStatusText();
        tint = "#3EA854";
        break;
    case Transport::Connecting:
        text = tr("◐ %1 · 连接中…").arg(m_session->connectionSummary());
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
    Transport *other = serial ? static_cast<Transport *>(m_session->ssh())
                              : static_cast<Transport *>(m_session->serial());
    if (other->isOpen()) {
        const QString label = other->kind() == Transport::SshLink
            ? tr("SSH %1").arg(m_session->ssh()->summary())
            : tr("串口 %1").arg(m_session->serial()->portName());
        text += tr("  ·  后台 %1 已连接").arg(label);
    }

    m_statusLabel->setText(text);
    // 未连接时清空内联样式，交回主题 QSS 的中性色，不让它一直显眼
    m_statusLabel->setStyleSheet(tint
        ? QStringLiteral("color: %1; font-weight: 600;").arg(QLatin1String(tint))
        : QString());
}

void UiBuilder::applyWindowMargins(bool maximized)
{
    // 窗口化时外围留 6px 呼吸空间（配合面板圆角）；
    // 最大化时清零留白和终端网格内边距，让串口/SSH 终端贴满整个窗口。
    const int m = maximized ? 0 : 6;
    m_ui->mainLayout->setContentsMargins(m, m, m, m);
    m_ui->terminalLayout->setContentsMargins(m, m, m, m);
    m_serialView->setPad(m);
    m_sshView->setPad(m);
}

void UiBuilder::restoreTerminalFontSettings()
{
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
}

// ---------------------------------------------------------------- 关键词高亮

void UiBuilder::applyKeywords(const QList<KeywordRule> &rules)
{
    m_store->setKeywords(rules);
    m_serialView->setKeywords(rules);
    m_sshView->setKeywords(rules);
}

void UiBuilder::onHighlightRequested(const QString &word)
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
            m_ui->statusbar->showMessage(tr("已高亮「%1」").arg(word), 3000);
            return;
        }
    }
    KeywordRule rule;
    rule.text = word;
    rule.color = nextKeywordColor(rules.size());
    rules.append(rule);
    applyKeywords(rules);
    m_store->save();
    m_ui->statusbar->showMessage(tr("已高亮「%1」").arg(word), 3000);
}

void UiBuilder::openKeywordSettings()
{
    FramelessDialog dlg(m_window);
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
    const bool dark = m_theme->isDark();
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

void UiBuilder::openFontSettings()
{
    FramelessDialog dlg(m_window);
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
    const bool dark = m_theme->isDark();
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
