#ifndef AGENTBRIDGE_H
#define AGENTBRIDGE_H

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QLocalServer;
class QLocalSocket;
class QTimer;

/*
 * AgentBridge: 本机命名管道，把已经连上的串口/SSH 暴露给 MCP。
 * 只接受带本次启动令牌的请求。管道名和令牌写在配置目录的 agent.json。
 * 全部在 GUI 线程处理；wait 用定时器轮询屏幕，不卡住界面。
 */
class AgentBridge : public QObject
{
    Q_OBJECT

public:
    struct Hooks
    {
        std::function<QJsonObject()> status;
        std::function<QString(int maxLines)> recentText;
        // 滚动历史 + 当前屏的总行数（单调递增），wait 的「新增」锚点。
        std::function<int()> recentLineCount;
        // 自 fromLine 起的新增文本（wait 命中只看这段）。
        std::function<QString(int fromLine, int maxLines)> recentTextFrom;
        // 空字符串表示已交给当前链路；非空是可以直接返回给调用方的错误。
        // 整行直达（clear -a 等本地命令走这里）。
        std::function<QString(const QString &text)> sendLine;
        // 裸字节直达（控制字符、打字机逐段发送），不补行尾、不记行历史。
        std::function<QString(const QByteArray &bytes)> sendBytes;
        // 当前链路的行尾字符（串口 '\n'，SSH '\r'），打字机最后发它。
        std::function<char()> lineEnding;
        // 打字机完整发完一行后回调（记输入历史）。被打断时不调用。
        std::function<void(const QString &line)> lineSent;
    };

    // pipeName/agentFile 可注入：单测用独立管道与临时文件，不碰真实配置。
    explicit AgentBridge(QObject *parent = nullptr,
                         const QString &pipeName = QStringLiteral("XtermAgent"));
    ~AgentBridge() override;

    // 监听成功后写 agent.json。已经有实例占着管道时什么都不改。
    void start(Hooks hooks, const QString &agentFile = QString());
    QString pipeName() const { return m_pipeName; }

    // 用户在终端里有了动作（键盘/粘贴/命令栏）：立即中止 AI 的打字机，
    // 剩余字符丢弃，对应的 send_line 请求以 interrupted 错误应答。
    // 返回是否真的中止了一次打字。
    bool interruptTyping();

    // 打字机是否正在输入（用于界面锁定人工输入）
    bool isTyping() const { return m_typing.active; }

    static QString instructions();

signals:
    // 携带合法令牌的请求到达: 有 AI 正在通过 MCP 访问本窗口（用于点亮状态角标）。
    // 连接是按请求建立的，没有“断开”事件，状态由上层超时判定。
    void agentActive();
    // 打字机状态变化：active=true 时 text 是正在逐字输入的命令（进度随 tick 刷新），
    // active=false 表示结束（发完或被打断）。
    void agentTypingChanged(bool active, const QString &text);

private:
    struct Waiter
    {
        QLocalSocket *sock = nullptr;
        QJsonValue id;
        QString needle;
        int fromLine = -1; // 挂起时的行锚点：命中只看这之后的新增文本
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
    void reply(QLocalSocket *sock, const QJsonValue &id, bool ok, const QJsonObject &result,
               const QString &error);
    void pollWaiters();
    void startTyping(QLocalSocket *sock, const QJsonValue &id, const QString &text,
                     const QString &waitNeedle, int timeoutMs, int intervalMs);
    void typingTick();
    void finishTyping(bool interrupted); // 停止计时器、清状态、发结束信号
    void writeAgentFile();
    void removeAgentFile();
    bool probeExistingAgent() const; // 探测是否已有活实例在服务管道

    Hooks m_hooks;
    QLocalServer *m_server = nullptr;
    QTimer *m_poll = nullptr;
    QTimer *m_typeTimer = nullptr;
    Typing m_typing;
    QString m_pipeName;
    QString m_token;
    QString m_agentFile;
    QHash<QLocalSocket *, QByteArray> m_buf;
    QList<Waiter> m_waiters;
};

#endif // AGENTBRIDGE_H
