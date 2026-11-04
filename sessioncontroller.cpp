#include "sessioncontroller.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "commandbar.h"
#include "completionengine.h"
#include "filebrowserdialog.h"
#include "framelessdialog.h"
#include "hoststats.h"
#include "mcpmainhook.h"
#include "screenview.h"
#include "serialsession.h"
#include "sshsession.h"
#include "transport.h"
#include "uibuilder.h"
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
} // namespace

SessionController::SessionController(QWidget *window, Ui::MainWindow *ui,
                                     CommandStore *store, McpMainHook *mcp,
                                     QObject *parent)
    : QObject(parent), m_ui(ui), m_window(window), m_store(store), m_mcp(mcp)
{
    m_serial = new SerialSession(this);
    m_ssh = new SshSession(this);
    // 主机状态采样走共享 SSH 会话的 stats 通道（任务 8）；展示缓存下沉
    // HostStats，状态栏文案由 sshStatusText() 取。
    m_hostStats = new HostStats(m_ssh, this);
    m_completion = new CompletionEngine; // 非 QObject，不接受 parent；生命周期由指针托管

    // 配置落盘防抖：浏览下拉/连续输入每一步都会触发保存信号，同步写盘
    // （含 Defender 扫描）会把 UI 拍出一顿一顿的卡顿；内存立即更新，
    // 写盘合并到用户停下来之后一次完成。
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { m_store->save(); });

    wireSessions();
}

SessionController::~SessionController()
{
    // CompletionEngine 不是 QObject；SFTP 双栏窗口若还开着也要随会话一起走
    //（destroyed 回调会把指针清空，delete 自身安全）
    delete m_fileBrowser;
    delete m_completion;
}

void SessionController::wireSessions()
{
    // ---- 串口链路 ----
    connect(m_serial, &SerialSession::opened, this, [this] {
        m_serialView->appendSystemMessage(tr("── 已连接 %1 ──").arg(connectionSummary()));
        // printk 回到设备默认级别：静音开关状态重置由 UiBuilder 连本信号完成
        emit connectionStateChanged();
        if (m_mode == Mode::Serial)
            focusActiveTerminal();
    });
    connect(m_serial, &SerialSession::closed, this, [this] {
        m_serialView->appendSystemMessage(tr("── 连接已断开 ──"));
        emit connectionStateChanged(); // XOFF 提示熄灭由 UiBuilder 连本信号完成
    });
    connect(m_serial, &SerialSession::errorOccurred, this, [this](const QString &msg) {
        m_ui->statusbar->showMessage(msg, 8000);
        m_serialView->appendSystemMessage(tr("── %1 ──").arg(msg));
    });
    connect(m_serial, &SerialSession::stateChanged, this,
            [this](Transport::State) { emit connectionStateChanged(); });

    // ---- SSH 链路 ----
    connect(m_ssh, &SshSession::opened, this, [this] {
        m_sshView->appendSystemMessage(tr("── 已连接 %1 ──").arg(m_ssh->summary()));
        m_hostStats->clearCache();
        m_hostStats->resetDelta();
        m_ssh->startStats();
        emit connectionStateChanged();
        if (m_mode == Mode::Ssh)
            focusActiveTerminal();
    });
    connect(m_ssh, &SshSession::closed, this, [this] {
        m_ssh->stopStats();
        m_hostStats->clearCache();
        emit connectionStateChanged(); // XOFF 提示熄灭由 UiBuilder 连本信号完成
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
        m_ui->statusbar->showMessage(msg, 8000);
        m_sshView->appendSystemMessage(tr("── %1 ──").arg(msg));
    });
    connect(m_ssh, &SshSession::hostKeyDecisionRequired, this,
            &SessionController::promptHostKeyDecision);
    connect(m_ssh, &SshSession::unknownHostKeyRecorded, this,
            [this](const QString &host) {
                m_sshView->appendSystemMessage(
                    tr("── 已记录主机 %1 的新密钥。首次信任无法防止中间人攻击 ──")
                        .arg(host));
            });
    connect(m_ssh, &SshSession::authAnswersRequired, this,
            &SessionController::promptAuthAnswers);
    connect(m_ssh, &SshSession::stateChanged, this,
            [this](Transport::State) { emit connectionStateChanged(); });

    // stats 新帧只刷状态栏读数（原逻辑：仅 SSH 模式刷新，避免每秒白刷连接按钮）
    connect(m_hostStats, &HostStats::statsUpdated, this, [this] {
        if (m_mode == Mode::Ssh)
            emit statsChanged();
    });
}

