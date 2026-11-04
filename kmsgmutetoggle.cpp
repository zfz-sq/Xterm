#include "kmsgmutetoggle.h"

#include <QEvent>

#include "serialsession.h"
#include "transport.h"

KmsgMuteToggle::KmsgMuteToggle(SerialSession *serial, QWidget *parent)
    : QToolButton(parent), m_serial(serial)
{
    setAutoRaise(true);
    connect(this, &QToolButton::clicked, this, [this] {
        if (!m_serial || m_serial->state() != Transport::Connected) {
            emit statusMessageRequested(tr("串口未连接，无法切换内核日志状态"), 5000);
            return;
        }
        m_muted = !m_muted;
        emit commandRequested(QStringLiteral("echo %1 > /proc/sys/kernel/printk")
                                  .arg(m_muted ? 1 : 7));
        updateUi();
        emit noticeRequested(m_muted
            ? tr("── 内核日志已静音（仅 EMERG 级上串口，dmesg 仍可查看）──")
            : tr("── 内核日志已恢复输出 ──"));
    });
    updateUi();
}

void KmsgMuteToggle::reset()
{
    m_muted = false;
    updateUi();
}

void KmsgMuteToggle::updateUi()
{
    setText(m_muted ? tr("内核日志：静音") : tr("内核日志：正常"));
    setToolTip(m_muted
        ? tr("内核日志已静音（echo 1 > /proc/sys/kernel/printk）。\n"
             "点击恢复输出（echo 7）。设备重启后需重新静音。")
        : tr("静音内核日志刷屏（echo 1 > /proc/sys/kernel/printk）：\n"
             "WiFi/蓝牙驱动的 printk 会插进 vi/top 的画面，静音后仅紧急级\n"
             "日志上串口，dmesg 随时仍可查看。再次点击恢复（echo 7）。"));
    // 静音是「非默认」状态，用琥珀色提示（与「连接中」同色，两主题都读得清）
    setStyleSheet(m_muted
        ? QStringLiteral("color: #D89B2C; font-weight: 600;")
        : QString());
}
