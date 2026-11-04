#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include "fsbridge.h"       // FsBridge 值成员（M2.b：xterm.fs.* 工具后端）
#include "hostbridge.h"     // HostBridge 值成员（M2.c：xterm.host.* 工具后端）
#include "sessioncontroller.h" // Mode（别名保留）与会话协作类

class CommandStore;
class HotkeyManager;
class McpMainHook;
class ThemeManager;
class UiBuilder;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

/*
 * MainWindow（M1 拆分后）: 纯编排壳。
 *
 * 会话生命周期在 SessionController，UI 编排与刷新在 UiBuilder，主题在
 * ThemeManager，快捷键在 HotkeyManager，MCP 挂钩在 McpMainHook；本类只
 * 保留协作类创建顺序、跨协作类接线（终端输入的 AI 锁定守卫、PTY 尺寸
 * 同步、MCP Hooks 组装）、eventFilter/closeEvent/changeEvent 编排。
 * 发送入口 sendRaw/sendLine 的实现移至 SessionController（行为不变）。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // 工作模式定义随 M1.d 迁至 SessionController；保留别名兼容旧引用
    using Mode = SessionController::Mode;

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void connectSignals(); // 跨协作类胶水：终端输入守卫、PTY 尺寸同步

    Ui::MainWindow *ui;
    CommandStore *m_store = nullptr;        // 配置状态源
    ThemeManager *m_theme = nullptr;        // 主题切换协作类（M1.a）
    HotkeyManager *m_hotkey = nullptr;      // 窗口级快捷键 F5/F9/F10（M1.b）
    McpMainHook *m_mcp = nullptr;           // MCP 挂钩/角标/状态提示（M1.c）
    SessionController *m_session = nullptr; // 会话生命周期协作类（M1.d）
    UiBuilder *m_uiBuilder = nullptr;       // UI 编排与状态刷新协作类（M1.d）
    FsBridge m_fs;                          // xterm.fs.* 工具后端（M2.b，非 QObject）
    HostBridge m_host;                      // xterm.host.* 工具后端（M2.c，非 QObject）
};
#endif // MAINWINDOW_H
