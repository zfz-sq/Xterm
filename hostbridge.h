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
 * 后端注入：HostHooks 是纯函数集——真实实现 hooksForStats()（见
 * sftpclient.cpp）由主窗口挂上；测试注入内存 fake。远端命令都是固定
 * 字符串（top 条数经 qBound 成整数后拼接），无注入面。
 */
class HostBridge
{
public:
    // Host 后端能力，全部同步语义。exec 的 *err 非空 = 命令失败。
    struct HostHooks
    {
        // stats 通道最新缓存帧（未连接/无样本时各 ok=false）
        std::function<HostSnapshot()> snapshot;
        // 按需远端命令，返回 stdout；失败置 *err 并返回空串
        std::function<QString(const QString &command, QString *err)> exec;
    };

    HostBridge() = default;

    void setBackend(HostHooks hooks) { m_hooks = std::move(hooks); }

    // 五件工具填进 hooks->tools / hooks->toolMeta（AgentBridge::start 合入注册表）
    void registerTools(AgentBridge::Hooks *hooks);

    // 真实后端：stats 缓存 + SftpClient 按需 exec（实现见 sftpclient.cpp，
    // 避免本文件拖进 SSH 依赖链；client 须是独立实例，挂同一 SshSession 即可）
    static HostHooks hooksForStats(HostStats *stats, SftpClient *client);

    // 远端命令输出的结构化解析（公开静态，单测直接喂文本）：
    static QJsonArray parseDf(const QString &text);              // df -P -k
    static QJsonObject parseUptime(const QString &text);         // /proc/uptime + /proc/loadavg
    static QJsonArray parsePs(const QString &text, int maxRows); // ps auxww --sort=-%cpu

private:
    HostHooks m_hooks;
};

#endif // HOSTBRIDGE_H
