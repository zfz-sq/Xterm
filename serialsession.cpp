#include "serialsession.h"

SerialSession::SerialSession(QObject *parent)
    : Transport(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &SerialSession::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &SerialSession::onSerialError);
}

QString SerialSession::open(const QString &portName, qint32 baudRate,
                            QSerialPort::DataBits dataBits, QSerialPort::Parity parity,
                            QSerialPort::StopBits stopBits,
                            QSerialPort::FlowControl flowControl, bool dtr, bool rts)
{
    if (m_port.isOpen())
        m_port.close();
    setState(Disconnected);

    m_port.setPortName(portName);
    m_port.setBaudRate(baudRate);
    m_port.setDataBits(dataBits);
    m_port.setParity(parity);
    m_port.setStopBits(stopBits);
    m_port.setFlowControl(flowControl);

    if (!m_port.open(QIODevice::ReadWrite)) {
        const QString err = tr("打开 %1 失败：%2").arg(portName, m_port.errorString());
        emitError(err);
        return err;
    }
    // DTR/RTS 只能在打开后压电平。Windows 默认把两根线都拉高，
    // ESP32/Arduino 这类 DTR/RTS 接复位电路的板子会被无端复位，
    // 所以电平交给配置决定；硬件流控下 RTS 由驱动接管，设置会被忽略。
    m_port.setDataTerminalReady(dtr);
    m_port.setRequestToSend(rts);
    emitOpened();
    return QString();
}

void SerialSession::close()
{
    if (!m_port.isOpen()) {
        setState(Disconnected);
        return;
    }
    m_port.close();
    emitClosed();
}

QString SerialSession::send(const QByteArray &data)
{
    if (state() != Connected)
        return tr("串口未连接，无法发送");
    if (m_port.write(data) == -1)
        return tr("写入 %1 失败：%2").arg(m_port.portName(), m_port.errorString());
    return QString();
}

void SerialSession::onReadyRead()
{
    emit received(m_port.readAll());
}

void SerialSession::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;

    // 设备被拔出：关闭并上报
    if (error == QSerialPort::ResourceError) {
        const QString msg = tr("串口错误：%1（设备可能已被拔出）").arg(m_port.errorString());
        close();
        emitError(msg);
        return;
    }
    // 打开失败时的错误已经在 open() 返回值里上报，这里只处理运行期错误
    if (m_port.isOpen())
        emitError(tr("串口错误：%1").arg(m_port.errorString()));
}
