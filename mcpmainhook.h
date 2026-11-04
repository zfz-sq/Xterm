#ifndef MCPMAINHOOK_H
#define MCPMAINHOOK_H

#include <QObject>

#include "agentbridge.h" // AgentBridge::Hooks

class QLabel;
class QStatusBar;
class QTimer;
class QToolButton;

/*
 * McpMainHook: MCP 挂钩与在线角标协作类（MainWindow 拆分 M1.c）。
 *
 * 职责：
 * - 拥有 AgentBridge 实例并按注入的数据适配器启动它
 * - 拥有 MCP 侧边按钮、状态栏常驻状态标签、AI 锁定提示标签、在线角标定时器
 * - 在线/打字信号 → 角标图标、状态文案、锁定提示的联动刷新
 * - 「显示 MCP 提示词」对话框
 *
 * 边界：agentStatus/agentRecentText/agentSendLine/agentSendBytes 等数据适配器
 * 依赖会话与视图状态，留在 MainWindow（M1.d 后归 SessionController），由主窗口
 * 组装成 AgentBridge::Hooks 注入 start()。本类不反向持有主窗口，也不碰会话逻辑。
 * 创建的部件由 MainWindow 按原顺序挂进命令栏侧边组与状态栏（挂载点归主窗口）。
 */
class McpMainHook : public QObject
{
    Q_OBJECT

public:
    // statusBar 仅用于打字机/打断的临时提示消息（showMessage）
    explicit McpMainHook(QStatusBar *statusBar, QObject *parent = nullptr);

    // 用主窗口提供的数据适配器启动桥接，并接好在线/打字信号与角标定时器。
    // allowWrite 透传给 AgentBridge（写类 FS 工具开关，写入 agent.json 披露）
    void start(const AgentBridge::Hooks &hooks, bool allowWrite = false);

    // 供主窗口挂载的部件（内容/可见性由本类管理，位置由主窗口决定）
    QToolButton *button() const { return m_mcpBtn; }
    QLabel *statusWidget() const { return m_mcpStatusLabel; }
    QLabel *lockWidget() const { return m_agentLockLabel; }

    // 主题级联：按当前主题与在线状态重建 MCP 按钮图标（含在线绿点）
    void refreshIcon();

    // AI 打字机守卫：主窗口在键盘拦截 / 发送路径上查询与打断
    bool isTyping() const;
    bool interruptTyping();

private:
    void setActive(bool on);
    void refreshStatus();
    void showPromptDialog();

    QStatusBar *m_statusBar = nullptr;
    AgentBridge *m_agent = nullptr;
    QToolButton *m_mcpBtn = nullptr;
    QLabel *m_mcpStatusLabel = nullptr;
    QLabel *m_agentLockLabel = nullptr;
    QTimer *m_mcpActiveTimer = nullptr;
    bool m_mcpActive = false;
};

#endif // MCPMAINHOOK_H
