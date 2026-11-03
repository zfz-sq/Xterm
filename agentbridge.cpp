#include "agentbridge.h"

#include "commandstore.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

namespace {

constexpr int kMaxLine = 1024 * 1024;

QString agentFilePath()
{
    const QString cfg = CommandStore::configPath();
    return QDir(QFileInfo(cfg).absolutePath()).filePath(QStringLiteral("agent.json"));
}

int clampTimeout(int ms)
{
    if (ms <= 0)
        return 3000;
    return qBound(100, ms, 30000);
}

int clampLines(int n)
{
    if (n <= 0)
        return 120;
    return qBound(1, n, 400);
}

} // namespace

QString AgentBridge::instructions()
{
    return QStringLiteral(
        "你通过 MCP 工具操作用户电脑上已经打开的 Xterm，从而使用它当前连着的设备。"
        "不要自己打开串口，不要另起 ssh，不要去点这个软件的窗口。\n"
        "\n"
        "先调用 status。若未连接，告诉用户在 Xterm 里点「连接」，不要猜测端口、波特率或主机。\n"
        "\n"
        "用 send_line 发送一行。不要自行追加 \\n 或 \\r，软件会按当前链路补上："
        "串口（含 STM32 USB CDC）是 \\n，SSH 是 \\r。\n"
        "\n"
        "发送后用 wait，或给 send_line 带上 wait，等到预期输出或超时。"
        "不要用固定睡眠判断成功。需要看屏幕已有内容时用 read。"
        "wait 自带回显免疫：等待词出现在命令文本里时，会等它再次出现（真实输出）才命中，"
        "无需用引号拆分等技巧绕开回显。\n"
        "\n"
        "send_line 是打字机式逐字发送，用户在界面上能看到输入过程。"
        "打字期间用户终端的普通输入被锁定，用户按 Ctrl+C 会主动打断 AI 输入："
        "剩余字符不会发出，你会收到 interrupted by user 错误——"
        "这不是设备或网络故障，也不代表命令失败，是用户要亲自操作设备，"
        "应停止当前动作，等待用户指示后再继续。\n"
        "\n"
        "不要用这些工具操作 top、vim 等全屏程序。\n"
        "\n"
        "这些调用会直接作用到连接的设备上，与在终端里敲命令相同。");
}

AgentBridge::AgentBridge(QObject *parent, const QString &pipeName)
    : QObject(parent)
    , m_pipeName(pipeName)
{
    m_poll = new QTimer(this);
    m_poll->setInterval(40);
    connect(m_poll, &QTimer::timeout, this, &AgentBridge::pollWaiters);

    // 打字机：间隔在 startTyping 里按请求设置
    m_typeTimer = new QTimer(this);
    connect(m_typeTimer, &QTimer::timeout, this, &AgentBridge::typingTick);
}

AgentBridge::~AgentBridge()
{
    m_waiters.clear();
    if (m_server)
        m_server->close();
    removeAgentFile();
}

void AgentBridge::start(Hooks hooks, const QString &agentFile)
{
    m_hooks = std::move(hooks);
    if (m_server)
        return;

    m_agentFile = agentFile.isEmpty() ? agentFilePath() : agentFile;

    // 已有活实例在服务管道时（用户开了第二个 Xterm），本实例不抢管道、
    // 不覆盖 agent.json——否则次实例一旦退出会留下死令牌，所有 MCP 客户端
    // 全部「令牌不匹配」。注意 Windows 允许同名管道多实例，listen() 成功
    // 不代表没有别人在监听，必须主动探测。
    if (probeExistingAgent())
        return;

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    // Windows 上进程退出会释放管道。这里不 removeServer，避免把另一个正在运行的 Xterm 挤掉。
    if (!m_server->listen(m_pipeName)) {
        m_server->deleteLater();
        m_server = nullptr;
        return;
    }
    connect(m_server, &QLocalServer::newConnection, this, &AgentBridge::onNewConnection);
    m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    writeAgentFile();
}

