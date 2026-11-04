#include "fsbridge.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryFile>

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
    QString err;
    const QString resolved = m_hooks.realPath
                                 ? m_hooks.realPath(QStringLiteral("."), &err) : QString();
    if (resolved.isEmpty() || !resolved.startsWith(QLatin1Char('/'))) {
        m_root.clear();
        return false; // 未连接 / 通道未就绪：FS 工具回到「不可用」态
    }
    m_root = normalizePosix(resolved);
    return true;
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
    if (norm != m_root && !norm.startsWith(m_root + QLatin1Char('/'))) {
        *err = QStringLiteral("路径越界: %1 不在会话根目录 %2 内（沙箱限制）").arg(norm, m_root);
        return false;
    }
    *abs = norm;
    return true;
}

QString FsBridge::readRemoteText(const QString &abs, qint64 maxBytes, QString *err,
                                 qint64 *size) const
{
    // download 到本地临时文件再读回：复用既有传输链路，天然带进度与错误语义
    QTemporaryFile tmp(QDir::temp().filePath(QStringLiteral("xterm-fs-read-XXXXXX")));
    tmp.setAutoRemove(true);
    if (!tmp.open()) {
        *err = QStringLiteral("本地临时文件创建失败");
        return QString();
    }
    tmp.close(); // 留出路径给 download 覆盖写
    if (!m_hooks.download || !m_hooks.download(abs, tmp.fileName(), err))
        return QString();
    QFile f(tmp.fileName());
    if (!f.open(QIODevice::ReadOnly)) {
        *err = QStringLiteral("读取下载内容失败: %1").arg(abs);
        return QString();
    }
    const QByteArray bytes = f.read(maxBytes + 1); // 多读 1 字节用于判超限
    if (bytes.size() > maxBytes) {
        *err = QStringLiteral("文件超过 max_bytes（%1 字节），请改用 xterm.fs.download")
                   .arg(QString::number(maxBytes));
        return QString();
    }
    *size = bytes.size();
    return QString::fromUtf8(bytes); // 二进制按 UTF-8 容错解码（无效字节替换）
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

    // —— xterm.fs.stat：存在性 + 元数据（探测不存在也算成功，exists=false）——
    hooks->tools.insert(QStringLiteral("xterm.fs.stat"),
                        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            QString abs, err;
                            if (!resolveInside(call.params.value(QStringLiteral("path")).toString(),
                                               &abs, &err))
                                return {false, false, {}, err};
                            if (!m_hooks.list)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            QString parent, base;
                            splitParentBase(abs, &parent, &base);
                            if (base.isEmpty()) { // 会话根本身：目录必然存在
                                QJsonObject data;
                                data.insert(QStringLiteral("path"), abs);
                                data.insert(QStringLiteral("exists"), true);
                                data.insert(QStringLiteral("is_dir"), true);
                                return {true, false, data, QString()};
                            }
                            const QVector<SftpEntry> entries = m_hooks.list(parent, &err);
                            if (!err.isEmpty())
                                return {false, false, {}, err};
                            QJsonObject data;
                            data.insert(QStringLiteral("path"), abs);
                            for (const SftpEntry &e : entries) {
                                if (e.name != base)
                                    continue;
                                data = entryToJson(e);
                                data.insert(QStringLiteral("path"), abs);
                                data.insert(QStringLiteral("exists"), true);
                                return {true, false, data, QString()};
                            }
                            data.insert(QStringLiteral("exists"), false);
                            return {true, false, data, QString()};
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

    // —— xterm.fs.list：列目录 ——
    hooks->tools.insert(QStringLiteral("xterm.fs.list"),
                        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            QString abs, err;
                            if (!resolveInside(call.params.value(QStringLiteral("path")).toString(),
                                               &abs, &err))
                                return {false, false, {}, err};
                            if (!m_hooks.list)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            const QVector<SftpEntry> entries = m_hooks.list(abs, &err);
                            if (!err.isEmpty())
                                return {false, false, {}, err};
                            QJsonArray arr;
                            for (const SftpEntry &e : entries)
                                arr.append(entryToJson(e));
                            QJsonObject data;
                            data.insert(QStringLiteral("path"), abs);
                            data.insert(QStringLiteral("entries"), arr);
                            return {true, false, data, QString()};
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

    // —— xterm.fs.read：读文本内容（download → 临时文件 → 限长读回）——
    hooks->tools.insert(QStringLiteral("xterm.fs.read"),
                        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            QString abs, err;
                            if (!resolveInside(call.params.value(QStringLiteral("path")).toString(),
                                               &abs, &err))
                                return {false, false, {}, err};
                            const qint64 maxBytes = qBound<qint64>(
                                1, call.params.value(QStringLiteral("max_bytes")).toDouble(kDefaultReadBytes),
                                kMaxReadBytes);
                            qint64 size = 0;
                            const QString text = readRemoteText(abs, maxBytes, &err, &size);
                            if (!err.isEmpty())
                                return {false, false, {}, err};
                            QJsonObject data;
                            data.insert(QStringLiteral("path"), abs);
                            data.insert(QStringLiteral("size"), double(size));
                            data.insert(QStringLiteral("text"), text);
                            return {true, false, data, QString()};
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

    // —— 写类（write / upload）：allow_write 显式开启才放行 ——
    auto writeDenied = [] {
        return AgentBridge::ToolResult{false, false, {},
                                       QStringLiteral("写操作被拒绝：allow_write 未开启"
                                                      "（在 Xterm 配置 mcpAllowWrite 显式"
                                                      "开启并重启后生效）")};
    };

    hooks->tools.insert(QStringLiteral("xterm.fs.write"),
                        [this, writeDenied](const AgentBridge::ToolCall &call)
                            -> AgentBridge::ToolResult {
                            if (!m_allowWrite)
                                return writeDenied();
                            QString abs, err;
                            if (!resolveInside(call.params.value(QStringLiteral("path")).toString(),
                                               &abs, &err))
                                return {false, false, {}, err};
                            const QByteArray content =
                                call.params.value(QStringLiteral("content")).toString().toUtf8();
                            if (!m_hooks.upload)
                                return {false, false, {}, QStringLiteral("FS 后端未挂载")};
                            // 写本地临时文件再 upload：复用传输链路，覆盖写远端
                            QTemporaryFile tmp(
                                QDir::temp().filePath(QStringLiteral("xterm-fs-write-XXXXXX")));
                            tmp.setAutoRemove(true);
                            if (!tmp.open())
                                return {false, false, {}, QStringLiteral("本地临时文件创建失败")};
                            tmp.write(content);
                            tmp.close();
                            if (!m_hooks.upload(tmp.fileName(), abs, &err))
                                return {false, false, {}, err};
                            QJsonObject data;
                            data.insert(QStringLiteral("path"), abs);
                            data.insert(QStringLiteral("size"), double(content.size()));
                            return {true, false, data, QString()};
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
                        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
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
                            if (!m_hooks.download(abs, local, &err))
                                return {false, false, {}, err};
                            QJsonObject data;
                            data.insert(QStringLiteral("remote"), abs);
                            data.insert(QStringLiteral("local"), local);
                            return {true, false, data, QString()};
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
                        [this, writeDenied](const AgentBridge::ToolCall &call)
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
                            if (!m_hooks.upload(local, abs, &err))
                                return {false, false, {}, err};
                            QJsonObject data;
                            data.insert(QStringLiteral("remote"), abs);
                            data.insert(QStringLiteral("local"), local);
                            return {true, false, data, QString()};
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
