#include "filebrowsermodel.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

QString browserStateFilePath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(base).filePath(QStringLiteral("filebrowser.json"));
}

QString shellQuote(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString cmdQuote(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

bool validFileName(const QString &name)
{
    return !name.isEmpty()
        && name != QLatin1String(".")
        && name != QLatin1String("..")
        && !name.contains(QLatin1Char('/'))
        && !name.contains(QLatin1Char('\\'));
}

bool commandUsesEachFile(const QString &command)
{
    for (int i = 0; i < command.size(); ++i) {
        if (command.at(i) != QLatin1Char('!'))
            continue;
        const QChar next = (i + 1 < command.size()) ? command.at(i + 1) : QChar();
        if (next == QLatin1Char('&') || next == QLatin1Char('/'))
            continue;
        return true;
    }
    return false;
}

bool commandNeedsSelection(const QString &command)
{
    return command.contains(QLatin1String("!&")) || commandUsesEachFile(command);
}

QByteArray fileDigest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return hash.result();
}

QString humanSize(quint64 size)
{
    return QLocale().formattedDataSize(qint64(size));
}

QString permString(quint32 perm, bool isDir, bool isLink)
{
    QString text;
    text += isLink ? QChar::fromLatin1('l') : (isDir ? QChar::fromLatin1('d') : QChar::fromLatin1('-'));
    const quint32 masks[3] = { 0400, 0200, 0100 };
    const char sym[3] = { 'r', 'w', 'x' };
    for (int group = 0; group < 3; ++group) {
        for (int bit = 0; bit < 3; ++bit)
            text += QChar::fromLatin1((perm & (masks[bit] >> (group * 3))) ? sym[bit] : '-');
    }
    return text;
}

QString remoteJoin(const QString &base, const QString &name)
{
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

QString localJoin(const QString &base, const QString &name)
{
    return QDir(base).filePath(name);
}


BrowserState loadBrowserState(const QString &legacySessionKey)
{
    BrowserState state;
    QFile file(browserStateFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return state;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return state;
    const QJsonObject root = doc.object();
    state.local = root.value(QStringLiteral("local")).toString();
    state.remote = root.value(QStringLiteral("remote")).toString();
    // 旧文件按 user@host:port 分条。IP 会变，读不到新字段时把已有记录并成一份。
    if (state.local.isEmpty() && state.remote.isEmpty()) {
        const QJsonObject paths = root.value(QStringLiteral("paths")).toObject();
        const QJsonObject current = paths.value(legacySessionKey).toObject();
        state.local = current.value(QStringLiteral("local")).toString();
        state.remote = current.value(QStringLiteral("remote")).toString();
        for (auto it = paths.constBegin();
             it != paths.constEnd() && (state.local.isEmpty() || state.remote.isEmpty()); ++it) {
            const QJsonObject one = it.value().toObject();
            if (state.local.isEmpty())
                state.local = one.value(QStringLiteral("local")).toString();
            if (state.remote.isEmpty())
                state.remote = one.value(QStringLiteral("remote")).toString();
        }
    }
    const QJsonArray commands = root.value(QStringLiteral("commands")).toArray();
    for (const QJsonValue &value : commands) {
        const QJsonObject obj = value.toObject();
        FileCommand command;
        command.name = obj.value(QStringLiteral("name")).toString();
        command.command = obj.value(QStringLiteral("command")).toString();
        command.remote = obj.value(QStringLiteral("remote")).toBool(true);
        if (!command.name.isEmpty() || !command.command.isEmpty())
            state.commands.append(command);
    }
    return state;
}


void saveBrowserState(const BrowserState &state)
{
    const QString path = browserStateFilePath();
    QJsonObject root;
    {
        // 读完即关。句柄还开着时，Windows 上 QSaveFile 替换会失败，目录就写不进去。
        QFile in(path);
        if (in.open(QIODevice::ReadOnly)) {
            const QJsonDocument existing = QJsonDocument::fromJson(in.readAll());
            if (existing.isObject())
                root = existing.object();
        }
    }
    root.remove(QStringLiteral("paths"));
    // 停在「此电脑」时 local 为空，不要把上次的本地目录清掉。
    if (!state.local.isEmpty())
        root.insert(QStringLiteral("local"), state.local);
    if (!state.remote.isEmpty())
        root.insert(QStringLiteral("remote"), state.remote);
    else
        root.remove(QStringLiteral("remote"));

    QJsonArray commands;
    for (const FileCommand &command : state.commands) {
        commands.append(QJsonObject{
            { QStringLiteral("name"), command.name },
            { QStringLiteral("command"), command.command },
            { QStringLiteral("remote"), command.remote },
        });
    }
    root.insert(QStringLiteral("commands"), commands);

    QDir().mkpath(QFileInfo(path).absolutePath());
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    {
        QSaveFile out(path);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(payload);
            if (out.commit())
                return;
        }
    }
    QFile plain(path);
    if (plain.open(QIODevice::WriteOnly | QIODevice::Truncate))
        plain.write(payload);
}

UploadBatch collectUploadTasks(const QStringList &localPaths, const QString &remoteBase,
                               const std::function<bool(const QString &)> &remoteNameExists)
{
    UploadBatch batch;
    for (const QString &path : localPaths) {
        const QFileInfo info(path);
        if (info.isSymLink()) { // 链接/junction 可能成环，直接跳过不递归
            ++batch.skippedLinks;
            continue;
        }
        const QString remoteRoot = remoteJoin(remoteBase, info.fileName());
        if (!info.isDir()) {
            UploadTask task;
            task.local = path;
            task.remote = remoteRoot;
            task.size = info.size();
            batch.files.append(task);
            continue;
        }
        if (remoteNameExists(info.fileName()))
            batch.conflicts << info.fileName();
        // 递归收集子项：目录进 dirs、文件进 files，远程路径保持同样的相对结构
        batch.dirs.append(UploadDirTask{path, remoteRoot});
        const QDir root(path);
        QDirIterator it(path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo entry = it.fileInfo();
            if (entry.isSymLink()) {
                ++batch.skippedLinks;
                continue;
            }
            const QString remote = remoteRoot + QLatin1Char('/')
                + root.relativeFilePath(entry.absoluteFilePath());
            if (entry.isDir()) {
                batch.dirs.append(UploadDirTask{entry.absoluteFilePath(), remote});
                continue;
            }
            UploadTask task;
            task.local = entry.absoluteFilePath();
            task.remote = remote;
            task.size = entry.size();
            batch.files.append(task);
        }
    }
    // 字典序里父目录是子目录的前缀，天然父先子后
    std::sort(batch.dirs.begin(), batch.dirs.end(),
              [](const UploadDirTask &a, const UploadDirTask &b) { return a.remote < b.remote; });
    return batch;
}

void SizeColumnDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    option->displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
}

bool LocalSortProxy::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    auto *fs = static_cast<QFileSystemModel *>(sourceModel());
    const bool leftDir = fs->isDir(left);
    const bool rightDir = fs->isDir(right);
    if (leftDir != rightDir)
        return leftDir;
    return left.data().toString().compare(right.data().toString(),
                                          Qt::CaseInsensitive) < 0;
}
