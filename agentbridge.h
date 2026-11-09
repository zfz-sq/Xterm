#ifndef AGENTBRIDGE_H
#define AGENTBRIDGE_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QRegularExpression>
#include <QString>

#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QTimer;
class AuditLog;

/*
 * AgentBridge: 本机命名管道，把已经连上的串口/SSH 暴露给 MCP。
 * 只接受带本次启动令牌的请求。管道名和令牌写在配置目录的 agent.json。
 * 全部在 GUI 线程处理；wait 用定时器轮询屏幕，不卡住界面。
 */
class AgentBridge : public QObject
{
    Q_OBJECT

public:
    // —— M2.a 工具路由：bridge 只做令牌鉴权 + 分发，工具本体是注册表条目 ——
    // 一次工具调用的上下文（sock/id 供挂起类工具接管应答）
    struct ToolCall
    {
        QJsonObject params;
        QLocalSocket *sock = nullptr;
        QJsonValue id;
        // 返回 deferred 之后用它完成应答。同步完成不要调用，bridge 按 ToolResult 回。
        std::function<void(bool ok, const QJsonObject &data, const QString &error)> reply;
    };
    // deferred = 处理器已接管应答（打字机、wait、以及尚未完成的文件/主机查询），
    // bridge 不再回复；否则 bridge 按 ok/data/error 应答
    struct ToolResult
    {
        bool ok = true;
        bool deferred = false;
        QJsonObject data;
        QString error;
    };
    using ToolHandler = std::function<ToolResult(const ToolCall &)>;

    // 后端既可能当场回调（测试 fake、离线失败），也可能等信号（真实 SFTP）。
    // 当场完成时把结果留在 ToolResult 里；晚到的完成走 ToolCall::reply。
    class ToolCompletion
    {
    public:
        ToolCompletion() = default;
        explicit ToolCompletion(const ToolCall &call)
            : m_state(std::make_shared<State>())
        {
            m_state->reply = call.reply;
        }

        void finish(bool ok, const QJsonObject &data, const QString &error) const
        {
            if (!m_state || m_state->settled)
                return;
            m_state->settled = true;
            if (m_state->armed) {
                if (m_state->reply)
                    m_state->reply(ok, data, error);
                return;
            }
            m_state->sync = true;
            m_state->ok = ok;
            m_state->data = data;
            m_state->error = error;
        }

        ToolResult resultOrDefer() const
        {
            if (!m_state)
                return ToolResult{false, false, {}, QStringLiteral("内部错误")};
            m_state->armed = true;
            if (m_state->sync)
                return ToolResult{m_state->ok, false, m_state->data, m_state->error};
            return ToolResult{true, true, {}, {}};
        }

    private:
        struct State
        {
            bool settled = false;
            bool armed = false;
            bool sync = false;
            bool ok = false;
            QJsonObject data;
            QString error;
            std::function<void(bool, const QJsonObject &, const QString &)> reply;
        };
        std::shared_ptr<State> m_state;
    };

    struct Hooks
    {
        std::function<QJsonObject()> status;
        std::function<QString(int maxLines)> recentText;
        // 绝对行号（M3.4）：环形丢弃计数 + 现存行数，单调递增。wait 的「新增」
        // 锚点——相对行号在环形丢行后会漂移，这是 M3 唯一动 M2 语义的点
        std::function<qint64()> recentLineCount;
        // 自绝对行号 fromLine 起的新增文本（wait 命中只看这段）。
        std::function<QString(qint64 fromLine, int maxLines)> recentTextFrom;
        // 空字符串表示已交给当前链路；非空是可以直接返回给调用方的错误。
        // 整行直达（clear -a 等本地命令走这里）。
        std::function<QString(const QString &text)> sendLine;
        // 裸字节直达（控制字符、打字机逐段发送），不补行尾、不记行历史。
        std::function<QString(const QByteArray &bytes)> sendBytes;
        // 当前链路的行尾字符（串口 '\n'，SSH '\r'），打字机最后发它。
        std::function<char()> lineEnding;
        // 打字机完整发完一行后回调（记输入历史）。被打断时不调用。
        std::function<void(const QString &line)> lineSent;

        // M2.a 命名空间工具注册表（key = 完整名，如 "xterm.fs.stat"）：
        // 内置五件（xterm.session.status / xterm.screen.read / xterm.screen.wait /
        // xterm.transport.sendLine / xterm.transport.sendControl）由 bridge 基于
        // 上面能力字段自动注册；调用方同名条目覆盖内置，新名即新工具
        // （M2.b FS / M2.c Host 从这里注入）
        QHash<QString, ToolHandler> tools;
        // extraTools 的 tools.list 元数据：完整名 → {description, params}
        QHash<QString, QJsonObject> toolMeta;
    };

    // pipeName/agentFile 可注入：单测用独立管道与临时文件，不碰真实配置。
    explicit AgentBridge(QObject *parent = nullptr,
                         const QString &pipeName = QStringLiteral("XtermAgent"));
    ~AgentBridge() override;

    // 监听成功后写 agent.json。已经有实例占着管道时什么都不改。
    // allowWrite：写类工具（xterm.fs.write/upload）总开关，写入 agent.json
    // 披露给客户端；默认 false（M2.b 安全边界：读类默认可用，写类需显式开）
    void start(Hooks hooks, const QString &agentFile = QString(), bool allowWrite = false);
    QString pipeName() const { return m_pipeName; }
    // 本实例是否在服务管道（false = 被另一个 Xterm 实例占用）
    bool isServing() const { return m_server != nullptr; }

