#include "sshsession.h"

#include <QFileInfo>
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
    connect(&m_pty, &ConPty::outputReceived, this, &SshSession::onOutputReceived);
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
    m_promptTail.clear();
    m_promptHintSent = false;
    m_hostKeyHintSent = false;
    m_autofillLeft = m_target.password.isEmpty() ? 0 : 3;
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

QString SshSession::sanitizedArg(const QString &raw)
{
    // 粘贴带进来的换行/回车/制表符等控制字符会拼出非法命令行，
    // CreateProcessW 直接拒绝（Win32 错误 87 = ERROR_INVALID_PARAMETER）。
    QString out;
    out.reserve(raw.size());
    for (const QChar c : raw)
        if (c.unicode() >= 0x20 && c.unicode() != 0x7f)
            out += c;
    return out.trimmed();
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
    // Windows 上 ssh.exe 会先试 GSSAPI/Kerberos，域环境里连接能卡几十秒。
    // 交互流量走低延迟，避免和批量传输挤在同一档。
    args << QStringLiteral("-o") << QStringLiteral("GSSAPIAuthentication=no");
    args << QStringLiteral("-o")
         << QStringLiteral("PreferredAuthentications=publickey,keyboard-interactive,password");
    args << QStringLiteral("-o") << QStringLiteral("IPQoS=lowdelay");
    const QString key = sanitizedArg(m_target.privateKey);
    if (!key.isEmpty()) {
        // 指定了密钥就别再把 agent 里每把钥匙都试一遍，否则很容易
        // “Too many authentication failures” 然后连真正的钥匙都排不上。
        args << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes");
        args << QStringLiteral("-i") << key;
    }

    QString destination = m_target.host;
    if (!m_target.user.isEmpty())
        destination = m_target.user + QLatin1Char('@') + m_target.host;
    args << destination;
    return args;
}

void SshSession::onOutputReceived(const QByteArray &chunk)
{
    emit received(chunk);
    if (m_autofillLeft <= 0 && m_promptHintSent)
        return; // 既不代填、提醒也发过了，不必再扫每一块输出

    // 只看窗口尾部：提示可能恰好被拆在两个 chunk 交界处。
    // 先剥 CSI 序列（颜色/光标移动会插进 "password" 中间），再压掉空白做匹配。
    m_promptTail += chunk;
    if (m_promptTail.size() > 512)
        m_promptTail.remove(0, m_promptTail.size() - 512);
    QByteArray t;
    t.reserve(m_promptTail.size());
    for (int i = 0; i < m_promptTail.size(); ++i) {
        const char c = m_promptTail.at(i);
        if (c == '\x1b') {
            if (i + 1 < m_promptTail.size() && m_promptTail.at(i + 1) == '[') {
                ++i;
                while (i + 1 < m_promptTail.size()
                       && !(m_promptTail.at(i + 1) >= '@' && m_promptTail.at(i + 1) <= '~'))
                    ++i;
            }
            continue;
        }
        if (!QChar::isSpace(char16_t(uchar(c))))
            t += c;
    }
    const QString squ = QString::fromUtf8(t).toLower();
    const bool password = squ.contains(QLatin1String("password:"));
    const bool hostKey = squ.contains(QLatin1String("yes/no"));
    if (!password && !hostKey)
        return;
    m_promptTail.clear();
    if (hostKey && !password) {
        // 不代答 yes。自动接受未知主机等于跳过指纹核对。
        if (!m_hostKeyHintSent) {
            m_hostKeyHintSent = true;
            emit hostKeyPromptDetected();
        }
        return;
    }
    if (m_autofillLeft <= 0) {
        // 没有可代填的凭据（没配密码，或 3 次机会已用完）：必须把「轮到你了，
        // 而且密码输入不回显」说出来 —— 否则用户对着一个怎么敲都不显示的提示行，
        // 只会判定程序卡死。
        m_promptHintSent = true;
        emit passwordPromptDetected();
        return;
    }
    --m_autofillLeft; // 最多代填 3 次，防错误密码死循环
    m_pty.write(m_target.password.toUtf8() + "\r");
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
    // 视口尺寸可能在连接建立前就已更新（先 resize 后连接的竞态里，
    // setPtySize 只存了值没能下发）。连接一就绪就补发一次；尺寸没变时
    // ConPTY 侧有守卫，不会引发多余重绘。
    m_pty.resize(m_cols, m_rows);
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
