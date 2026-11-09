#include "agentbridge.h"

#include "auditlog.h"

#include "commandstore.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
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
        "不要自己另起终端软件，不要去点这个软件的窗口。\n"
        "\n"
        "工具带 xterm.* 命名空间；tools.list 可随时拿到全部工具与参数说明。\n"
        "\n"
        "先调用 xterm.session.status。若未连接，且 tools.list 里有 xterm.session."
        "openSsh/openSerial，可用 xterm.session.list 看两条链路状态后发起连接"
        "（端口、波特率、主机等参数由用户明确给出才填，缺省用界面已配置的；"
        "凭据只认界面配置）；没有这些工具时告诉用户在 Xterm 里点「连接」，"
        "不要猜测端口、波特率或主机。\n"
        "\n"
        "xterm.session.switch 在串口/SSH 两条链路间切换（不断开另一条），"
        "xterm.session.close 断开。open 幂等：已连接时不会重复发起，"
        "换参数连接请先 close 再 open。\n"
        "\n"
        "用 xterm.transport.sendLine 发送一行。不要自行追加 \\n 或 \\r，软件会按"
        "当前链路补上：串口（含 STM32 USB CDC）是 \\n，SSH 是 \\r。\n"
        "\n"
        "发送后用 xterm.screen.wait，或给 xterm.transport.sendLine 带上 wait，"
        "等到预期输出或超时。不要用固定睡眠判断成功。"
        "需要看屏幕已有内容时用 xterm.screen.read。"
        "wait 自带回显免疫：等待词出现在命令文本里时，会等它再次出现"
        "（真实输出）才命中，无需用引号拆分等技巧绕开回显。\n"
        "\n"
        "xterm.transport.sendLine 是打字机式逐字发送，用户在界面上能看到输入过程"
        "（sendTypewriter 是它的正式命名入口，行为一致）。"
        "打字期间用户终端的普通输入被锁定，用户按 Ctrl+C 会主动打断 AI 输入："
        "剩余字符不会发出，你会收到 interrupted by user 错误——"
        "这不是设备或网络故障，也不代表命令失败，是用户要亲自操作设备，"
        "应停止当前动作，等待用户指示后再继续。"
        "AI 自己要中断远端长输出命令时用 xterm.transport.interrupt"
        "（停打字机 + 发 Ctrl+C）。\n"
        "\n"
        "不要用这些工具操作 top、vim 等全屏程序。\n"
        "\n"
        "若 tools.list 里有 xterm.fs.*，可读写远端文件：仅限会话根目录及子路径"
        "（沙箱；符号链接按真实路径复核，指向根外会被拒绝）。"
        "同一时刻只处理一个文件操作，收到「进行中」就等它结束再重试。"
        "写类默认关闭，被拒时说明用户未开启 allow_write。\n"
        "\n"
        "若 tools.list 里有 xterm.host.*，可读远端主机状态：cpu/mem 来自采样通道"
        "（SSH 连接后约 6 秒就绪，ok=false 时稍后重试），disk/uptime/top 按需"
        "在远端跑只读命令。仅 SSH 会话可用。\n"
        "\n"
        "读屏优先 xterm.screen.snapshot（结构化：文本/颜色/超链接段，适合看"
        "带布局的输出）；xterm.screen.find 在滚动历史里查（可正则）；"
        "xterm.screen.waitForPattern 按正则等新输出，发命令后建议 min_new=2"
        "防回显。\n"
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
    delete m_audit;
}

