#ifndef SESSIONBRIDGE_H
#define SESSIONBRIDGE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

#include "agentbridge.h" // ToolHandler / Hooks（工具注册表）

/*
 * SessionBridge: AgentBridge 的 xterm.session 扩展工具后端（M2.f）。
 *
 * 六件工具：list / current / switch / openSsh / openSerial / close
 * （status 是 AgentBridge 内置，M2.a 起就有）。动作语义见 SessionController
 * 的 agent* 适配器——open 幂等（已连接直接回状态），重连 = 先 close 再 open。
 *
 * 后端注入：FsBridge/HostBridge/ScreenBridge 同款纯函数集，本类零会话/
 * UI 依赖（真实实现在 SessionController，测试喂 fake 即可）。
 */
class SessionBridge
{
public:
    // 动作类返回 err（空串 = 成功），out 填给工具的 data
    struct SessionHooks
    {
        std::function<QJsonArray()> list;     // 两条链路状态
        std::function<QJsonObject()> current; // 当前会话详情
        std::function<QString(const QString &kind, QJsonObject *out)> switchTo;
        std::function<QString(const QJsonObject &params, QJsonObject *out)> openSsh;
        std::function<QString(const QJsonObject &params, QJsonObject *out)> openSerial;
        std::function<QString(const QString &kind, QJsonObject *out)> close;
    };

    SessionBridge() = default;

    void setBackend(SessionHooks hooks) { m_hooks = std::move(hooks); }

    // 六件工具填进 hooks->tools / hooks->toolMeta（AgentBridge::start 合入注册表）
    void registerTools(AgentBridge::Hooks *hooks);

private:
    SessionHooks m_hooks;
};

#endif // SESSIONBRIDGE_H
