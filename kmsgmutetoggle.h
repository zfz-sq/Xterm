#ifndef KMSGMUTETOGGLE_H
#define KMSGMUTETOGGLE_H

#include <QToolButton>

class SerialSession;

/*
 * KmsgMuteToggle: 状态栏「内核日志静音」开关（MainWindow 拆分 M1.d）。
 *
 * 设备侧 printk（WiFi/蓝牙驱动刷屏）会异步直写串口控制台，插进 vi/top
 * 画面字节流的任意位置，任何终端仿真都无法分离——唯一根治是降低
 * console_loglevel。开关只负责翻转「期望状态」并请求发送：
 *   静音: echo 1 > /proc/sys/kernel/printk（仅 KERN_EMERG 上串口）
 *   恢复: echo 7 > /proc/sys/kernel/printk
 * 实际发送走主链路 sendLine（共享 AI 锁定与输入历史），由外部连接
 * commandRequested 完成；设备重启/重连后 printk 回默认值，外部在
 * opened/closed 时调 reset()，避免显示与实际脱节。
 */
class KmsgMuteToggle : public QToolButton
{
    Q_OBJECT

public:
    // serial 仅用于点击时的连接状态检查（state()），不拥有。
    // 若创建时尚无会话（UiBuilder 先于 SessionController），可后补 setSerial。
    explicit KmsgMuteToggle(SerialSession *serial, QWidget *parent = nullptr);

    void setSerial(SerialSession *serial) { m_serial = serial; }

    bool isMuted() const { return m_muted; }

    // 连接建立/断开时调用：设备侧 printk 回到默认级别，期望状态重新计
    void reset();

signals:
    // 点击且串口已连接时发出（echo 1 或 echo 7 的完整命令行）
    void commandRequested(const QString &line);
    // 静音/恢复生效后给终端页的系统提示（── … ──）
    void noticeRequested(const QString &systemMessage);
    // 串口未连接时点击的状态栏提示
    void statusMessageRequested(const QString &text, int timeoutMs);

private:
    void updateUi();

    SerialSession *m_serial = nullptr;
    bool m_muted = false; // 客户端记住的期望状态
};

#endif // KMSGMUTETOGGLE_H