void AgentBridge::start(Hooks hooks, const QString &agentFile, bool allowWrite)
{
    m_hooks = std::move(hooks);
    m_allowWrite = allowWrite;
    if (m_server)
        return;

    // M2.a：基于能力字段注册内置工具，再合入调用方的额外工具（同名覆盖）
    registerBuiltinTools();
    for (auto it = m_hooks.tools.constBegin(); it != m_hooks.tools.constEnd(); ++it) {
        m_tools.insert(it.key(), it.value());
        m_toolMeta.insert(it.key(), m_hooks.toolMeta.value(it.key()));
    }

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
    // 审计（M2.e）：agent.json 同目录 logs/ 下 jsonl，滚动轮转。
    // 测试注入临时 agentFile 时审计也落临时目录，不碰真实配置
    m_audit = new AuditLog(
        QDir(QFileInfo(m_agentFile).absolutePath()).filePath(QStringLiteral("logs/agent-audit.jsonl")));
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

    // 用文件里的令牌连管道发一个工具调用：能连通且令牌通过 = 有活实例在
    // 前台服务。先试新命名空间名，旧实例（M2.a 之前）不认识会拒绝，再回退
    // 旧短名——升级窗口内新旧两版双开时不能误判旧实例已死而抢管道。
    QLocalSocket sock;
    sock.connectToServer(pipe);
    if (!sock.waitForConnected(300))
        return false; // 无人监听: 文件是崩溃残留，本实例安全接管（写入时覆盖）
    auto probeWith = [&sock, &token](const QString &method) {
        QJsonObject req;
        req.insert(QStringLiteral("id"), 1);
        req.insert(QStringLiteral("token"), token);
        req.insert(QStringLiteral("method"), method);
        req.insert(QStringLiteral("params"), QJsonObject());
        sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
        sock.write("\n");
        if (!sock.waitForBytesWritten(300) || !sock.waitForReadyRead(300))
            return false; // 有连接但对端无响应（半死状态）: 视为无实例
        const QJsonObject resp = QJsonDocument::fromJson(sock.readLine()).object();
        return resp.value(QStringLiteral("ok")).toBool(); // 令牌不匹配/未知工具都算不通过
    };
    return probeWith(QStringLiteral("xterm.session.status"))
           || probeWith(QStringLiteral("status"));
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
    // 写类 FS 工具开关（M2.b）：客户端可读；真实判定在 FsBridge，同源同步
    obj.insert(QStringLiteral("allow_write"), m_allowWrite);
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
    // 对端断开还没应答的调用：不再结算（审计只在真正应答时记一条完整事件），
    // 按连接前缀清掉，防止同地址复用时串到新连接
    const QString prefix = QString::number(quintptr(sock)) + QLatin1Char('#');
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it.key().startsWith(prefix))
            it = m_pending.erase(it);
        else
            ++it;
    }
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
    // 统一应答出口：先结算审计与 agentToolCall（即使对端已断开，
    // 调用本身也发生了，照样记一条完整事件），再尝试写回
    settlePending(sock, id, ok, error);
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
    const QString method = req.value(QStringLiteral("method")).toString();
    if (req.value(QStringLiteral("token")).toString() != m_token) {
        // 审计（M2.e）：非法令牌也是可观测事件（谁在试探管道）
        if (m_audit) {
            QJsonObject e;
            e.insert(QStringLiteral("event"), QStringLiteral("auth_failed"));
            e.insert(QStringLiteral("method"), method);
            m_audit->append(e);
        }
        reply(sock, id, false, {}, QStringLiteral("令牌不匹配"));
        return;
    }
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();
    emit agentActive(); // 令牌校验通过: 合法的 AI 客户端正在访问

    // 限流（M2.e 令牌桶）：超性请求立即拒绝——防 AI 循环刷 read 类工具
    // 卡死 GUI 线程。错误文案引导稍等重试（挂起中的 wait 不占令牌，
    // 轮询是内部定时器不走这条路径）
    if (!takeRateToken()) {
        if (m_audit) {
            QJsonObject e;
            e.insert(QStringLiteral("event"), QStringLiteral("rate_limited"));
            e.insert(QStringLiteral("method"), method);
            m_audit->append(e);
        }
        reply(sock, id, false, {},
              QStringLiteral("rate limited：请求过快，请等待约 1 秒后重试"));
        return;
    }
    // 审计上下文入表：reply() 统一出口结算（同步与挂起类工具都覆盖）
    {
        PendingCall p;
        p.method = method;
        p.params = params;
        p.startMs = QDateTime::currentMSecsSinceEpoch();
        m_pending.insert(pendingKey(sock, id), p);
    }

    // 协议级：工具自发现（不带 xterm. 前缀）
    if (method == QLatin1String("tools.list")) {
        QJsonObject result;
        result.insert(QStringLiteral("tools"), toolCatalog());
        reply(sock, id, true, result, {});
        return;
    }
    // 工具路由：bridge 只做分发与兜底，工具本体都在注册表里（M2.a）
    const auto it = m_tools.constFind(method);
    if (it == m_tools.constEnd()) {
        reply(sock, id, false, {},
              QStringLiteral("未知工具: %1（tools.list 可查全部工具）").arg(method));
        return;
    }
    ToolCall call;
    call.params = params;
    call.sock = sock;
    call.id = id;
    call.reply = [this, sock, id](bool ok, const QJsonObject &data, const QString &error) {
        reply(sock, id, ok, data, error);
    };
    const ToolResult r = it.value()(call);
    if (r.deferred)
        return; // 挂起类工具（打字机 / wait / 异步文件与主机查询）接管了应答
    reply(sock, id, r.ok, r.data, r.error);
}