bool AgentBridge::probeExistingAgent() const
{
    QFile f(m_agentFile);
    if (!f.open(QIODevice::ReadOnly))
        return false; // 没有文件: 本实例是第一个
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QString pipe = obj.value(QStringLiteral("pipe")).toString();
    const QString token = obj.value(QStringLiteral("token")).toString();
    if (pipe.isEmpty() || token.isEmpty())
        return false;

    // 用文件里的令牌连管道发一个 status：能连通且令牌通过 = 有活实例在前台服务。
    QLocalSocket sock;
    sock.connectToServer(pipe);
    if (!sock.waitForConnected(300))
        return false; // 无人监听: 文件是崩溃残留，本实例安全接管（写入时覆盖）
    QJsonObject req;
    req.insert(QStringLiteral("id"), 1);
    req.insert(QStringLiteral("token"), token);
    req.insert(QStringLiteral("method"), QStringLiteral("status"));
    req.insert(QStringLiteral("params"), QJsonObject());
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    sock.write("\n");
    if (!sock.waitForBytesWritten(300) || !sock.waitForReadyRead(300))
        return false; // 有连接但对端无响应（半死状态）: 视为无实例
    const QJsonObject resp = QJsonDocument::fromJson(sock.readLine()).object();
    return resp.value(QStringLiteral("ok")).toBool(); // 令牌不匹配 = 前实例已死
}

void AgentBridge::writeAgentFile()
{
    QDir().mkpath(QFileInfo(m_agentFile).absolutePath());
    QJsonObject obj;
    obj.insert(QStringLiteral("pipe"), m_pipeName);
    obj.insert(QStringLiteral("token"), m_token);
    // pid 供客户端判断实例存活性（异常退出残留的文件可识别）
    obj.insert(QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid()));
    obj.insert(QStringLiteral("instructions"), instructions());
    QSaveFile file(m_agentFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.commit();
}

