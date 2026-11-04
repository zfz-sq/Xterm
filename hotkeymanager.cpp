#include "hotkeymanager.h"

#include <QAction>
#include <QKeySequence>

HotkeyManager::HotkeyManager(QObject *parent)
    : QObject(parent)
{
    // 三个 action 归本类所有，由 MainWindow 取走挂进「工具」菜单并接信号。
    // 快捷键唯一注册在这里（菜单项与按键同一动作，避免另起一套 QShortcut）。
    m_connect = new QAction(tr("连接"), this);
    m_connect->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_connect, &QAction::triggered, this, &HotkeyManager::connectRequested);

    m_switchMode = new QAction(tr("切换到 SSH"), this);
    m_switchMode->setShortcut(QKeySequence(Qt::Key_F9));
    connect(m_switchMode, &QAction::triggered, this, &HotkeyManager::switchModeRequested);

    m_sftp = new QAction(tr("SFTP"), this);
    m_sftp->setShortcut(QKeySequence(Qt::Key_F10));
    connect(m_sftp, &QAction::triggered, this, &HotkeyManager::sftpRequested);
}

void HotkeyManager::setConnectText(const QString &text)
{
    m_connect->setText(text);
}

void HotkeyManager::setSwitchModeText(const QString &text)
{
    m_switchMode->setText(text);
}