// 内置五件：基于 Hooks 能力字段组装。方法名带 xterm.* 命名空间（M2.1），
// 旧短名（status/read/wait/send_line/send_control）直接废弃，不保留别名。
void AgentBridge::registerBuiltinTools()
{
    m_tools.clear();
    m_toolMeta.clear();

    auto meta = [this](const char *name, const QString &desc, QJsonObject params) {
        const QString full = QString::fromLatin1(name);
        QJsonObject m;
        m.insert(QStringLiteral("name"), full);
        m.insert(QStringLiteral("description"), desc);
        m.insert(QStringLiteral("params"), params);
        m_toolMeta.insert(full, m);
    };

    m_tools.insert(QStringLiteral("xterm.session.status"),
                   [this](const ToolCall &) -> ToolResult {
                       return ToolResult{true, false,
                                         m_hooks.status ? m_hooks.status() : QJsonObject(),
                                         QString()};
                   });
    meta("xterm.session.status", QStringLiteral("当前链路状态（连接/类型/端口），先调它确认已连接"),
         QJsonObject());

    m_tools.insert(QStringLiteral("xterm.screen.read"),
                   [this](const ToolCall &call) -> ToolResult {
                       const int lines = clampLines(call.params.value(QStringLiteral("max_lines")).toInt());
                       QJsonObject data;
                       data.insert(QStringLiteral("text"),
                                   m_hooks.recentText ? m_hooks.recentText(lines) : QString());
                       return ToolResult{true, false, data, QString()};
                   });
    {
        QJsonObject params;
        QJsonObject maxLines;
        maxLines.insert(QStringLiteral("type"), QStringLiteral("integer"));
        maxLines.insert(QStringLiteral("description"),
                        QStringLiteral("返回尾部行数（含当前屏），默认 120，上限 400"));
        params.insert(QStringLiteral("max_lines"), maxLines);
        meta("xterm.screen.read", QStringLiteral("读屏幕尾部文本（滚动历史 + 当前屏）"), params);
    }

    m_tools.insert(QStringLiteral("xterm.transport.sendControl"),
                   [this](const ToolCall &call) -> ToolResult {
                       // 控制字符直达当前链路（如 ctrl+c 中断长输出命令），
                       // 不补行尾、不记行历史
                       const QString key = call.params.value(QStringLiteral("key")).toString().toLower();
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
                       if (byte == 0)
                           return ToolResult{false, false, {},
                                             QStringLiteral("未知控制键（支持 ctrl+a..z / esc / tab）")};
                       const QString err = m_hooks.sendBytes
                                               ? m_hooks.sendBytes(QByteArray(1, byte))
                                               : QStringLiteral("未连接");
                       if (!err.isEmpty())
                           return ToolResult{false, false, {}, err};
                       QJsonObject data;
                       data.insert(QStringLiteral("sent"), key);
                       return ToolResult{true, false, data, QString()};
                   });
    {
        QJsonObject params;
        QJsonObject key;
        key.insert(QStringLiteral("type"), QStringLiteral("string"));
        key.insert(QStringLiteral("description"),
                   QStringLiteral("ctrl+a..z / esc / tab，如 ctrl+c 中断长输出"));
        params.insert(QStringLiteral("key"), key);
        meta("xterm.transport.sendControl", QStringLiteral("发送单个控制字符/键"), params);
    }

    // B 组正名（M2.2）：API 级 Ctrl+C——若打字机在打先停（该次调用会收到
    // interrupted 错误），再把 \x03 直达当前链路。区别于用户侧只停打字机
    // 不发字节的 interruptTyping()：那是用户要亲自接管，这里是 AI 自己要
    // 中断远端命令
    m_tools.insert(QStringLiteral("xterm.transport.interrupt"),
                   [this](const ToolCall &) -> ToolResult {
                       const bool wasTyping = interruptTyping();
                       const QString err = m_hooks.sendBytes
                                               ? m_hooks.sendBytes(QByteArray(1, char(0x03)))
                                               : QStringLiteral("未连接");
                       if (!err.isEmpty())
                           return ToolResult{false, false, {}, err};
                       QJsonObject data;
                       data.insert(QStringLiteral("sent"), QStringLiteral("^C"));
                       data.insert(QStringLiteral("typing_interrupted"), wasTyping);
                       return ToolResult{true, false, data, QString()};
                   });
    meta("xterm.transport.interrupt",
         QStringLiteral("中断当前命令：停掉 AI 打字机（若在打）并向链路发 Ctrl+C，"
                        "用于打断 top/cat 大文件等长输出"),
         QJsonObject());

    m_tools.insert(QStringLiteral("xterm.transport.sendLine"),
                   [this](const ToolCall &call) -> ToolResult {
                       const QString text = call.params.value(QStringLiteral("text")).toString();
                       // 本地命令（clear -a）保持整行直达，不进打字机
                       if (text.trimmed() == QLatin1String("clear -a")) {
                           const QString err = m_hooks.sendLine ? m_hooks.sendLine(text)
                                                                : QStringLiteral("未连接");
                           if (!err.isEmpty())
                               return ToolResult{false, false, {}, err};
                           QJsonObject data;
                           data.insert(QStringLiteral("text"),
                                       m_hooks.recentText ? m_hooks.recentText(clampLines(0)) : QString());
                           return ToolResult{true, false, data, QString()};
                       }
                       // 打字机逐字发送：用户看得见输入过程，且随时可被自己的操作打断。
                       // 上限 1 秒/字符：常规命令 20ms 默认，慢速演示/留足打断窗口时用大间隔
                       const QString needle = call.params.value(QStringLiteral("wait")).toString();
                       const int interval = qBound(
                           5, call.params.value(QStringLiteral("interval_ms")).toInt(20), 1000);
                       startTyping(call.sock, call.id, text, needle,
                                   clampTimeout(call.params.value(QStringLiteral("timeout_ms")).toInt()),
                                   interval);
                       return ToolResult{true, true, {}, QString()}; // 打字机接管应答
                   });
    {
        QJsonObject params;
        QJsonObject text;
        text.insert(QStringLiteral("type"), QStringLiteral("string"));
        text.insert(QStringLiteral("description"), QStringLiteral("要发送的一行（不含行尾，软件按链路补）"));
        params.insert(QStringLiteral("text"), text);
        QJsonObject wait;
        wait.insert(QStringLiteral("type"), QStringLiteral("string"));
        wait.insert(QStringLiteral("description"), QStringLiteral("发完后等待屏幕新增中出现该文本"));
        params.insert(QStringLiteral("wait"), wait);
        QJsonObject timeout;
        timeout.insert(QStringLiteral("type"), QStringLiteral("integer"));
        timeout.insert(QStringLiteral("description"), QStringLiteral("wait 超时毫秒数，默认 3000"));
        params.insert(QStringLiteral("timeout_ms"), timeout);
        QJsonObject interval;
        interval.insert(QStringLiteral("type"), QStringLiteral("integer"));
        interval.insert(QStringLiteral("description"), QStringLiteral("打字间隔毫秒，默认 20，上限 1000"));
        params.insert(QStringLiteral("interval_ms"), interval);
        meta("xterm.transport.sendLine",
             QStringLiteral("打字机式逐字发送一行（用户可见、可 Ctrl+C 打断），可附带 wait"), params);
    }

    // B 组正名（M2.2）：sendTypewriter 是 sendLine 的正式命名入口（sendLine
    // 本身就是打字机式，两者行为完全一致），同名同参直接复用同一处理器
    m_tools.insert(QStringLiteral("xterm.transport.sendTypewriter"),
                   m_tools.value(QStringLiteral("xterm.transport.sendLine")));
    {
        QJsonObject twMeta = m_toolMeta.value(
            QStringLiteral("xterm.transport.sendLine"));
        twMeta.insert(QStringLiteral("name"),
                      QStringLiteral("xterm.transport.sendTypewriter"));
        twMeta.insert(QStringLiteral("description"),
                      QStringLiteral("sendLine 的正式命名入口：打字机式逐字发送一行，"
                                     "行为与 sendLine 完全一致，可附带 wait"));
        m_toolMeta.insert(QStringLiteral("xterm.transport.sendTypewriter"), twMeta);
    }

    m_tools.insert(QStringLiteral("xterm.screen.wait"),
                   [this](const ToolCall &call) -> ToolResult {
                       const QString needle = call.params.value(QStringLiteral("contains")).toString();
                       if (needle.isEmpty())
                           return ToolResult{false, false, {},
                                             QStringLiteral("wait 需要 contains")};
                       // 同 sendLine：以绝对行锚点判「新增」，高频刷屏下依旧可靠
                       addWaiter(call.sock, call.id, needle, 1,
                                 clampTimeout(call.params.value(QStringLiteral("timeout_ms")).toInt()));
                       return ToolResult{true, true, {}, QString()}; // 轮询器接管应答
                   });
    {
        QJsonObject params;
        QJsonObject contains;
        contains.insert(QStringLiteral("type"), QStringLiteral("string"));
        contains.insert(QStringLiteral("description"),
                        QStringLiteral("等待屏幕新增文本中出现该子串（回显免疫）"));
        params.insert(QStringLiteral("contains"), contains);
        QJsonObject timeout;
        timeout.insert(QStringLiteral("type"), QStringLiteral("integer"));
        timeout.insert(QStringLiteral("description"), QStringLiteral("超时毫秒数，默认 3000，上限 30000"));
        params.insert(QStringLiteral("timeout_ms"), timeout);
        meta("xterm.screen.wait", QStringLiteral("等待屏幕新增文本出现指定子串，超时返回 timed_out"), params);
    }

    m_tools.insert(QStringLiteral("xterm.screen.waitForPattern"),
                   [this](const ToolCall &call) -> ToolResult {
                       const QString pattern =
                           call.params.value(QStringLiteral("pattern")).toString();
                       if (pattern.isEmpty())
                           return ToolResult{false, false, {},
                                             QStringLiteral("waitForPattern 需要 pattern")};
                       // 语义对齐 M3.3 查找：大小写不敏感；非法正则直接报错
                       // （比 GUI 的「当无命中」更显式，AI 能感知写错了）
                       QRegularExpression re(pattern,
                                             QRegularExpression::CaseInsensitiveOption);
                       if (!re.isValid())
                           return ToolResult{false, false, {},
                                             QStringLiteral("正则无效 (%1): %2")
                                                 .arg(re.errorString(), pattern)};
                       const int minNew = qBound(
                           1, call.params.value(QStringLiteral("min_new")).toInt(1), 10);
                       addWaiter(call.sock, call.id, pattern, minNew,
                                 clampTimeout(call.params.value(QStringLiteral("timeout_ms")).toInt()),
                                 re);
                       return ToolResult{true, true, {}, QString()}; // 轮询器接管应答
                   });
    {
        QJsonObject params;
        QJsonObject pattern;
        pattern.insert(QStringLiteral("type"), QStringLiteral("string"));
        pattern.insert(QStringLiteral("description"),
                       QStringLiteral("正则（大小写不敏感），如 \"error\"|\"fail\""));
        params.insert(QStringLiteral("pattern"), pattern);
        QJsonObject minNew;
        minNew.insert(QStringLiteral("type"), QStringLiteral("integer"));
        minNew.insert(QStringLiteral("description"),
                      QStringLiteral("新增中至少出现次数，默认 1；发命令后等输出用 2 防回显"));
        params.insert(QStringLiteral("min_new"), minNew);
        QJsonObject timeout;
        timeout.insert(QStringLiteral("type"), QStringLiteral("integer"));
        timeout.insert(QStringLiteral("description"), QStringLiteral("超时毫秒数，默认 3000，上限 30000"));
        params.insert(QStringLiteral("timeout_ms"), timeout);
        meta("xterm.screen.waitForPattern",
             QStringLiteral("等待屏幕新增文本匹配正则（比 wait 的子串更活），超时返回 timed_out"), params);
    }
}