void SessionController::attachTerminal(ScreenView *serialView, ScreenView *sshView,
                                       CommandBar *commandBar)
{
    m_serialView = serialView;
    m_sshView = sshView;
    m_commandBar = commandBar;

    // 链路字节流直连各自的终端页（两条链路独立存活，互不串台）
    connect(m_serial, &SerialSession::received, m_serialView, &ScreenView::receiveData);
    connect(m_ssh, &SshSession::received, m_sshView, &ScreenView::receiveData);

    // 调试档才挂补全；终端档没有本地行编辑，Tab 交给设备（onSerialModeToggled 同步）
    m_serialView->setCompletionEngine(m_ui->serialModeBtn->isChecked() ? nullptr
                                                                       : m_completion);
    // 把当前配置里的命令名注入补全源
    const Profile p = m_store->profile(m_currentProfile);
    QStringList names;
    for (const CommandItem &c : p.commands)
        names << c.text;
    m_completion->setUserCommands(names);
}

// ---------------------------------------------------------------- 连接

void SessionController::toggleConnect()
{
    if (m_mode == Mode::Serial) {
        if (m_serial->isOpen() || m_serial->state() == Transport::Connecting) {
            m_serial->close();
            return;
        }
        const QString port = selectedPortName();
        if (port.isEmpty()) {
            m_ui->statusbar->showMessage(tr("请先选择串口端口"), 5000);
            return;
        }
        const qint32 baud = m_ui->baudCombo->currentText().toInt();
        const auto dataBits = static_cast<QSerialPort::DataBits>(
            m_ui->dataBitsCombo->currentData().toInt());
        const QString parityStr = m_ui->parityCombo->currentData().toString();
        const QSerialPort::Parity parity =
            (parityStr == QStringLiteral("even")) ? QSerialPort::EvenParity
            : (parityStr == QStringLiteral("odd"))  ? QSerialPort::OddParity
                                                    : QSerialPort::NoParity;
        const double sb = m_ui->stopBitsCombo->currentData().toDouble();
        const QSerialPort::StopBits stopBits = (sb > 1.9)   ? QSerialPort::TwoStop
                                               : (sb > 1.4) ? QSerialPort::OneAndHalfStop
                                                            : QSerialPort::OneStop;
        const QSerialPort::FlowControl flow =
            (m_ui->flowCombo->currentData().toString() == QStringLiteral("hardware"))
                ? QSerialPort::HardwareControl
                : QSerialPort::NoFlowControl;

        // 失败原因由 errorOccurred 统一上报（状态栏 + 终端），这里不再重复提示
        m_serial->open(port, baud, dataBits, parity, stopBits, flow,
                       m_ui->dtrCheck->isChecked(), m_ui->rtsCheck->isChecked());
        emit connectionStateChanged();
        return;
    }

    // SSH
    if (m_ssh->isOpen() || m_ssh->state() == Transport::Connecting
        || m_ssh->state() == Transport::Disconnecting) {
        m_ssh->close();
        emit connectionStateChanged();
        return;
    }
    if (const QString err = SshSession::availabilityError(); !err.isEmpty()) {
        m_ui->statusbar->showMessage(err, 8000);
        m_sshView->appendSystemMessage(tr("── %1 ──").arg(err));
        return;
    }
    const SshTarget target = sshTargetFromUi();
    if (m_ssh->open(target).isEmpty())
        m_sshView->appendSystemMessage(tr("── 正在连接 %1 ──").arg(m_ssh->summary()));
    emit connectionStateChanged();
}

