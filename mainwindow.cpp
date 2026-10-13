#include "mainwindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include "commandbar.h"
#include "icons.h"
#include "serialsession.h"
#include "sshsession.h"
#include "terminalview.h"
#include "transport.h"
#include "ui_mainwindow.h"

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

void addHistoryEntry(QComboBox *combo, const QString &value)
{
    const QString text = value.trimmed();
    if (text.isEmpty())
        return;
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i).compare(text, Qt::CaseInsensitive) == 0) {
            combo->setCurrentIndex(i);
            return;
        }
    }
    combo->insertItem(0, text);
    while (combo->count() > 15)
        combo->removeItem(combo->count() - 1); // 只丢最旧的，当前项不动
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    m_serial = new SerialSession(this);
    m_ssh = new SshSession(this);
    m_store = new CommandStore(this);

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

    // 永久状态: 端口/波特率/连接状态
    m_statusLabel = new QLabel(tr("未连接"), this);
    ui->statusbar->addPermanentWidget(m_statusLabel, 1);

    connectSignals();

    m_store->load();
    applyTheme();
    ui->advancedBar->hide(); // 高级参数默认收起
    refreshPorts();
    reloadProfiles(m_store->lastProfile());
    setMode(m_store->mode() == QStringLiteral("ssh") ? Mode::Ssh : Mode::Serial, false);
}

MainWindow::~MainWindow()
{
    delete ui;
}

// ---------------------------------------------------------------- 初始化

void MainWindow::setupIcons()
{
    ui->refreshPortBtn->setIcon(themedIcon(QStringLiteral("refresh.png")));
    ui->settingsBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    ui->settingsBtn->setIconSize(QSize(16, 16));
    ui->sshSettingsBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    ui->sshSettingsBtn->setIconSize(QSize(16, 16));
    ui->keyBrowseBtn->setIconSize(QSize(16, 16));
    ui->themeBtn->setIcon(themedIcon(QStringLiteral("sun.png"))); // 实际图标由 applyTheme 设定
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

    m_loading = false;
}

void MainWindow::setupSshInputs()
{
    m_loading = true;

    tuneEditableCombo(ui->hostCombo, 12);
    tuneEditableCombo(ui->userCombo, 12);

    ui->sshPortCombo->setEditable(true);
    tuneEditableCombo(ui->sshPortCombo, 8);
    ui->sshPortCombo->setValidator(new QIntValidator(1, 65535, this));
    ui->sshPortCombo->addItems({QStringLiteral("22"), QStringLiteral("2222")});
    ui->sshPortCombo->setCurrentText(QStringLiteral("22"));

    for (int sec : {5, 10, 15, 30, 60})
        ui->timeoutCombo->addItem(QString::number(sec));
    ui->timeoutCombo->setCurrentText(QStringLiteral("10"));

    m_loading = false;
}

void MainWindow::setupTerminal()
{
    // 两个终端各占一页：各自的缓冲区、ANSI 状态、滚动位置都留在原地，
    // 切模式只是换一页显示，另一侧继续收发，内容不丢、连接不断。
    m_serialView = new TerminalView(this);
    ui->serialTerminalPageLayout->addWidget(m_serialView);

    m_sshView = new TerminalView(this);
    ui->sshTerminalPageLayout->addWidget(m_sshView);
    // SSH 的 tty 一定回显，本地回显关掉；Tab 交给远端 bash，不挂客户端补全引擎。
    m_sshView->setLocalEcho(false);

    // 面板外观由全局主题 QSS（QFrame#terminalPanel）定义
}

