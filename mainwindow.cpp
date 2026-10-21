#include "mainwindow.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QListView>
#include <QPushButton>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QtMath>
#include <QVBoxLayout>

#include "agentbridge.h"
#include "commandbar.h"
#include "filebrowserdialog.h"
#include "hoststats.h"
#include "icons.h"
#include "screenview.h"
#include "serialsession.h"
#include "sshsession.h"
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

    // 永久状态: 端口/波特率/连接状态
    m_statusLabel = new QLabel(tr("未连接"), this);
    ui->statusbar->addPermanentWidget(m_statusLabel, 1);

    connectSignals();

    m_store->load();
    applyTheme();
    // 恢复上次保存的终端字号（全局，两页一致）
    if (const int savedPt = m_store->fontSize(); savedPt > 0) {
        m_serialView->setFontSize(savedPt);
        m_sshView->setFontSize(savedPt);
    }
    ui->advancedBar->hide(); // 高级参数默认收起
    refreshPorts();
    reloadProfiles(m_store->lastProfile());
    setMode(m_store->mode() == QStringLiteral("ssh") ? Mode::Ssh : Mode::Serial, false);

    m_agent = new AgentBridge(this);
    m_agent->start({
        [this] { return agentStatus(); },
        [this](int maxLines) { return agentRecentText(maxLines); },
        [this](const QString &text) { return agentSendLine(text); },
    });
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
    const bool passwordHidden = ui->passwordEdit->echoMode() == QLineEdit::Password;
    ui->pwToggleBtn->setIcon(themedIcon(passwordHidden ? QStringLiteral("eye.png")
                                                       : QStringLiteral("eye_off.png")));
    ui->pwToggleBtn->setIconSize(QSize(16, 16));
    ui->pwToggleBtn->setText(QString());
    ui->keyBrowseBtn->setIconSize(QSize(16, 16));
    ui->filesBtn->setIcon(themedIcon(QStringLiteral("folder.png")));
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
    m_serialView->setLocalEcho(true);
    ui->serialTerminalPageLayout->addWidget(m_serialView);

    m_sshView = new ScreenView(this);
    ui->sshTerminalPageLayout->addWidget(m_sshView);

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
    connect(m_serialView, &ScreenView::sendData, this, &MainWindow::sendRaw);
    connect(m_sshView, &ScreenView::sendData, this, &MainWindow::sendRaw);
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

    // 主题切换 / 模式切换 / 高级参数收展
    connect(ui->themeBtn, &QToolButton::clicked, this, [this] {
        m_store->setDarkTheme(!m_store->isDarkTheme());
        m_store->save();
        applyTheme();
    });
    connect(ui->mcpBtn, &QToolButton::clicked, this, &MainWindow::onMcpClicked);
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
    connect(m_serial, &SerialSession::received, m_serialView, &ScreenView::receiveData);
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
    ui->modeBtn->setToolTip(serial ? tr("切换到 SSH 终端（串口连接与内容保持不断开）")
                                   : tr("切换回串口终端（SSH 连接与内容保持不断开）"));
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

    // 文件管理器看 SSH 链路（含后台模式），与当前在哪一页无关
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
    // 图标显示"将要切换到"的主题: 深色时显示太阳(切亮色)，亮色时显示月亮(切深色)
    ui->themeBtn->setIcon(themedIcon(dark ? QStringLiteral("sun.png") : QStringLiteral("moon.png")));
    ui->themeBtn->setToolTip(dark ? tr("切换到亮色主题") : tr("切换到深色主题"));
    if (m_commandBar)
        m_commandBar->applyTheme();
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

    QDialog dialog(this);
    dialog.setWindowTitle(tr("主机密钥已变化"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(tr("主机 %1 的密钥指纹和上次记下的不一致。").arg(host), &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto addLine = [&](const QString &title, const QString &hex) {
        layout->addWidget(new QLabel(title, &dialog));
        auto *value = new QLabel(show(hex), &dialog);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(value);
    };
    addLine(tr("上次记录"), stored);
    addLine(tr("这次连接"), current);

    auto *hint = new QLabel(tr("确认这台机器重装过或换过密钥后，再更新记录并继续。如果这台机器不该换密钥，请拒绝。"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("更新并连接"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("拒绝"));
    buttons->button(QDialogButtonBox::Ok)->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Ok)->setDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.setMinimumWidth(460);
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
    QDialog dialog(this);
    dialog.setWindowTitle(tr("SSH 认证"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    QList<QLineEdit *> edits;
    for (const SshAuthPrompt &prompt : prompts) {
        auto *edit = new QLineEdit(&dialog);
        edit->setEchoMode(prompt.echo ? QLineEdit::Normal : QLineEdit::Password);
        form->addRow(prompt.text, edit);
        edits.append(edit);
    }
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.setMinimumWidth(420);

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

    QDialog dlg(this);
    dlg.setWindowTitle(tr("MCP 提示词"));
    dlg.resize(680, 520);
    auto *lay = new QVBoxLayout(&dlg);
    auto *hint = new QLabel(tr("把提示词交给已经接上 Xterm MCP 的 AI。服务程序只连接本机正在运行的这个窗口。"), &dlg);
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto *edit = new QPlainTextEdit(AgentBridge::instructions(), &dlg);
    edit->setReadOnly(true);
    lay->addWidget(edit, 1);
    lay->addWidget(new QLabel(tr("MCP 配置："), &dlg));
    auto *cfg = new QPlainTextEdit(config, &dlg);
    cfg->setReadOnly(true);
    cfg->setFixedHeight(140);
    lay->addWidget(cfg);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto *copy = buttons->addButton(tr("复制提示词"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [edit] {
        QGuiApplication::clipboard()->setText(edit->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
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