QJsonArray AgentBridge::toolCatalog() const
{
    QJsonArray tools;
    for (auto it = m_toolMeta.constBegin(); it != m_toolMeta.constEnd(); ++it)
        tools.append(it.value());
    return tools;
}

void AgentBridge::setRateLimit(int capacity, int perSecond)
{
    m_rateCapacity = qMax(1, capacity);
    m_ratePerSecond = qMax(1, perSecond);
    m_rateTokens = m_rateCapacity; // 重置满桶，语义直观：改参数即重启计量
    m_rateLastMs = 0;
}

bool AgentBridge::takeRateToken()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_rateLastMs > 0) {
        const double refill = (now - m_rateLastMs) * double(m_ratePerSecond) / 1000.0;
        m_rateTokens = qMin(double(m_rateCapacity), m_rateTokens + refill);
    }
    m_rateLastMs = now;
    if (m_rateTokens < 1.0)
        return false;
    m_rateTokens -= 1.0;
    return true;
}

QString AgentBridge::pendingKey(QLocalSocket *sock, const QJsonValue &id) const
{
    QString idStr;
    if (id.isString())
        idStr = id.toString();
    else if (id.isDouble())
        idStr = QString::number(id.toDouble());
    else if (id.isBool())
        idStr = id.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    else
        idStr = QStringLiteral("_"); // 无 id 请求（协议允许）：占位同名覆盖
    return QString::number(quintptr(sock)) + QLatin1Char('#') + idStr;
}

