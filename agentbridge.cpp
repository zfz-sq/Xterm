#include "agentbridge.h"

#include "commandstore.h"

#include <QDateTime>
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
        "不要用固定睡眠判断成功。需要看屏幕已有内容时用 read。\n"
        "\n"
        "不要用这些工具操作 top、vim 等全屏程序。\n"
        "\n"
        "这些调用会直接作用到连接的设备上，与在终端里敲命令相同。");
}

AgentBridge::AgentBridge(QObject *parent)
    : QObject(parent)
{
    m_pipeName = QStringLiteral("XtermAgent");
    m_poll = new QTimer(this);
    m_poll->setInterval(40);
    connect(m_poll, &QTimer::timeout, this, &AgentBridge::pollWaiters);
}

AgentBridge::~AgentBridge()
{
    m_waiters.clear();
    if (m_server)
        m_server->close();
    removeAgentFile();
}

void AgentBridge::start(Hooks hooks)
{
    m_hooks = std::move(hooks);
    if (m_server)
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
    m_agentFile = agentFilePath();
    writeAgentFile();
}

void AgentBridge::writeAgentFile()
{
    QDir().mkpath(QFileInfo(m_agentFile).absolutePath());
    QJsonObject obj;
    obj.insert(QStringLiteral("pipe"), m_pipeName);
    obj.insert(QStringLiteral("token"), m_token);
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
    if (method == QLatin1String("send_line")) {
        const QString text = params.value(QStringLiteral("text")).toString();
        const QString err = m_hooks.sendLine ? m_hooks.sendLine(text) : QStringLiteral("未连接");
        if (!err.isEmpty()) {
            reply(sock, id, false, {}, err);
            return;
        }
        const QString needle = params.value(QStringLiteral("wait")).toString();
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
        w.timeoutMs = clampTimeout(params.value(QStringLiteral("timeout_ms")).toInt());
        w.deadlineMs = QDateTime::currentMSecsSinceEpoch() + w.timeoutMs;
        m_waiters.append(w);
        if (!m_poll->isActive())
            m_poll->start();
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
        const bool hit = count > w.beforeCount;
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
