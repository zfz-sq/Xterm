#include "fsbridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryFile>

#include <memory>

namespace {

// read 的默认/上限：AI 读配置/日志够用；巨文件应走 download
constexpr qint64 kDefaultReadBytes = 64 * 1024;
constexpr qint64 kMaxReadBytes = 1024 * 1024;

// stat = 列父目录找 basename。根目录的父是它自己（条目为空 = 存在且是目录）
void splitParentBase(const QString &abs, QString *parent, QString *base)
{
    if (abs == QLatin1String("/")) {
        *parent = QStringLiteral("/");
        *base = QString();
        return;
    }
    const int slash = abs.lastIndexOf(QLatin1Char('/'));
    *base = abs.mid(slash + 1);
    *parent = slash == 0 ? QStringLiteral("/") : abs.left(slash);
}

QJsonObject entryToJson(const SftpEntry &e)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), e.name);
    o.insert(QStringLiteral("is_dir"), e.isDir);
    o.insert(QStringLiteral("is_symlink"), e.isSymlink);
    o.insert(QStringLiteral("size"), double(e.size));
    o.insert(QStringLiteral("mtime"), double(e.mtime));
    o.insert(QStringLiteral("permissions"), double(e.permissions));
    return o;
}

// 操作结束（含同步回调）时清忙标志。由最外层和最内层回调共同持有。
struct BusyFlag
{
    bool *slot = nullptr;
    ~BusyFlag()
    {
        if (slot)
            *slot = false;
    }
};

} // namespace

QString FsBridge::normalizePosix(const QString &absPath)
{
    QStringList out;
    const QStringList parts = absPath.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &p : parts) {
        if (p.isEmpty() || p == QLatin1String("."))
            continue;
        if (p == QLatin1String("..")) {
            if (!out.isEmpty()) // 根之上的 .. 消解到根为止，不越界
                out.removeLast();
            continue;
        }
        out.append(p);
    }
    return QStringLiteral("/") + out.join(QLatin1Char('/'));
}

bool FsBridge::refreshRoot()
{
    m_root.clear();
    if (!m_hooks.realPath)
        return false;
    const int gen = ++m_rootGen;
    m_hooks.realPath(QStringLiteral("."), [this, gen](const QString &resolved, const QString &) {
        if (gen != m_rootGen)
            return;
        if (resolved.isEmpty() || !resolved.startsWith(QLatin1Char('/')))
            return;
        m_root = normalizePosix(resolved);
    });
    return !m_root.isEmpty();
}

void FsBridge::clearRoot()
{
    m_root.clear();
    ++m_rootGen; // 让尚未回来的 refresh realpath 作废
}

bool FsBridge::insideRoot(const QString &norm) const
{
    if (m_root == QLatin1String("/"))
        return norm.startsWith(QLatin1Char('/'));
    return norm == m_root || norm.startsWith(m_root + QLatin1Char('/'));
}

QString FsBridge::outsideError(const QString &norm) const
{
    return QStringLiteral("路径越界: %1 不在会话根目录 %2 内（沙箱限制）").arg(norm, m_root);
}

bool FsBridge::resolveInside(const QString &input, QString *abs, QString *err) const
{
    if (m_root.isEmpty()) {
        *err = QStringLiteral("SSH 未连接（或会话根目录未解析），FS 工具不可用");
        return false;
    }
    const QString joined = input.startsWith(QLatin1Char('/'))
                               ? input
                               : m_root + QLatin1Char('/') + input;
    const QString norm = normalizePosix(joined);
    if (!insideRoot(norm)) {
        *err = outsideError(norm);
        return false;
    }
    *abs = norm;
    return true;
}