void AgentBridge::removeAgentFile()
{
    if (m_agentFile.isEmpty() || m_token.isEmpty())
        return;
    QFile file(m_agentFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (obj.value(QStringLiteral("token")).toString() != m_token)
        return;
    QFile::remove(m_agentFile);
}

void AgentBridge::onNewConnection()
{
    while (QLocalSocket *sock = m_server->nextPendingConnection()) {
        m_buf.insert(sock, {});
        connect(sock, &QLocalSocket::readyRead, this, [this, sock] { onSocketReady(sock); });
        connect(sock, &QLocalSocket::disconnected, this, [this, sock] { onSocketClosed(sock); });
    }
}

void AgentBridge::onSocketClosed(QLocalSocket *sock)
{
    m_buf.remove(sock);
    for (int i = m_waiters.size() - 1; i >= 0; --i) {
        if (m_waiters.at(i).sock == sock)
            m_waiters.removeAt(i);
    }
    if (m_waiters.isEmpty())
        m_poll->stop();
    // 打字机对端断开：中止打字（不回错误——连接已经没了）
    if (m_typing.active && m_typing.sock == sock)
        finishTyping(true);
    sock->deleteLater();
}

void AgentBridge::onSocketReady(QLocalSocket *sock)
{
    QByteArray &buf = m_buf[sock];
    buf += sock->readAll();
    if (buf.size() > kMaxLine) {
        sock->disconnectFromServer();
        return;
    }
    int nl = -1;
    while ((nl = buf.indexOf('\n')) >= 0) {
        const QByteArray line = buf.left(nl);
        buf.remove(0, nl + 1);
        handleLine(sock, line);
    }
}

void AgentBridge::reply(QLocalSocket *sock, const QJsonValue &id, bool ok, const QJsonObject &result,
                        const QString &error)
{
    if (!sock || sock->state() != QLocalSocket::ConnectedState)
        return;
    QJsonObject obj;
    if (!id.isUndefined())
        obj.insert(QStringLiteral("id"), id);
    obj.insert(QStringLiteral("ok"), ok);
    if (ok)
        obj.insert(QStringLiteral("result"), result);
    else
        obj.insert(QStringLiteral("error"), error);
    sock->write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    sock->write("\n");
}

void AgentBridge::handleLine(QLocalSocket *sock, const QByteArray &line)
{
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        reply(sock, {}, false, {}, QStringLiteral("无法解析请求"));
        return;
    }
    const QJsonObject req = doc.object();
    const QJsonValue id = req.value(QStringLiteral("id"));
    if (req.value(QStringLiteral("token")).toString() != m_token) {
        reply(sock, id, false, {}, QStringLiteral("令牌不匹配"));
        return;
    }
    const QString method = req.value(QStringLiteral("method")).toString();
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    emit agentActive(); // 令牌校验通过: 合法的 AI 客户端正在访问

    if (method == QLatin1String("status")) {
        QJsonObject result = m_hooks.status ? m_hooks.status() : QJsonObject();
        reply(sock, id, true, result, {});
        return;
    }
    if (method == QLatin1String("read")) {
        const int lines = clampLines(params.value(QStringLiteral("max_lines")).toInt());
        QJsonObject result;
        result.insert(QStringLiteral("text"), m_hooks.recentText ? m_hooks.recentText(lines) : QString());
        reply(sock, id, true, result, {});
        return;
    }
    if (method == QLatin1String("send_control")) {
        // 控制字符直达当前链路（如 ctrl+c 中断长输出命令），不补行尾、不记行历史
        const QString key = params.value(QStringLiteral("key")).toString().toLower();
        char byte = 0;
        if (key == QLatin1String("esc"))
            byte = 0x1b;
        else if (key == QLatin1String("tab"))
            byte = 0x09;
        else if (key.startsWith(QLatin1String("ctrl+")) && key.size() == 6) {
            const QChar c = key.at(5);
            if (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                byte = char(c.toLatin1() - 'a' + 1);
        }
        if (byte == 0) {
            reply(sock, id, false, {},
                  QStringLiteral("未知控制键（支持 ctrl+a..z / esc / tab）"));
            return;
        }
        const QString err = m_hooks.sendBytes
                                ? m_hooks.sendBytes(QByteArray(1, byte))
                                : QStringLiteral("未连接");
        if (!err.isEmpty()) {
            reply(sock, id, false, {}, err);
            return;
        }
        QJsonObject result;
        result.insert(QStringLiteral("sent"), key);
        reply(sock, id, true, result, {});
        return;
    }
    if (method == QLatin1String("send_line")) {
        const QString text = params.value(QStringLiteral("text")).toString();
        // 本地命令（clear -a）保持整行直达，不进打字机
        if (text.trimmed() == QLatin1String("clear -a")) {
            const QString err = m_hooks.sendLine ? m_hooks.sendLine(text) : QStringLiteral("未连接");
            if (!err.isEmpty()) {
                reply(sock, id, false, {}, err);
                return;
            }
            QJsonObject result;
            result.insert(QStringLiteral("text"),
                          m_hooks.recentText ? m_hooks.recentText(clampLines(0)) : QString());
            reply(sock, id, true, result, {});
            return;
        }
        // 打字机逐字发送：用户看得见输入过程，且随时可被自己的操作打断
        const QString needle = params.value(QStringLiteral("wait")).toString();
        const int interval = qBound(5, params.value(QStringLiteral("interval_ms")).toInt(20), 120);
        startTyping(sock, id, text, needle,
                    clampTimeout(params.value(QStringLiteral("timeout_ms")).toInt()), interval);
        return;
    }
    if (method == QLatin1String("wait")) {
        const QString needle = params.value(QStringLiteral("contains")).toString();
        if (needle.isEmpty()) {
            reply(sock, id, false, {}, QStringLiteral("wait 需要 contains"));
            return;
        }
        const QString after = m_hooks.recentText ? m_hooks.recentText(clampLines(0)) : QString();
        Waiter w;
        w.sock = sock;
        w.id = id;
        w.needle = needle;
        w.beforeCount = after.count(needle);
        w.timeoutMs = clampTimeout(params.value(QStringLiteral("timeout_ms")).toInt());
        w.deadlineMs = QDateTime::currentMSecsSinceEpoch() + w.timeoutMs;
        m_waiters.append(w);
        if (!m_poll->isActive())
            m_poll->start();
        return;
    }
    reply(sock, id, false, {}, QStringLiteral("未知方法"));
}

void AgentBridge::pollWaiters()
{
    if (m_waiters.isEmpty()) {
        m_poll->stop();
        return;
    }
    const QString now = m_hooks.recentText ? m_hooks.recentText(120) : QString();
    const qint64 clock = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_waiters.size() - 1; i >= 0; --i) {
        const Waiter w = m_waiters.at(i);
        const int count = now.count(w.needle);
        // 命中条件: 新出现次数达到 minNew（普通为 1，回显免疫场景为 2）
        const bool hit = count - w.beforeCount >= w.minNew;
        const bool timedOut = clock >= w.deadlineMs;
        if (!hit && !timedOut)
            continue;
        m_waiters.removeAt(i);
        QJsonObject result;
        result.insert(QStringLiteral("text"), now);
        result.insert(QStringLiteral("matched"), hit);
        result.insert(QStringLiteral("timed_out"), !hit);
        reply(w.sock, w.id, true, result, {});
    }
    if (m_waiters.isEmpty())
        m_poll->stop();
}