void SessionController::refreshPorts()
{
    const QString keep = selectedPortName();
    m_loading = true;
    m_ui->portCombo->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : ports) {
        const QString port = info.portName();
        m_ui->portCombo->addItem(formatPortLabel(port, info.description()), port);
        const QString tip = QStringLiteral("%1 %2 %3")
                                .arg(info.description(), info.manufacturer(),
                                     info.serialNumber())
                                .simplified();
        m_ui->portCombo->setItemData(m_ui->portCombo->count() - 1, tip, Qt::ToolTipRole);
    }
    const int keepIndex = keep.isEmpty() ? -1 : m_ui->portCombo->findData(keep);
    if (keepIndex >= 0)
        m_ui->portCombo->setCurrentIndex(keepIndex);
    else if (m_ui->portCombo->count() > 0)
        m_ui->portCombo->setCurrentIndex(0);
    // 收起时固定 150；下拉列表按最长一项撑开，避免长名称被裁掉后无法辨认
    const QFontMetrics fm(m_ui->portCombo->font());
    int popupWidth = m_ui->portCombo->width();
    for (int i = 0; i < m_ui->portCombo->count(); ++i)
        popupWidth = qMax(popupWidth, fm.horizontalAdvance(m_ui->portCombo->itemText(i)) + 28);
    m_ui->portCombo->view()->setMinimumWidth(popupWidth);
    m_loading = false;

    if (m_ui->portCombo->count() == 0)
        m_ui->statusbar->showMessage(tr("未检测到可用串口设备"), 5000);
}

QString SessionController::selectedPortName() const
{
    const QString port = m_ui->portCombo->currentData().toString();
    return port.isEmpty() ? m_ui->portCombo->currentText() : port;
}

SshTarget SessionController::sshTargetFromUi() const
{
    SshTarget t;
    t.host = m_ui->hostEdit->text().trimmed();
    t.port = m_ui->sshPortCombo->currentText().toInt();
    if (t.port <= 0)
        t.port = 22;
    t.user = m_ui->userEdit->text().trimmed();
    t.password = m_ui->passwordEdit->text();
    t.privateKey = m_ui->keyEdit->text().trimmed();
    t.connectTimeoutSec = m_ui->timeoutCombo->currentText().toInt();
    if (t.connectTimeoutSec <= 0)
        t.connectTimeoutSec = 10;
    return t;
}

void SessionController::onConnectionParamChanged()
{
    if (m_loading)
        return;
    syncCurrentProfile();
}

// 串口终端模式切换。调试：为 STM32 CDC 这类无回显设备做的本地代画
// 代编辑（含远端画屏让路）；终端：对齐 SSH——设备回显，按键逐个裸发，
// vi 的单键提示、全屏重画都按 SSH 的路数走。只改本地行为，不动链路参数，
// 连接中也能切，所以不参与连接控件的锁定。
void SessionController::onSerialModeToggled(bool pure)
{
    m_ui->serialModeBtn->setText(pure ? tr("终端") : tr("调试"));
    m_serialView->setLocalEcho(!pure);
    // Tab 补全是行编辑的周边（读屏上输入行、发差量），纯终端没有本地
    // 行编辑，交给设备自己处理 Tab。
    m_serialView->setCompletionEngine(pure ? nullptr : m_completion);
    if (!m_loading)
        syncCurrentProfile();
}

void SessionController::browsePrivateKey()
{
    const QString dir = QDir::homePath() + QStringLiteral("/.ssh");
    const QString start = QDir(dir).exists() ? dir : QDir::homePath();
    // 只取路径，不自动聚焦任何输入框：键盘/输入法保持原样
    const QString file = QFileDialog::getOpenFileName(
        m_window, tr("选择 SSH 私钥"), start,
        QStringLiteral("所有文件 (*);;私钥 (*.pem *.key id_* id_ed25519 id_rsa)"));
    if (!file.isEmpty())
        m_ui->keyEdit->setText(QDir::toNativeSeparators(file));
}

