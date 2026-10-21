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
        // 空字符串表示已交给当前链路；非空是可以直接返回给调用方的错误。
        std::function<QString(const QString &text)> sendLine;
    };

    explicit AgentBridge(QObject *parent = nullptr);
    ~AgentBridge() override;

    // 监听成功后写 agent.json。已经有实例占着管道时什么都不改。
    void start(Hooks hooks);
    QString pipeName() const { return m_pipeName; }

    static QString instructions();

private:
    struct Waiter
    {
        QLocalSocket *sock = nullptr;
        QJsonValue id;
        QString needle;
        int beforeCount = 0;
        int timeoutMs = 3000;
        qint64 deadlineMs = 0;
    };

    void onNewConnection();
    void onSocketReady(QLocalSocket *sock);
    void onSocketClosed(QLocalSocket *sock);
    void handleLine(QLocalSocket *sock, const QByteArray &line);
    void reply(QLocalSocket *sock, const QJsonValue &id, bool ok, const QJsonObject &result,
               const QString &error);
    void pollWaiters();
    void writeAgentFile();
    void removeAgentFile();

    Hooks m_hooks;
    QLocalServer *m_server = nullptr;
    QTimer *m_poll = nullptr;
    QString m_pipeName;
    QString m_token;
    QString m_agentFile;
    QHash<QLocalSocket *, QByteArray> m_buf;
    QList<Waiter> m_waiters;
};

#endif // AGENTBRIDGE_H