void MainWindow::setupCompletion()
{
    m_completion = new CompletionEngine; // 非 QObject，不接受 parent；生命周期由 m_completion 指针托管
    m_serialView->setCompletionEngine(m_completion);
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
    connect(ui->dataBitsCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->parityCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->stopBitsCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::onConnectionParamChanged);

    connect(ui->hostCombo, &QComboBox::currentTextChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->sshPortCombo, &QComboBox::currentTextChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->userCombo, &QComboBox::currentTextChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->timeoutCombo, &QComboBox::currentTextChanged, this,
            &MainWindow::onConnectionParamChanged);
    connect(ui->keyEdit, &QLineEdit::textChanged, this, [this] {
        if (!m_loading)
            syncCurrentProfile();
    });
    connect(ui->argsEdit, &QLineEdit::textChanged, this, [this] {
        if (!m_loading)
            syncCurrentProfile();
    });

    // 终端键盘直通 / 预设按钮
    connect(m_serialView, &TerminalView::sendData, this, &MainWindow::sendRaw);
    connect(m_sshView, &TerminalView::sendData, this, &MainWindow::sendRaw);
    connect(m_sshView, &TerminalView::terminalSizeChanged, this,
            [this](int cols, int rows) { m_ssh->setPtySize(cols, rows); });

    // 主题切换 / 模式切换 / 高级参数收展
    connect(ui->themeBtn, &QToolButton::clicked, this, [this] {
        m_store->setDarkTheme(!m_store->isDarkTheme());
        m_store->save();
        applyTheme();
    });
    connect(ui->modeBtn, &QToolButton::clicked, this, [this] {
        setMode(m_mode == Mode::Serial ? Mode::Ssh : Mode::Serial);
    });
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
        updateConnectionUi();
        updatePersistentStatus();
        if (m_mode == Mode::Serial)
            m_serialView->setFocus();
    });
    connect(m_serial, &SerialSession::closed, this, [this] {
        m_serialView->appendSystemMessage(tr("── 连接已断开 ──"));
        updateConnectionUi();
        updatePersistentStatus();
    });
    connect(m_serial, &SerialSession::errorOccurred, this, [this](const QString &msg) {
        ui->statusbar->showMessage(msg, 8000);
        m_serialView->appendSystemMessage(tr("── %1 ──").arg(msg));
    });
    connect(m_serial, &SerialSession::received, m_serialView, &TerminalView::receiveData);
    connect(m_serial, &SerialSession::stateChanged, this,
            [this](Transport::State) { updateConnectionUi(); updatePersistentStatus(); });

    // ---- SSH 链路 ----
    connect(m_ssh, &SshSession::opened, this, [this] {
        m_sshView->appendSystemMessage(tr("── 已连接 %1 ──").arg(m_ssh->summary()));
        updateConnectionUi();
        updatePersistentStatus();
        if (m_mode == Mode::Ssh)
            m_sshView->setFocus();
    });
    connect(m_ssh, &SshSession::closed, this, [this] {
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
    connect(m_ssh, &SshSession::received, m_sshView, &TerminalView::receiveData);
    connect(m_ssh, &SshSession::stateChanged, this,
            [this](Transport::State) { updateConnectionUi(); updatePersistentStatus(); });
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

        // 失败原因由 errorOccurred 统一上报（状态栏 + 终端），这里不再重复提示
        m_serial->open(port, baud, dataBits, parity, stopBits);
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
    if (m_ssh->open(target).isEmpty()) {
        pushSshHistory(target.host, target.user);
        m_sshView->appendSystemMessage(tr("── 正在连接 %1 ──").arg(m_ssh->summary()));
    }
    updateConnectionUi();
}

SshTarget MainWindow::sshTargetFromUi() const
{
    SshTarget t;
    t.host = ui->hostCombo->currentText().trimmed();
    t.port = ui->sshPortCombo->currentText().toInt();
    if (t.port <= 0)
        t.port = 22;
    t.user = ui->userCombo->currentText().trimmed();
    t.privateKey = ui->keyEdit->text().trimmed();
    t.connectTimeoutSec = ui->timeoutCombo->currentText().toInt();
    if (t.connectTimeoutSec <= 0)
        t.connectTimeoutSec = 10;
    t.extraArgs = ui->argsEdit->text().trimmed();
    return t;
}

void MainWindow::pushSshHistory(const QString &host, const QString &user)
{
    addHistoryEntry(ui->hostCombo, host);
    addHistoryEntry(ui->userCombo, user);
}

void MainWindow::onConnectionParamChanged()
{
    if (m_loading)
        return;
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

// ---------------------------------------------------------------- 模式切换

Transport *MainWindow::activeTransport() const
{
    return m_mode == Mode::Serial ? static_cast<Transport *>(m_serial)
                                  : static_cast<Transport *>(m_ssh);
}

TerminalView *MainWindow::activeTerminal() const
{
    return m_mode == Mode::Serial ? m_serialView : m_sshView;
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
        activeTerminal()->setFocus();
}

void MainWindow::updateModeUi()
{
    const bool serial = (m_mode == Mode::Serial);
    ui->modeBtn->setIcon(themedIcon(serial ? QStringLiteral("ssh.png")
                                           : QStringLiteral("serial.png")));
    ui->modeBtn->setText(serial ? tr("SSH") : tr("串口"));
    ui->modeBtn->setToolTip(serial ? tr("切换到 SSH 终端（串口连接与内容保持不断开）")
                                   : tr("切换回串口终端（SSH 连接与内容保持不断开）"));
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
                             ui->parityCombo, ui->stopBitsCombo})
        combo->setEnabled(serial && idle);
    for (QComboBox *combo : {ui->hostCombo, ui->sshPortCombo, ui->userCombo,
                             ui->timeoutCombo})
        combo->setEnabled(!serial && idle);
    ui->keyEdit->setEnabled(!serial && idle);
    ui->argsEdit->setEnabled(!serial && idle);
    ui->keyBrowseBtn->setEnabled(!serial && idle);
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
    // 图标显示"将要切换到"的主题: 深色时显示太阳(切亮色)，亮色时显示月亮(切深色)
    ui->themeBtn->setIcon(themedIcon(dark ? QStringLiteral("sun.png") : QStringLiteral("moon.png")));
    ui->themeBtn->setToolTip(dark ? tr("切换到亮色主题") : tr("切换到深色主题"));
    if (m_commandBar)
        m_commandBar->applyTheme();
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
        text = tr("● %1 · 已连接").arg(connectionSummary());
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
        activeTerminal()->appendSystemMessage(tr("── 未连接，命令未发送 ──"));
        ui->connectBtn->setFocus(); // 焦点移到"连接"按钮，引导用户
        return;
    }
    const QString err = t->send(bytes);
    if (!err.isEmpty())
        ui->statusbar->showMessage(err, 8000);
}

void MainWindow::sendLine(const QString &line)
{
    sendRaw(line.toUtf8() + QByteArrayLiteral("\n"));
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
    }
    // SSH 参数：已连接或正在连的时候不动，避免改动把用户从当前会话上带偏
    if (!m_ssh->isOpen() && m_ssh->state() == Transport::Disconnected) {
        addHistoryEntry(ui->hostCombo, p.ssh.host);
        ui->hostCombo->setCurrentText(p.ssh.host);
        addHistoryEntry(ui->userCombo, p.ssh.user);
        ui->userCombo->setCurrentText(p.ssh.user);
        ui->sshPortCombo->setCurrentText(QString::number(p.ssh.port));
        ui->timeoutCombo->setCurrentText(QString::number(p.ssh.connectTimeoutSec));
        ui->keyEdit->setText(p.ssh.privateKey);
        ui->argsEdit->setText(p.ssh.extraArgs);
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
    m_ssh->close(); // 收尾在 IO 线程进行；ConPty 析构时会等它结束
    event->accept();
}