void SessionController::openFileBrowser()
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
    m_fileBrowser = new FileBrowserDialog(m_ssh, m_window);
    m_fileBrowser->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_fileBrowser, &QObject::destroyed, this, [this] { m_fileBrowser = nullptr; });
    m_fileBrowser->show();
}

void SessionController::onSftpRequested()
{
    if (m_ssh->state() == Transport::Connected)
        openFileBrowser();
    else
        appendActiveNote(tr("── SSH 未连接，无法打开 SFTP 文件管理 ──"));
}

void SessionController::openLink(const QUrl &url)
{
    if (!url.isValid())
        return;
    if (url.scheme() != QStringLiteral("file")) {
        // http(s)/mailto 等：交系统默认程序（浏览器/邮件）
        QDesktopServices::openUrl(url);
        return;
    }
    const QString path = url.toLocalFile();
    if (QFile::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    // 本地不存在：SSH 在线则当远端路径取回（fetch 完成后自动用关联程序打开）
    if (m_ssh->state() == Transport::Connected) {
        openFileBrowser();
        m_fileBrowser->fetchRemoteFile(path);
    } else {
        appendActiveNote(tr("── 链接指向的文件本地不存在，SSH 未连接无法取回：%1 ──")
                             .arg(path));
    }
}

// ---------------------------------------------------------------- 模式

Transport *SessionController::activeTransport() const
{
    return m_mode == Mode::Serial ? static_cast<Transport *>(m_serial)
                                  : static_cast<Transport *>(m_ssh);
}

ScreenView *SessionController::activeView() const
{
    return m_mode == Mode::Serial ? m_serialView : m_sshView;
}

void SessionController::focusActiveTerminal()
{
    activeView()->setFocus();
}

void SessionController::appendActiveNote(const QString &text)
{
    activeView()->appendSystemMessage(text);
}

void SessionController::setMode(Mode mode, bool persist)
{
    const bool changed = (mode != m_mode);
    m_mode = mode;

    // 只切显示：两条链路、两份终端内容都原样留在后台
    m_ui->modeStack->setCurrentIndex(mode == Mode::Serial ? 0 : 1);
    m_ui->advancedStack->setCurrentIndex(mode == Mode::Serial ? 0 : 1);
    m_ui->terminalStack->setCurrentIndex(mode == Mode::Serial ? 0 : 1);

    if (changed && persist) {
        m_store->setMode(mode == Mode::Serial ? QStringLiteral("serial")
                                              : QStringLiteral("ssh"));
        m_store->save();
    }
    if (changed && m_ui->advancedBar->isVisible()) {
        // 高级栏开着时切模式，跟随显示当前模式的参数，不改变收起/展开状态
        m_ui->advancedBar->setVisible(true);
    } else if (changed) {
        m_ui->advancedBar->hide();
    }

    // 模式/连接/状态栏级联刷新由 UiBuilder 连本信号完成
    emit modeChanged();
    if (changed)
        focusActiveTerminal();
}

void SessionController::toggleMode()
{
    setMode(m_mode == Mode::Serial ? Mode::Ssh : Mode::Serial);
}

QString SessionController::connectionSummary() const
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
    }(m_ui->parityCombo->currentData().toString());
    const double sb = m_ui->stopBitsCombo->currentData().toDouble();
    return QStringLiteral("%1 @ %2 %3%4%5")
        .arg(selectedPortName(), m_ui->baudCombo->currentText(),
             m_ui->dataBitsCombo->currentText(), parityChar,
             sb == 1.5 ? QStringLiteral("1.5") : QString::number(sb));
}

QString SessionController::sshStatusText() const
{
    return m_hostStats->statusText();
}

// ---------------------------------------------------------------- 认证弹窗

bool SessionController::confirmHostKeyChange(const QString &host, const QString &stored,
                                             const QString &current)
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

    FramelessDialog dialog(m_window);
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

void SessionController::promptHostKeyDecision(const SshHostKeyChange &change)
{
    const bool accept = confirmHostKeyChange(
        change.host, change.oldFingerprint, change.newFingerprint);
    m_ssh->answerHostKey(change.requestId, accept);
}