void AgentBridge::settlePending(QLocalSocket *sock, const QJsonValue &id,
                                bool ok, const QString &error)
{
    const QString key = pendingKey(sock, id);
    const auto it = m_pending.constFind(key);
    if (it == m_pending.constEnd())
        return; // 非工具调用应答（坏 JSON 的占位回复等），不审计不发射
    const PendingCall p = it.value();
    m_pending.erase(it);

    const qint64 durationMs = QDateTime::currentMSecsSinceEpoch() - p.startMs;
    if (m_audit) {
        QJsonObject e;
        e.insert(QStringLiteral("event"), QStringLiteral("call"));
        e.insert(QStringLiteral("method"), p.method);
        e.insert(QStringLiteral("ok"), ok);
        e.insert(QStringLiteral("duration_ms"), double(durationMs));
        e.insert(QStringLiteral("params"), p.params);
        if (!error.isEmpty())
            e.insert(QStringLiteral("error"), error.left(200)); // 摘要：防整屏文本进日志
        m_audit->append(e);
    }
    emit agentToolCall(p.method, ok, durationMs);
}

void AgentBridge::addWaiter(QLocalSocket *sock, const QJsonValue &id, const QString &needle,
                            int minNew, int timeoutMs, const QRegularExpression &re)
{
    Waiter w;
    w.sock = sock;
    w.id = id;
    w.needle = needle;
    w.regex = re.isValid() && !re.pattern().isEmpty();
    w.re = w.regex ? re : QRegularExpression();
    // 挂起时刻的绝对行锚点：命中只看这之后新增的行（M3.4 语义）
    w.fromLine = m_hooks.recentLineCount ? m_hooks.recentLineCount() : -1;
    w.minNew = minNew;
    w.timeoutMs = timeoutMs;
    w.deadlineMs = QDateTime::currentMSecsSinceEpoch() + w.timeoutMs;
    m_waiters.append(w);
    if (!m_poll->isActive())
        m_poll->start();
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
        // 命中条件: 锚点之后的新增文本里 needle 出现次数达到 minNew
        // （普通为 1，回显免疫场景为 2）；正则模式（M2.d waitForPattern）
        // 计非空匹配次数，空匹配不计
        const QString fresh = (m_hooks.recentTextFrom && w.fromLine >= 0)
                                  ? m_hooks.recentTextFrom(w.fromLine, 400) : QString();
        int count = 0;
        if (w.regex) {
            auto it = w.re.globalMatch(fresh);
            while (it.hasNext()) {
                if (it.next().capturedLength() > 0)
                    ++count;
            }
        } else {
            count = fresh.count(w.needle);
        }
        const bool hit = count >= w.minNew;
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
    // 自适应加速：整条命令的打字总时长控制在 ~1.2 秒内（tick 数 ≈ 1200/间隔），
    // 短命令保持逐字手速（可见的"AI 在输入"），长命令自动多个字符一并进，
    // 真实批量执行时几乎无感延迟。慢速档（大 interval_ms）仍用于演示/打断窗口。
    const int maxTicks = qMax(1, 1200 / qMax(1, m_typeTimer->interval()));
    const int chunk = qMax(1, (m_typing.rest.size() + maxTicks - 1) / maxTicks);
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
    // 行锚点：命中只看挂起之后新增的行——滑动窗口里的出现次数差
    // 在高频刷屏设备上不会增长（旧的滚出、新的滚入，相互抵消）。
    addWaiter(sock, id, needle, minNew, timeoutMs);
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