void AgentBridge::startTyping(QLocalSocket *sock, const QJsonValue &id, const QString &text,
                              const QString &waitNeedle, int timeoutMs, int intervalMs)
{
    // 上一次还没打完（同一管道上请求理应串行，防御一下）：当作被打断收掉
    if (m_typing.active)
        finishTyping(true);

    QByteArray bytes = text.toUtf8();
    while (bytes.endsWith('\n') || bytes.endsWith('\r'))
        bytes.chop(1);
    // 行尾按当前链路补：串口 \n，SSH \r（hook 缺省 \n）
    bytes.append(m_hooks.lineEnding ? m_hooks.lineEnding() : '\n');

    m_typing.active = true;
    m_typing.sock = sock;
    m_typing.id = id;
    m_typing.rest = bytes;
    m_typing.command = QString::fromUtf8(bytes.chopped(1));
    m_typing.waitNeedle = waitNeedle;
    // 回显免疫: needle 若是所发命令的子串，命令回显本身就会让它 +1；
    // 此时要求它再出现一次（真实输出）才算命中。
    m_typing.minNew = waitNeedle.isEmpty() ? 1 : (text.contains(waitNeedle) ? 2 : 1);
    m_typing.timeoutMs = timeoutMs;
    emit agentActive(); // 打字期间角标点亮
    emit agentTypingChanged(true, m_typing.command);
    m_typeTimer->setInterval(intervalMs);
    m_typeTimer->start();
}

void AgentBridge::typingTick()
{
    if (!m_typing.active)
        return;
    // 长命令自动加速：短命令接近真人手速，几百字符的脚本也不会拖太久
    const int left = m_typing.rest.size();
    const int chunk = left > 160 ? 6 : (left > 48 ? 3 : 1);
    const QByteArray part = m_typing.rest.left(chunk);
    const QString err = m_hooks.sendBytes ? m_hooks.sendBytes(part) : QStringLiteral("未连接");
    if (!err.isEmpty()) {
        QLocalSocket *sock = m_typing.sock;
        const QJsonValue id = m_typing.id;
        finishTyping(true);
        reply(sock, id, false, {}, err);
        return;
    }
    m_typing.rest.remove(0, part.size());
    if (!m_typing.rest.isEmpty())
        return; // 还没打完，下个 tick 继续

    // 整行（含行尾）已发出：记输入历史、收尾，然后按有无 wait 应答或挂等待
    QLocalSocket *sock = m_typing.sock;
    const QJsonValue id = m_typing.id;
    const QString needle = m_typing.waitNeedle;
    const int minNew = m_typing.minNew;
    const int timeoutMs = m_typing.timeoutMs;
    const QString command = m_typing.command;
    finishTyping(false);
    if (m_hooks.lineSent)
        m_hooks.lineSent(command);

    const QString after = m_hooks.recentText ? m_hooks.recentText(clampLines(0)) : QString();
    if (needle.isEmpty()) {
        QJsonObject result;
        result.insert(QStringLiteral("text"), after);
        reply(sock, id, true, result, {});
        return;
    }
    Waiter w;
    w.sock = sock;
    w.id = id;
    w.needle = needle;
    w.beforeCount = after.count(needle);
    w.minNew = minNew;
    w.timeoutMs = timeoutMs;
    w.deadlineMs = QDateTime::currentMSecsSinceEpoch() + w.timeoutMs;
    m_waiters.append(w);
    if (!m_poll->isActive())
        m_poll->start();
}

void AgentBridge::finishTyping(bool interrupted)
{
    if (!m_typing.active)
        return;
    m_typing.active = false;
    m_typeTimer->stop();
    m_typing.sock = nullptr;
    m_typing.rest.clear();
    emit agentTypingChanged(false, QString());
    Q_UNUSED(interrupted);
}

bool AgentBridge::interruptTyping()
{
    if (!m_typing.active)
        return false;
    QLocalSocket *sock = m_typing.sock;
    const QJsonValue id = m_typing.id;
    const QString command = m_typing.command;
    const int left = m_typing.rest.size();
    finishTyping(true);
    reply(sock, id, false, {},
          QStringLiteral("interrupted by user: 用户按 Ctrl+C 主动打断了 AI 输入"
                         "（%1 剩 %2 字符未发送）。这不是设备或网络故障，"
                         "也不代表命令执行失败——用户现在要亲自操作终端。"
                         "请停止当前动作，等待用户指示后再继续。")
              .arg(command.left(40), QString::number(left)));
    return true;
}
