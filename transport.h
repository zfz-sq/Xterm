#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <QByteArray>
#include <QObject>
#include <QString>

/*
 * Transport: 链路后端的统一契约。串口（SerialSession）与 SSH（SshSession）
 * 都实现它，MainWindow 只面向这一套接口接线，新增链路不必改上层。
 *
 * 返回值约定（沿用 SerialSession）：空字符串 = 请求已被接受，非空 = 可直接
 * 显示进状态栏的错误原因。
 *
 * 生命周期是异步的：open() 只代表「开始建立」，真正的结果一律回到
 * opened() / errorOccurred()；已经建立后的结束回到 closed()。
 * 因此上层 UI 在 open() 返回后不能假定链路已可用，要看信号。
 *
 * 线程约定：全部公开方法与信号都在创建 Transport 的线程（GUI 线程）调用，
 * 内部若使用工作线程，由实现自己负责把数据以 queued 信号送回主线程。
 */
class Transport : public QObject
{
    Q_OBJECT

public:
    enum Kind
    {
        SerialLink,
        SshLink
    };

    enum State
    {
        Disconnected,   // 未建立，也没有在建立
        Connecting,     // 正在握手 / 拉起子进程
        Connected,      // 可收发
        Disconnecting   // 正在拆除，等 closed()
    };

    explicit Transport(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~Transport() override = default;

    virtual Kind kind() const = 0;
    State state() const { return m_state; }
    bool isOpen() const { return m_state == Connected; }

    // 请求拆除。幂等：非 Connected/Connecting 状态直接返回。
    // 真正的结束以 closed() 为准，UI 不要在调用后立刻假定已完成。
    virtual void close() = 0;

    // 原始字节下行。未连接时返回错误原因，不产生副作用。
    virtual QString send(const QByteArray &data) = 0;

signals:
    void opened();
    void closed();
    void errorOccurred(const QString &message);
    void received(const QByteArray &chunk);

protected:
    // 仅允许实现类在状态确实改变时设置，避免状态与信号脱节。
    void setState(State state)
    {
        if (m_state == state)
            return;
        m_state = state;
        emitStateChanged(state);
    }

    void emitOpened()
    {
        setState(Connected);
        emit opened();
    }
    void emitClosed()
    {
        setState(Disconnected);
        emit closed();
    }
    void emitError(const QString &message)
    {
        emit errorOccurred(message);
    }

signals:
    // 状态变化供上层刷新按钮文案（连接中 / 断开中），实现类通过 setState() 触发。
    void stateChanged(Transport::State state);

private:
    void emitStateChanged(State state) { emit stateChanged(state); }

    State m_state = Disconnected;
};

#endif // TRANSPORT_H
