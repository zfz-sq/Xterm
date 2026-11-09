#ifndef HOSTBRIDGE_H
#define HOSTBRIDGE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

#include "agentbridge.h" // ToolHandler / Hooks（工具注册表）
#include "hoststats.h"   // HostSnapshot（snapshot 后端的数据形状）

class SftpClient;

/*
 * HostBridge: AgentBridge 的 xterm.host.* 工具后端（M2.c）。
 *
 * 五件工具：
 * - cpu / mem：直读 HostStats 的 stats 通道缓存帧（CPU 差值需要第二帧，
 *   连接后约 6 秒就绪；未就绪时工具成功返回 ok=false 附说明，不算错误）
 * - disk / uptime / top：按需在远端跑一条只读命令（df / /proc / ps），
 *   输出解析成结构化 JSON 后返回，只对 SSH 会话可用（串口无主机概念）
 *
 * 后端注入：HostHooks 是回调集。exec 的真实实现等 SftpClient 信号，
 * 不在界面线程上空等；同一时刻只跑一条远端命令，进行中的第二次直接拒绝。
 * cpu / mem 只读缓存，不受这把锁影响。
 */
class HostBridge
{
public:
    struct HostHooks
    {
        std::function<HostSnapshot()> snapshot;
        // 按需远端命令。done 可能当场触发（离线失败、测试 fake），也可能稍后。
        // err 非空 = 命令失败，out 无效。
        std::function<void(const QString &command,
                           const std::function<void(const QString &out,
                                                    const QString &err)> &done)>
            exec;
    };

    HostBridge() = default;

    void setBackend(HostHooks hooks) { m_hooks = std::move(hooks); }

    // 五件工具填进 hooks->tools / hooks->toolMeta（AgentBridge::start 合入注册表）
    void registerTools(AgentBridge::Hooks *hooks);

    // 真实后端：stats 缓存 + SftpClient 按需 exec（实现见 sftpclient.cpp，
    // 不进入嵌套事件循环；client 须是独立实例，挂同一 SshSession 即可）
    static HostHooks hooksForStats(HostStats *stats, SftpClient *client);

    // 远端命令输出的结构化解析（公开静态，单测直接喂文本）：
    static QJsonArray parseDf(const QString &text);              // df -P -k
    static QJsonObject parseUptime(const QString &text);         // /proc/uptime + /proc/loadavg
    static QJsonArray parsePs(const QString &text, int maxRows); // ps auxww --sort=-%cpu

private:
    HostHooks m_hooks;
    bool m_execBusy = false;
};

#endif // HOSTBRIDGE_H
