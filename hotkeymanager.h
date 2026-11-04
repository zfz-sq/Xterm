#ifndef HOTKEYMANAGER_H
#define HOTKEYMANAGER_H

#include <QObject>

class QAction;

/*
 * HotkeyManager: 窗口级全局快捷键协作类（MainWindow 拆分 M1.b）。
 *
 * 职责：创建并持有 F5/F9/F10 三个 QAction，统一注册快捷键，
 * 提供菜单挂载入口与文案联动。触发行为通过信号回抛给 MainWindow，
 * 本类不反向持有主窗口，也不碰具体业务（连接/切换/文件管理）。
 *
 * 边界说明：Ctrl+F / Ctrl+滚轮 / F1–F8 透传等属于 ScreenView 局部按键
 * （操作视图自身的查找栏、字号、选区），不在此登记；AI 打字机期间的
 * 键盘拦截是输入守卫，留在主窗口事件过滤器。
 */
class HotkeyManager : public QObject
{
    Q_OBJECT

public:
    explicit HotkeyManager(QObject *parent = nullptr);

    // 供标题栏「工具」菜单 addAction 挂载（action 归本类所有）
    QAction *connectAction() const { return m_connect; }     // F5
    QAction *switchModeAction() const { return m_switchMode; } // F9
    QAction *sftpAction() const { return m_sftp; }           // F10

    // 文案联动：连接项随连接状态（连接/断开），切换项随当前模式（→SSH/→串口）
    void setConnectText(const QString &text);
    void setSwitchModeText(const QString &text);

signals:
    void connectRequested();
    void switchModeRequested();
    void sftpRequested();

private:
    QAction *m_connect = nullptr;
    QAction *m_switchMode = nullptr;
    QAction *m_sftp = nullptr;
};

#endif // HOTKEYMANAGER_H
