#ifndef SERIALSESSION_H
#define SERIALSESSION_H

#include <QByteArray>
#include <QSerialPort>

#include "transport.h"

/*
 * SerialSession: 独占 QSerialPort，实现 Transport 契约的串口后端。
 * 全程序只有这一处打开串口；按钮和终端共用同一个 send()。
 * 状态机由基类维护：open() 成功即 Connected，close() 同步完成。
 */
class SerialSession : public Transport
{
    Q_OBJECT

public:
    explicit SerialSession(QObject *parent = nullptr);

    Kind kind() const override { return SerialLink; }

    QString portName() const { return m_port.portName(); }

    QString open(const QString &portName, qint32 baudRate,
                 QSerialPort::DataBits dataBits, QSerialPort::Parity parity,
                 QSerialPort::StopBits stopBits,
                 QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl,
                 bool dtr = true, bool rts = true);
    void close() override;

    QString send(const QByteArray &data) override;

private:
    void onReadyRead();
    void onSerialError(QSerialPort::SerialPortError error);

    QSerialPort m_port; // 唯一的 QSerialPort 实例
};

#endif // SERIALSESSION_H