    // 用户在终端里有了动作（键盘/粘贴/命令栏）：立即中止 AI 的打字机，
    // 剩余字符丢弃，对应的 xterm.transport.sendLine 请求以 interrupted 错误应答。
    // 返回是否真的中止了一次打字。
    bool interruptTyping();

    // 打字机是否正在输入（用于界面锁定人工输入）
    bool isTyping() const { return m_typing.active; }

    // 速率限制（M2.e 令牌桶）：容量 capacity 个、每秒回填 perSecond 个，
    // 超性请求立即拒绝（防 AI 循环刷 read 类工具卡 GUI 线程）。start 前后
    // 均可调；单测用小参数验证拒绝路径
    void setRateLimit(int capacity, int perSecond);

    static QString instructions();

signals:
    // 携带合法令牌的请求到达: 有 AI 正在通过 MCP 访问本窗口（用于点亮状态角标）。
    // 连接是按请求建立的，没有“断开”事件，状态由上层超时判定。
    void agentActive();
    // 一次工具调用完成（M2.e 可观测）：method 带命名空间全名，
    // durationMs 含挂起类工具的等待时间（从请求到达到应答）。
    // 挂起后客户端断开没应答的调用不发声；上层用幂等文案刷新
    void agentToolCall(const QString &method, bool ok, qint64 durationMs);
    // 打字机状态变化：active=true 时 text 是正在逐字输入的命令（进度随 tick 刷新），
    // active=false 表示结束（发完或被打断）。
    void agentTypingChanged(bool active, const QString &text);

private:
    // 审计结算上下文（M2.e）：请求到达时入表，reply() 统一出口结算
    struct PendingCall
    {
        QString method;
        QJsonObject params;
        qint64 startMs = 0;
    };

    struct Waiter
    {
        QLocalSocket *sock = nullptr;
        QJsonValue id;
        QString needle;
        // M2.d waitForPattern：needle 按正则解释（大小写不敏感）；
        // 空匹配不计次，与 M3.3 的查找语义一致
        bool regex = false;
        QRegularExpression re;
        qint64 fromLine = -1; // 挂起时的绝对行锚点：命中只看这之后的新增文本
        int minNew = 1;    // needle 需在新增里出现的最少次数（回显免疫时为 2）
        int timeoutMs = 3000;
        qint64 deadlineMs = 0;
    };

    struct Typing
    {
        bool active = false;
        QLocalSocket *sock = nullptr; // 打完后应答/等待的对端（可能已断开，reply 自查）
        QJsonValue id;
        QByteArray rest;   // 尚未发出的字节（含结尾行尾符）
        QString command;   // 完整命令（不含行尾），用于 UI 与 lineSent
        QString waitNeedle;
        int minNew = 1;
        int timeoutMs = 3000;
    };

    void onNewConnection();
    void onSocketReady(QLocalSocket *sock);
    void onSocketClosed(QLocalSocket *sock);
    void handleLine(QLocalSocket *sock, const QByteArray &line);
    void registerBuiltinTools();               // 基于 m_hooks 能力字段注册内置工具
    QJsonArray toolCatalog() const;            // tools.list 的清单（含参数描述）
    void addWaiter(QLocalSocket *sock, const QJsonValue &id, const QString &needle,
                   int minNew, int timeoutMs,
                   const QRegularExpression &re = QRegularExpression()); // wait 挂起（sendLine 带也用）
    void reply(QLocalSocket *sock, const QJsonValue &id, bool ok, const QJsonObject &result,
               const QString &error);
    void pollWaiters();
    // —— M2.e 可观测：审计 + 限流 ——
    bool takeRateToken();                          // 令牌桶取令牌（自回填）
    QString pendingKey(QLocalSocket *sock, const QJsonValue &id) const;
    void settlePending(QLocalSocket *sock, const QJsonValue &id,
                       bool ok, const QString &error); // reply 出口结算审计+信号
    void startTyping(QLocalSocket *sock, const QJsonValue &id, const QString &text,
                     const QString &waitNeedle, int timeoutMs, int intervalMs);
    void typingTick();
    void finishTyping(bool interrupted); // 停止计时器、清状态、发结束信号
    void writeAgentFile();
    void removeAgentFile();
    bool probeExistingAgent() const; // 探测是否已有活实例在服务管道

    Hooks m_hooks;
    QHash<QString, ToolHandler> m_tools;      // 完整工具名 → 处理器（路由表）
    QHash<QString, QJsonObject> m_toolMeta;   // 同名 → tools.list 元数据
    AuditLog *m_audit = nullptr;              // agentFile 同目录 logs/ 下 jsonl
    QHash<QString, PendingCall> m_pending;    // sock#id → 调用上下文（审计结算）
    double m_rateTokens = 30.0;               // 令牌桶当前令牌（容量默认 30）
    int m_rateCapacity = 30;
    int m_ratePerSecond = 10;                 // 每秒回填（默认 10）
    qint64 m_rateLastMs = 0;
    QLocalServer *m_server = nullptr;
    QTimer *m_poll = nullptr;
    QTimer *m_typeTimer = nullptr;
    Typing m_typing;
    QString m_pipeName;
    QString m_token;
    QString m_agentFile;
    bool m_allowWrite = false;
    QHash<QLocalSocket *, QByteArray> m_buf;
    QList<Waiter> m_waiters;
};

#endif // AGENTBRIDGE_H
