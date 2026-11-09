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
 * - 安全边界：路径沙箱（词法归一后再 realpath，真实路径必须仍在会话根内，
 *   挡住符号链接逃逸）+ allow_write 开关（write / upload 默认拒绝）
 * - 同一时刻只跑一个文件操作。进行中的第二次调用直接拒绝，避免在界面线程
 *   里嵌套事件循环
 *
 * 后端注入：FsHooks 是回调集。测试 fake 在调用返回前就回调；真实实现
 * hooksForSftp() 等 SftpClient 的信号，不在界面线程上空等。
 * 沙箱根在 SSH 连接后由 refreshRoot() 用 realPath(".") 解析，断开即 clearRoot。
 */
class FsBridge
{
public:
    // FS 后端。done 可能在本次调用内触发，也可能稍后在界面线程触发。
    // err 非空 = 失败（其余结果无效）。
    struct FsHooks
    {
        // 解析为真实绝对路径（SFTP realpath；"." → 会话主目录）。
        // 不存在时 resolved 为空并给出 err。
        std::function<void(const QString &path,
                           const std::function<void(const QString &resolved,
                                                    const QString &err)> &done)>
            realPath;
        std::function<void(const QString &path,
                           const std::function<void(const QVector<SftpEntry> &entries,
                                                    const QString &err)> &done)>
            list;
        std::function<void(const QString &remote, const QString &local,
                           const std::function<void(bool ok, const QString &err)> &done)>
            download;
        std::function<void(const QString &local, const QString &remote,
                           const std::function<void(bool ok, const QString &err)> &done)>
            upload;
    };

    FsBridge() = default;

    void setBackend(FsHooks hooks) { m_hooks = std::move(hooks); }
    void setAllowWrite(bool allow) { m_allowWrite = allow; }
    bool allowWrite() const { return m_allowWrite; }

    // 沙箱根：SSH 连接后调 refreshRoot。回调若当场发生，返回时根已就位；
    // 真实链路的 realpath 稍后到达，期间工具报「根未解析」。断开调 clearRoot。
    bool refreshRoot();
    void clearRoot();
    QString root() const { return m_root; }

    // 六件工具填进 hooks->tools / hooks->toolMeta（AgentBridge::start 合入注册表）
    void registerTools(AgentBridge::Hooks *hooks);

    // 真实后端：SftpClient 信号 → 回调（实现见 sftpclient.cpp，
    // 不进入嵌套事件循环）。client 须是独立实例（挂同一 SshSession 即可）
    static FsHooks hooksForSftp(SftpClient *client);

    // 词法归一化绝对 POSIX 路径：合并分隔符、消解 . 与 ..（不触文件系统）。
    // 相对路径先由调用方拼到根上再进来；根之上的 .. 消解到根为止
    static QString normalizePosix(const QString &absPath);

private:
    // 沙箱判定：input（绝对或相对根）解析成沙箱内的词法绝对路径
    bool resolveInside(const QString &input, QString *abs, QString *err) const;
    bool insideRoot(const QString &norm) const;
    QString outsideError(const QString &norm) const;
    // 词法路径 → 真实路径。allowCreate 时目标不存在则改核父目录，
    // 成功路径是「真实父目录 + 文件名」。done 的 err 非空即失败。
    void canonicalize(const QString &lexical, bool allowCreate,
                      const std::function<void(const QString &path, const QString &err)> &done) const;

    FsHooks m_hooks;
    QString m_root; // 沙箱根（归一化绝对路径；空 = 无 SSH 会话）
    int m_rootGen = 0; // clear/refresh 递增，丢弃过期的 realpath 回调
    bool m_allowWrite = false;
    bool m_busy = false; // 一个文件操作进行中
};

#endif // FSBRIDGE_H