void FsBridge::canonicalize(const QString &lexical, bool allowCreate,
                            const std::function<void(const QString &path, const QString &err)> &done) const
{
    if (!m_hooks.realPath) {
        done({}, QStringLiteral("FS 后端未挂载"));
        return;
    }
    m_hooks.realPath(lexical, [this, lexical, allowCreate, done](const QString &resolved,
                                                                 const QString &err) {
        if (!resolved.isEmpty()) {
            const QString norm = normalizePosix(resolved);
            if (!insideRoot(norm))
                done({}, outsideError(norm));
            else
                done(norm, {});
            return;
        }
        if (!allowCreate) {
            done({}, err.isEmpty() ? QStringLiteral("路径不存在: %1").arg(lexical) : err);
            return;
        }
        QString parent, base;
        splitParentBase(lexical, &parent, &base);
        if (base.isEmpty()) {
            done({}, err.isEmpty() ? QStringLiteral("路径不存在: %1").arg(lexical) : err);
            return;
        }
        m_hooks.realPath(parent, [this, parent, base, done](const QString &pres, const QString &perr) {
            if (pres.isEmpty()) {
                done({}, perr.isEmpty() ? QStringLiteral("父目录不存在: %1").arg(parent) : perr);
                return;
            }
            const QString pnorm = normalizePosix(pres);
            if (!insideRoot(pnorm)) {
                done({}, outsideError(pnorm));
                return;
            }
            const QString joined = (pnorm == QLatin1String("/"))
                                       ? QStringLiteral("/") + base
                                       : pnorm + QLatin1Char('/') + base;
            done(normalizePosix(joined), {});
        });
    });
}

