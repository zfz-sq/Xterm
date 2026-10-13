#include "sshsession.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {
// 保活：30s 一次、连续 3 次无响应即断开。嵌入式现场经常遇到网络半断开，
// 不做保活的话会话会一直"看起来连着"，直到用户敲键盘才发现。
constexpr int kKeepAliveIntervalSec = 30;
constexpr int kKeepAliveCountMax = 3;

QString firstExisting(const QStringList &candidates)
{
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return path;
    }
    return QString();
}
} // namespace

SshSession::SshSession(QObject *parent)
    : Transport(parent)
{
    connect(&m_pty, &ConPty::launched, this, &SshSession::onLaunched);
    connect(&m_pty, &ConPty::outputReceived, this, &Transport::received);
    connect(&m_pty, &ConPty::finished, this, &SshSession::onFinished);
}

SshSession::~SshSession()
{
    // ConPty 析构里会停线程；这里只需保证状态一致
    m_pty.stop();
    m_pty.waitForFinished(2000);
}

QString SshSession::sshExecutable()
{
    const QString found = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    if (!found.isEmpty())
        return found;
    // PATH 里没有时退回已知安装位置（OpenSSH for Windows 客户端按需安装）
    return firstExisting({
        QStringLiteral(R"(C:\Windows\System32\OpenSSH\ssh.exe)"),
        QStringLiteral(R"(C:\Windows\SysWOW64\OpenSSH\ssh.exe)"),
    });
}

QString SshSession::availabilityError()
{
    if (!ConPty::isSupported())
        return QObject::tr("当前系统不支持 ConPTY，SSH 终端需要 Windows 10 1809 及以上版本");
    if (sshExecutable().isEmpty())
        return QObject::tr("未找到系统 OpenSSH 客户端（ssh.exe）。请在"
                           "「设置 → 应用 → 可选功能」安装 OpenSSH 客户端");
    return QString();
}

void SshSession::setPtySize(int cols, int rows)
{
    if (cols < 20 || rows < 4)
        return;
    if (m_cols == cols && m_rows == rows)
        return;
    m_cols = cols;
    m_rows = rows;
    if (state() == Connected)
        m_pty.resize(cols, rows);
}

QString SshSession::open(const SshTarget &target)
{
    if (const QString err = availabilityError(); !err.isEmpty()) {
        emitError(err);
        return err;
    }
    if (target.host.trimmed().isEmpty()) {
        const QString err = tr("请先填写 SSH 主机地址");
        emitError(err);
        return err;
    }
    if (state() == Connecting || state() == Disconnecting) {
        const QString err = tr("上一个 SSH 会话还在处理中，请稍候");
        emitError(err);
        return err;
    }
    if (state() == Connected) {
        // 已有会话：先收掉，closed() 之后自动接着建立新的（不阻塞 UI）
        m_pendingReopen = target;
        m_hasPendingReopen = true;
        beginClose();
        return QString();
    }

    m_target = target;
    m_target.host = target.host.trimmed();
    m_userRequestedClose = false;
    m_pty.reset();

    setState(Connecting);
    const QStringList args = buildArguments();
    if (!m_pty.start(sshExecutable(), args, m_cols, m_rows)) {
        setState(Disconnected);
        const QString err = tr("SSH 会话仍在收尾，无法立即开始新连接");
        emitError(err);
        return err;
    }
    return QString();
}

void SshSession::close()
{
    m_hasPendingReopen = false; // 用户主动断开，不保留「断开后重连」的意图
    beginClose();
}

void SshSession::beginClose()
{
    if (state() == Disconnected) {
        m_pty.reset();
        return;
    }
    if (state() == Disconnecting)
        return;
    m_userRequestedClose = true;
    setState(Disconnecting);
    m_pty.stop(); // 收摊在 IO 线程完成，closed() 由 onFinished() 给出
}

QString SshSession::send(const QByteArray &data)
{
    if (state() != Connected)
        return tr("SSH 未连接，无法发送");
    m_pty.write(data); // 投递到 IO 线程，GUI 不等
    return QString();
}

QString SshSession::summary() const
{
    QString s;
    if (!m_target.user.isEmpty())
        s += m_target.user + QLatin1Char('@');
    s += m_target.host;
    if (m_target.port != 22)
        s += QLatin1Char(':') + QString::number(m_target.port);
    return s;
}

QStringList SshSession::buildArguments() const
{
    QStringList args;
    args << QStringLiteral("-tt");            // 强制分配 tty（本层已有真 tty，双保险）
    args << QStringLiteral("-p") << QString::number(qBound(1, m_target.port, 65535));
    args << QStringLiteral("-o")
         << QStringLiteral("ConnectTimeout=%1").arg(qBound(1, m_target.connectTimeoutSec, 120));
    args << QStringLiteral("-o")
         << QStringLiteral("ServerAliveInterval=%1").arg(kKeepAliveIntervalSec);
    args << QStringLiteral("-o")
         << QStringLiteral("ServerAliveCountMax=%1").arg(kKeepAliveCountMax);
    args << QStringLiteral("-o") << QStringLiteral("TCPKeepAlive=yes");
    if (!m_target.privateKey.trimmed().isEmpty())
        args << QStringLiteral("-i") << m_target.privateKey.trimmed();
    if (!m_target.extraArgs.trimmed().isEmpty())
        args << QProcess::splitCommand(m_target.extraArgs.trimmed());

    QString destination = m_target.host;
    if (!m_target.user.isEmpty())
        destination = m_target.user + QLatin1Char('@') + m_target.host;
    args << destination;
    return args;
}

void SshSession::onLaunched(const QString &errorText)
{
    if (state() != Connecting)
        return; // 已经被用户关闭，忽略迟到通知
    if (!errorText.isEmpty()) {
        setState(Disconnected);
        m_pty.reset();
        emitError(errorText);
        return;
    }
    emitOpened();
}

void SshSession::onFinished(int exitCode, bool terminatedByUs)
{
    const bool wasLive = (state() == Connected || state() == Disconnecting);
    m_pty.reset();
    if (!wasLive)
        return; // 启动就失败：错误已由 onLaunched() 上报

    const bool userRequested = m_userRequestedClose || terminatedByUs;
    emitClosed();
    emit sessionEnded(exitCode, userRequested);
    if (!userRequested && exitCode != 0)
        emitError(tr("SSH 会话异常结束（%1）").arg(exitHint(exitCode)));

    if (m_hasPendingReopen) {
        m_hasPendingReopen = false;
        const SshTarget next = m_pendingReopen;
        m_pendingReopen = SshTarget();
        open(next);
    }
}

QString SshSession::exitHint(int exitCode)
{
    switch (exitCode) {
    case 255:
        return tr("退出码 255，常见原因是主机不可达、认证失败或网络中断");
    case 124:
    case 143:
        return tr("连接被超时或远端终止");
    default:
        return tr("退出码 %1").arg(exitCode);
    }
}
