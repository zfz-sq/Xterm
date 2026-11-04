// M1.c McpMainHook 协作类测试：部件创建/初始可见性、打字机守卫的启动前安全、
// start() 后状态标签联动。不驱动真实打字机信号（那属 AgentBridge，另有 test_agentbridge），
// 只锁协作类自身的窄职责与状态契约，验证「状态灯/锁定提示无回归」。
#include <QLabel>
#include <QStatusBar>
#include <QTest>
#include <QToolButton>

#include "mcpmainhook.h"

class TestMcpMainHook : public QObject
{
    Q_OBJECT

private slots:
    void widgetsCreatedAndInitiallyHidden();
    void guardsSafeBeforeStart();
    void startShowsPersistentStatusAndKeepsLockHidden();
};

void TestMcpMainHook::widgetsCreatedAndInitiallyHidden()
{
    QStatusBar bar;
    McpMainHook hook(&bar);

    // 三个部件均由协作类创建并对外暴露，供主窗口按序挂载
    QVERIFY(hook.button() != nullptr);
    QVERIFY(hook.statusWidget() != nullptr);
    QVERIFY(hook.lockWidget() != nullptr);

    // 构造阶段：状态标签与锁定标签隐藏（状态要等 start() 刷新，锁定只在打字时出现）
    QVERIFY(hook.statusWidget()->isHidden());
    QVERIFY(hook.lockWidget()->isHidden());
}

void TestMcpMainHook::guardsSafeBeforeStart()
{
    QStatusBar bar;
    McpMainHook hook(&bar);

    // 未启动桥接时 m_agent 为空，守卫必须安全返回 false，不得崩
    QVERIFY(!hook.isTyping());
    QVERIFY(!hook.interruptTyping());
}

void TestMcpMainHook::startShowsPersistentStatusAndKeepsLockHidden()
{
    QStatusBar bar;
    McpMainHook hook(&bar);

    // 空 Hooks 即可：本用例只验证启动后的常驻状态标签，不发起任何工具调用
    hook.start(AgentBridge::Hooks{});

    // refreshStatus() 在 start() 末尾调用，状态标签转为可见且带文案
    // （就绪 / 未启用取决于管道是否被占用，两者都应为非空可见）
    QVERIFY(!hook.statusWidget()->isHidden());
    QVERIFY(!hook.statusWidget()->text().isEmpty());

    // 非打字状态：AI 锁定提示仍隐藏
    QVERIFY(hook.lockWidget()->isHidden());

    // 启动后打字守卫可用且初始为空闲
    QVERIFY(!hook.isTyping());
}

QTEST_MAIN(TestMcpMainHook) // 涉及 QStatusBar/QToolButton/QLabel，需 QApplication
#include "test_mcpmainhook.moc"