void FsBridge::registerTools(AgentBridge::Hooks *hooks)
{
    auto meta = [hooks](const char *name, const QString &desc, QJsonObject params) {
        const QString full = QString::fromLatin1(name);
        QJsonObject m;
        m.insert(QStringLiteral("name"), full);
        m.insert(QStringLiteral("description"), desc);
        m.insert(QStringLiteral("params"), params);
        hooks->toolMeta.insert(full, m);
    };

    auto takeBusy = [this](const AgentBridge::ToolCall &call, QString *err,
                           AgentBridge::ToolCompletion *completion,
                           std::shared_ptr<BusyFlag> *busy) -> bool {
        if (m_busy) {
            *err = QStringLiteral("已有文件操作进行中，请稍后重试");
            return false;
        }
        m_busy = true;
        *busy = std::shared_ptr<BusyFlag>(new BusyFlag{&m_busy});
        *completion = AgentBridge::ToolCompletion(call);
        return true;
    };

    // —— xterm.fs.stat：存在性 + 元数据。列父目录，不跟随最后一级符号链接 ——
    hooks->tools.insert(QStringLiteral("xterm.fs.stat"),
                        [this, takeBusy](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            QString abs, err;
                            if (!resolveInside(call.params.value(QStringLiteral("path")).toString(),
                                               &abs, &err))
                                return {false, false, {}, err};
                            QString parent, base;
                            splitParentBase(abs, &parent, &base);
                            if (base.isEmpty()) { // 会话根本身：目录必然存在
                                QJsonObject data;
                                data.insert(QStringLiteral("path"), abs);
                                data.insert(QStringLiteral("exists"), true);
                                data.insert(QStringLiteral("is_dir"), true);
                                return {true, false, data, QString()};
                            }
                            if (!m_hooks.list)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            AgentBridge::ToolCompletion completion;
                            std::shared_ptr<BusyFlag> busy;
                            if (!takeBusy(call, &err, &completion, &busy))
                                return {false, false, {}, err};
                            canonicalize(parent, false,
                                         [this, busy, completion, base, abs](const QString &canonParent,
                                                                             const QString &canonErr) {
                                             if (!canonErr.isEmpty()) {
                                                 completion.finish(false, {}, canonErr);
                                                 return;
                                             }
                                             m_hooks.list(canonParent,
                                                          [busy, completion, base, abs](
                                                              const QVector<SftpEntry> &entries,
                                                              const QString &listErr) {
                                                              if (!listErr.isEmpty()) {
                                                                  completion.finish(false, {}, listErr);
                                                                  return;
                                                              }
                                                              QJsonObject data;
                                                              data.insert(QStringLiteral("path"), abs);
                                                              for (const SftpEntry &e : entries) {
                                                                  if (e.name != base)
                                                                      continue;
                                                                  data = entryToJson(e);
                                                                  data.insert(QStringLiteral("path"), abs);
                                                                  data.insert(QStringLiteral("exists"), true);
                                                                  completion.finish(true, data, {});
                                                                  return;
                                                              }
                                                              data.insert(QStringLiteral("exists"), false);
                                                              completion.finish(true, data, {});
                                                          });
                                         });
                            return completion.resultOrDefer();
                        });
    meta("xterm.fs.stat",
         QStringLiteral("查远端文件/目录的元数据（大小/类型/权限/mtime）；不存在时 exists=false"),
         [] {
             QJsonObject params;
             QJsonObject path;
             path.insert(QStringLiteral("type"), QStringLiteral("string"));
             path.insert(QStringLiteral("description"),
                         QStringLiteral("远端路径（相对=会话根目录；沙箱内）"));
             params.insert(QStringLiteral("path"), path);
             return params;
         }());

    // —— xterm.fs.list：列目录（先 realpath，符号链接指向根外则拒绝）——
    hooks->tools.insert(QStringLiteral("xterm.fs.list"),
                        [this, takeBusy](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            QString abs, err;
                            if (!resolveInside(call.params.value(QStringLiteral("path")).toString(),
                                               &abs, &err))
                                return {false, false, {}, err};
                            if (!m_hooks.list)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            AgentBridge::ToolCompletion completion;
                            std::shared_ptr<BusyFlag> busy;
                            if (!takeBusy(call, &err, &completion, &busy))
                                return {false, false, {}, err};
                            canonicalize(abs, false, [this, busy, completion](const QString &canon,
                                                                              const QString &canonErr) {
                                if (!canonErr.isEmpty()) {
                                    completion.finish(false, {}, canonErr);
                                    return;
                                }
                                m_hooks.list(canon, [busy, completion, canon](const QVector<SftpEntry> &entries,
                                                                              const QString &listErr) {
                                    if (!listErr.isEmpty()) {
                                        completion.finish(false, {}, listErr);
                                        return;
                                    }
                                    QJsonArray arr;
                                    for (const SftpEntry &e : entries)
                                        arr.append(entryToJson(e));
                                    QJsonObject data;
                                    data.insert(QStringLiteral("path"), canon);
                                    data.insert(QStringLiteral("entries"), arr);
                                    completion.finish(true, data, {});
                                });
                            });
                            return completion.resultOrDefer();
                        });
    meta("xterm.fs.list", QStringLiteral("列远端目录条目（name/is_dir/size/mtime/权限）"),
         [] {
             QJsonObject params;
             QJsonObject path;
             path.insert(QStringLiteral("type"), QStringLiteral("string"));
             path.insert(QStringLiteral("description"),
                         QStringLiteral("远端目录（默认会话根目录；沙箱内）"));
             params.insert(QStringLiteral("path"), path);
             return params;
         }());

    // —— xterm.fs.read：realpath 后 download → 临时文件 → 限长读回 ——
    hooks->tools.insert(QStringLiteral("xterm.fs.read"),
                        [this, takeBusy](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            QString abs, err;
                            if (!resolveInside(call.params.value(QStringLiteral("path")).toString(),
                                               &abs, &err))
                                return {false, false, {}, err};
                            if (!m_hooks.download)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            const qint64 maxBytes = qBound<qint64>(
                                1, call.params.value(QStringLiteral("max_bytes")).toDouble(kDefaultReadBytes),
                                kMaxReadBytes);
                            AgentBridge::ToolCompletion completion;
                            std::shared_ptr<BusyFlag> busy;
                            if (!takeBusy(call, &err, &completion, &busy))
                                return {false, false, {}, err};
                            canonicalize(abs, false, [this, busy, completion, maxBytes](const QString &canon,
                                                                                        const QString &canonErr) {
                                if (!canonErr.isEmpty()) {
                                    completion.finish(false, {}, canonErr);
                                    return;
                                }
                                auto tmp = std::make_shared<QTemporaryFile>(
                                    QDir::temp().filePath(QStringLiteral("xterm-fs-read-XXXXXX")));
                                tmp->setAutoRemove(true);
                                if (!tmp->open()) {
                                    completion.finish(false, {}, QStringLiteral("本地临时文件创建失败"));
                                    return;
                                }
                                tmp->close();
                                const QString localName = tmp->fileName();
                                m_hooks.download(canon, localName,
                                                 [busy, completion, tmp, canon, maxBytes](bool ok,
                                                                                          const QString &dlErr) {
                                                     if (!ok) {
                                                         completion.finish(false, {}, dlErr);
                                                         return;
                                                     }
                                                     QFile f(tmp->fileName());
                                                     if (!f.open(QIODevice::ReadOnly)) {
                                                         completion.finish(false, {},
                                                                           QStringLiteral("读取下载内容失败: %1")
                                                                               .arg(canon));
                                                         return;
                                                     }
                                                     const QByteArray bytes = f.read(maxBytes + 1);
                                                     if (bytes.size() > maxBytes) {
                                                         completion.finish(
                                                             false, {},
                                                             QStringLiteral("文件超过 max_bytes（%1 字节），"
                                                                            "请改用 xterm.fs.download")
                                                                 .arg(QString::number(maxBytes)));
                                                         return;
                                                     }
                                                     QJsonObject data;
                                                     data.insert(QStringLiteral("path"), canon);
                                                     data.insert(QStringLiteral("size"), double(bytes.size()));
                                                     data.insert(QStringLiteral("text"),
                                                                 QString::fromUtf8(bytes));
                                                     completion.finish(true, data, {});
                                                 });
                            });
                            return completion.resultOrDefer();
                        });
    meta("xterm.fs.read",
         QStringLiteral("读远端文本文件内容（默认尾 64KB，上限 1MB；更大用 download）"),
         [] {
             QJsonObject params;
             QJsonObject path;
             path.insert(QStringLiteral("type"), QStringLiteral("string"));
             path.insert(QStringLiteral("description"), QStringLiteral("远端文件路径（沙箱内）"));
             params.insert(QStringLiteral("path"), path);
             QJsonObject maxBytes;
             maxBytes.insert(QStringLiteral("type"), QStringLiteral("integer"));
             maxBytes.insert(QStringLiteral("description"),
                             QStringLiteral("最多读取字节数，默认 65536，上限 1048576"));
             params.insert(QStringLiteral("max_bytes"), maxBytes);
             return params;
         }());

    auto writeDenied = [] {
        return AgentBridge::ToolResult{false, false, {},
                                       QStringLiteral("写操作被拒绝：allow_write 未开启"
                                                      "（在 Xterm 配置 mcpAllowWrite 显式"
                                                      "开启并重启后生效）")};
    };

    hooks->tools.insert(QStringLiteral("xterm.fs.write"),
                        [this, takeBusy, writeDenied](const AgentBridge::ToolCall &call)
                            -> AgentBridge::ToolResult {
                            if (!m_allowWrite)
                                return writeDenied();
                            QString abs, err;
                            if (!resolveInside(call.params.value(QStringLiteral("path")).toString(),
                                               &abs, &err))
                                return {false, false, {}, err};
                            if (!m_hooks.upload)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            const QByteArray content =
                                call.params.value(QStringLiteral("content")).toString().toUtf8();
                            auto tmp = std::make_shared<QTemporaryFile>(
                                QDir::temp().filePath(QStringLiteral("xterm-fs-write-XXXXXX")));
                            tmp->setAutoRemove(true);
                            if (!tmp->open())
                                return {false, false, {}, QStringLiteral("本地临时文件创建失败")};
                            tmp->write(content);
                            tmp->close();
                            AgentBridge::ToolCompletion completion;
                            std::shared_ptr<BusyFlag> busy;
                            if (!takeBusy(call, &err, &completion, &busy))
                                return {false, false, {}, err};
                            const QString localName = tmp->fileName();
                            canonicalize(abs, true, [this, busy, completion, tmp, localName, content](
                                                       const QString &canon, const QString &canonErr) {
                                if (!canonErr.isEmpty()) {
                                    completion.finish(false, {}, canonErr);
                                    return;
                                }
                                m_hooks.upload(localName, canon,
                                               [busy, completion, tmp, canon, content](bool ok,
                                                                                       const QString &upErr) {
                                                   if (!ok) {
                                                       completion.finish(false, {}, upErr);
                                                       return;
                                                   }
                                                   QJsonObject data;
                                                   data.insert(QStringLiteral("path"), canon);
                                                   data.insert(QStringLiteral("size"),
                                                               double(content.size()));
                                                   completion.finish(true, data, {});
                                               });
                            });
                            return completion.resultOrDefer();
                        });
    meta("xterm.fs.write", QStringLiteral("写远端文本文件（整体覆盖；需 allow_write）"),
         [] {
             QJsonObject params;
             QJsonObject path;
             path.insert(QStringLiteral("type"), QStringLiteral("string"));
             path.insert(QStringLiteral("description"), QStringLiteral("远端目标路径（沙箱内）"));
             params.insert(QStringLiteral("path"), path);
             QJsonObject content;
             content.insert(QStringLiteral("type"), QStringLiteral("string"));
             content.insert(QStringLiteral("description"), QStringLiteral("完整文件内容（UTF-8）"));
             params.insert(QStringLiteral("content"), content);
             return params;
         }());

    hooks->tools.insert(QStringLiteral("xterm.fs.download"),
                        [this, takeBusy](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            QString abs, err;
                            if (!resolveInside(
                                    call.params.value(QStringLiteral("remote_path")).toString(),
                                    &abs, &err))
                                return {false, false, {}, err};
                            const QString local =
                                call.params.value(QStringLiteral("local_path")).toString();
                            if (local.isEmpty() || !QFileInfo(local).isAbsolute())
                                return {false, false, {},
                                        QStringLiteral("local_path 需为本地绝对路径")};
                            if (!m_hooks.download)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            AgentBridge::ToolCompletion completion;
                            std::shared_ptr<BusyFlag> busy;
                            if (!takeBusy(call, &err, &completion, &busy))
                                return {false, false, {}, err};
                            canonicalize(abs, false, [this, busy, completion, local](const QString &canon,
                                                                                     const QString &canonErr) {
                                if (!canonErr.isEmpty()) {
                                    completion.finish(false, {}, canonErr);
                                    return;
                                }
                                m_hooks.download(canon, local,
                                                 [busy, completion, canon, local](bool ok, const QString &dlErr) {
                                                     if (!ok) {
                                                         completion.finish(false, {}, dlErr);
                                                         return;
                                                     }
                                                     QJsonObject data;
                                                     data.insert(QStringLiteral("remote"), canon);
                                                     data.insert(QStringLiteral("local"), local);
                                                     completion.finish(true, data, {});
                                                 });
                            });
                            return completion.resultOrDefer();
                        });
    meta("xterm.fs.download", QStringLiteral("下载远端文件到本地（读类，默认可用）"),
         [] {
             QJsonObject params;
             QJsonObject remote;
             remote.insert(QStringLiteral("type"), QStringLiteral("string"));
             remote.insert(QStringLiteral("description"), QStringLiteral("远端文件路径（沙箱内）"));
             params.insert(QStringLiteral("remote_path"), remote);
             QJsonObject local;
             local.insert(QStringLiteral("type"), QStringLiteral("string"));
             local.insert(QStringLiteral("description"), QStringLiteral("本地保存绝对路径"));
             params.insert(QStringLiteral("local_path"), local);
             return params;
         }());

    hooks->tools.insert(QStringLiteral("xterm.fs.upload"),
                        [this, takeBusy, writeDenied](const AgentBridge::ToolCall &call)
                            -> AgentBridge::ToolResult {
                            if (!m_allowWrite)
                                return writeDenied();
                            const QString local =
                                call.params.value(QStringLiteral("local_path")).toString();
                            const QFileInfo info(local);
                            if (local.isEmpty() || !info.isAbsolute() || !info.isFile())
                                return {false, false, {},
                                        QStringLiteral("local_path 需为存在的本地文件绝对路径")};
                            QString abs, err;
                            if (!resolveInside(
                                    call.params.value(QStringLiteral("remote_path")).toString(),
                                    &abs, &err))
                                return {false, false, {}, err};
                            if (!m_hooks.upload)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            AgentBridge::ToolCompletion completion;
                            std::shared_ptr<BusyFlag> busy;
                            if (!takeBusy(call, &err, &completion, &busy))
                                return {false, false, {}, err};
                            canonicalize(abs, true, [this, busy, completion, local](const QString &canon,
                                                                                    const QString &canonErr) {
                                if (!canonErr.isEmpty()) {
                                    completion.finish(false, {}, canonErr);
                                    return;
                                }
                                m_hooks.upload(local, canon,
                                               [busy, completion, canon, local](bool ok, const QString &upErr) {
                                                   if (!ok) {
                                                       completion.finish(false, {}, upErr);
                                                       return;
                                                   }
                                                   QJsonObject data;
                                                   data.insert(QStringLiteral("remote"), canon);
                                                   data.insert(QStringLiteral("local"), local);
                                                   completion.finish(true, data, {});
                                               });
                            });
                            return completion.resultOrDefer();
                        });
    meta("xterm.fs.upload", QStringLiteral("上传本地文件到远端（需 allow_write）"),
         [] {
             QJsonObject params;
             QJsonObject local;
             local.insert(QStringLiteral("type"), QStringLiteral("string"));
             local.insert(QStringLiteral("description"), QStringLiteral("本地文件绝对路径"));
             params.insert(QStringLiteral("local_path"), local);
             QJsonObject remote;
             remote.insert(QStringLiteral("type"), QStringLiteral("string"));
             remote.insert(QStringLiteral("description"),
                           QStringLiteral("远端目标路径（沙箱内，覆盖写）"));
             params.insert(QStringLiteral("remote_path"), remote);
             return params;
         }());
}