void SessionController::promptAuthAnswers(SshRequestId requestId,
                                          const QList<SshAuthPrompt> &prompts)
{
    FramelessDialog dialog(m_window);
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

// ---------------------------------------------------------------- MCP 适配器

QJsonObject SessionController::agentStatus() const
{
    Transport *t = activeTransport();
    const ScreenView *view = activeView();
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

QString SessionController::agentRecentText(int maxLines) const
{
    const ScreenView *view = activeView();
    return view ? view->recentText(maxLines) : QString();
}

int SessionController::agentRecentLineCount() const
{
    const ScreenView *view = activeView();
    return view ? view->recentLineCount() : 0;
}

QString SessionController::agentRecentTextFrom(int fromLine, int maxLines) const
{
    const ScreenView *view = activeView();
    return view ? view->recentTextFrom(fromLine, maxLines) : QString();
}

QString SessionController::agentSendBytes(const QByteArray &bytes)
{
    Transport *t = activeTransport();
    if (!t || t->state() != Transport::Connected)
        return tr("未连接");
    return t->send(bytes);
}

QString SessionController::agentSendLine(const QString &text)
{
    if (text.trimmed() == QLatin1String("clear -a")) {
        ScreenView *view = activeView();
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
    ScreenView *view = activeView();
    if (view) {
        view->rememberSentLine(QString::fromUtf8(bytes.chopped(1)));
        view->showOutbound(bytes);
    }
    return t->send(bytes);
}

char SessionController::agentLineEnding() const
{
    return m_mode == Mode::Serial ? '\n' : '\r';
}

// ---------------------------------------------------------------- 发送

void SessionController::sendRaw(const QByteArray &bytes)
{
    Transport *t = activeTransport();
    if (t->state() != Transport::Connected) {
        // 明确提示而不是静默无反应
        const bool ssh = (m_mode == Mode::Ssh);
        const QString why = t->state() == Transport::Connecting
            ? (ssh ? tr("SSH 正在连接中") : tr("串口正在连接中"))
            : (ssh ? tr("SSH 未连接") : tr("串口未连接"));
        m_ui->statusbar->showMessage(tr("%1，无法发送 —— 请先点“连接”").arg(why), 5000);
        appendActiveNote(tr("── 未连接，命令未发送 ──"));
        m_ui->connectBtn->setFocus(); // 焦点移到"连接"按钮，引导用户
        return;
    }
    const QString err = t->send(bytes);
    if (!err.isEmpty())
        m_ui->statusbar->showMessage(err, 8000);
}

void SessionController::sendLine(const QString &line)
{
    // 命令栏按钮也是人为输入：AI 打字期间同样锁定（唯一出口是终端里的 Ctrl+C）
    if (m_mcp && m_mcp->isTyping()) {
        m_ui->statusbar->showMessage(tr("AI 正在操作，输入已锁定 —— 按 Ctrl+C 打断"), 3000);
        return;
    }
    if (line.trimmed() == QLatin1String("clear -a")) {
        ScreenView *view = activeView();
        if (view) {
            view->rememberSentLine(line.trimmed());
            view->clearLocalDisplay();
        }
        return;
    }
    ScreenView *view = activeView();
    if (view)
        view->rememberSentLine(line);
    sendRaw(line.toUtf8() + QByteArrayLiteral("\n"));
}

// ---------------------------------------------------------------- 配置

Profile SessionController::currentProfileFromUi() const
{
    Profile p;
    p.name = m_currentProfile;
    p.connection.port = selectedPortName();
    p.connection.baud = m_ui->baudCombo->currentText().toInt();
    p.connection.dataBits = m_ui->dataBitsCombo->currentData().toInt();
    p.connection.parity = m_ui->parityCombo->currentData().toString();
    p.connection.stopBits = m_ui->stopBitsCombo->currentData().toDouble();
    p.connection.flowControl = m_ui->flowCombo->currentData().toString();
    p.connection.dtr = m_ui->dtrCheck->isChecked();
    p.connection.rts = m_ui->rtsCheck->isChecked();
    p.connection.pureTerminal = m_ui->serialModeBtn->isChecked();
    p.ssh = sshTargetFromUi();
    p.commands = m_commandBar->commands();
    return p;
}

void SessionController::syncCurrentProfile()
{
    if (m_currentProfile.isEmpty() || !m_store->hasProfile(m_currentProfile))
        return;
    m_store->upsertProfile(currentProfileFromUi()); // 内存立即生效（切换配置/退出都不丢）
    scheduleProfileSave();                          // 写盘防抖合并
}

void SessionController::scheduleProfileSave()
{
    // 400ms 内再有变化就重新计时，最终只写一次盘。退出路径 shutdownForClose()
    // 里有显式 save()，挂起中的定时器不会造成数据丢失。
    m_saveTimer->start(400);
}

void SessionController::reloadProfiles(const QString &selectName)
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

void SessionController::applyCurrentProfile()
{
    if (m_currentProfile.isEmpty())
        return;
    const Profile p = m_store->profile(m_currentProfile);

    m_loading = true;
    if (!m_serial->isOpen() && !p.connection.port.isEmpty()) {
        const int i = m_ui->portCombo->findData(p.connection.port);
        if (i >= 0)
            m_ui->portCombo->setCurrentIndex(i);
    }
    if (!m_serial->isOpen()) {
        m_ui->baudCombo->setCurrentText(QString::number(p.connection.baud));
        for (int i = 0; i < m_ui->dataBitsCombo->count(); ++i)
            if (m_ui->dataBitsCombo->itemData(i).toInt() == p.connection.dataBits)
                m_ui->dataBitsCombo->setCurrentIndex(i);
        for (int i = 0; i < m_ui->parityCombo->count(); ++i)
            if (m_ui->parityCombo->itemData(i).toString() == p.connection.parity)
                m_ui->parityCombo->setCurrentIndex(i);
        for (int i = 0; i < m_ui->stopBitsCombo->count(); ++i)
            if (qFuzzyCompare(m_ui->stopBitsCombo->itemData(i).toDouble(), p.connection.stopBits))
                m_ui->stopBitsCombo->setCurrentIndex(i);
        for (int i = 0; i < m_ui->flowCombo->count(); ++i)
            if (m_ui->flowCombo->itemData(i).toString() == p.connection.flowControl)
                m_ui->flowCombo->setCurrentIndex(i);
        m_ui->dtrCheck->setChecked(p.connection.dtr);
        m_ui->rtsCheck->setChecked(p.connection.rts);
    }
    // 终端模式只影响本地行为，不动串口链路，连接中也按配置生效
    m_ui->serialModeBtn->setChecked(p.connection.pureTerminal);
    // SSH 参数：已连接或正在连的时候不动，避免改动把用户从当前会话上带偏
    if (!m_ssh->isOpen() && m_ssh->state() == Transport::Disconnected) {
        m_ui->hostEdit->setText(p.ssh.host);
        m_ui->userEdit->setText(p.ssh.user);
        m_ui->sshPortCombo->setEditText(QString::number(p.ssh.port > 0 ? p.ssh.port : 22));
        UiBuilder::pruneCustomPortItems(m_ui->sshPortCombo);
        m_ui->timeoutCombo->setCurrentText(QString::number(p.ssh.connectTimeoutSec));
        m_ui->passwordEdit->setText(p.ssh.password);
        m_ui->keyEdit->setText(p.ssh.privateKey);
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

void SessionController::onCommandsChanged()
{
    // 同步给补全引擎（取每条命令的第一个 token 作为命令名）
    QStringList names;
    for (const CommandItem &c : m_commandBar->commands())
        names << c.text;
    m_completion->setUserCommands(names);
    syncCurrentProfile();
}

// ---------------------------------------------------------------- 关闭

void SessionController::shutdownForClose()
{
    syncCurrentProfile();
    m_store->setMode(m_mode == Mode::Serial ? QStringLiteral("serial")
                                            : QStringLiteral("ssh"));
    m_store->save();
    m_serial->close();
    m_ssh->close();
}
