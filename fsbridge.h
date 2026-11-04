#ifndef FSBRIDGE_H
#define FSBRIDGE_H

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

#include "agentbridge.h"     // ToolHandler / Hooks（工具注册表）
#include "ssh/sftprequest.h" // SftpEntry

class SftpClient;

/*
 * FsBridge: AgentBridge 的 xterm.fs.* 工具后端（M2.b）。
 *
 * 职责：
 * - 六件 FS 工具（stat / list / read / write / download / upload）注册进
 *   AgentBridge::Hooks.tools；bridge 只见 ToolHandler，语义与安全都在这里
 * - 安全边界：路径沙箱（只放行会话根目录及子路径，词法归一防 .. 逃逸）
 *   + allow_write 开关（write / upload 默认拒绝，需配置显式开启）
 *
 * 后端注入：FsHooks 是纯函数集——真实实现 hooksForSftp()（见 sftpclient.cpp，
 * 把 SftpClient 的异步信号桥接成同步）由主窗口挂上；测试注入内存 fake，
 * read/write 走真实本地临时文件，桥接逻辑同样被覆盖。
 * 沙箱根在 SSH 连接后由 refreshRoot() 用 realPath(".") 解析，断开即 clearRoot。
 */
class FsBridge
{
public:
    // FS 后端能力，全部同步语义。err 非空 = 失败（其余返回值无效）。
    struct FsHooks
    {
        // 解析为真实绝对路径（SFTP realpath；"." → 会话主目录）
        std::function<QString(const QString &path, QString *err)> realPath;
        // 列目录条目（不存在/非目录经 *err 报错）
        std::function<QVector<SftpEntry>(const QString &path, QString *err)> list;
        // 远端 → 本地（覆盖写本地文件）
        std::function<bool(const QString &remote, const QString &local, QString *err)> download;
        // 本地 → 远端（覆盖写远端文件）
        std::function<bool(const QString &local, const QString &remote, QString *err)> upload;
    };

    FsBridge() = default;

    void setBackend(FsHooks hooks) { m_hooks = std::move(hooks); }
    void setAllowWrite(bool allow) { m_allowWrite = allow; }
    bool allowWrite() const { return m_allowWrite; }

    // 沙箱根：SSH 连接后调 refreshRoot（同步 realPath "."），失败/断开调 clearRoot
    bool refreshRoot();
    void clearRoot() { m_root.clear(); }
    QString root() const { return m_root; }

    // 六件工具填进 hooks->tools / hooks->toolMeta（AgentBridge::start 合入注册表）
    void registerTools(AgentBridge::Hooks *hooks);

    // 真实后端：SftpClient 异步信号 → 同步 FsHooks（实现见 sftpclient.cpp，
    // 避免本文件拖进整条 SSH 依赖链）。client 须是独立实例（挂同一 SshSession
    // 即可，SftpClient 支持同会话多适配器）
    static FsHooks hooksForSftp(SftpClient *client);

    // 词法归一化绝对 POSIX 路径：合并分隔符、消解 . 与 ..（不触文件系统）。
    // 相对路径先由调用方拼到根上再进来；根之上的 .. 消解到根为止
    static QString normalizePosix(const QString &absPath);

private:
    // 沙箱判定：input（绝对或相对根）解析成沙箱内绝对路径；失败时 *err 给原因
    bool resolveInside(const QString &input, QString *abs, QString *err) const;
    // read 内部：download 到临时文件，限长读回文本；err/size 出参
    QString readRemoteText(const QString &abs, qint64 maxBytes, QString *err, qint64 *size) const;

    FsHooks m_hooks;
    QString m_root; // 沙箱根（归一化绝对路径；空 = 无 SSH 会话）
    bool m_allowWrite = false;
};

#endif // FSBRIDGE_H
