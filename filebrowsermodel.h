#ifndef FILEBROWSERMODEL_H
#define FILEBROWSERMODEL_H

#include <QByteArray>
#include <QList>
#include <QModelIndex>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QVector>

#include <functional>

/*
 * FileBrowserModel: SFTP 文件管理器的数据与模型工具（无窗口依赖）。
 *
 * 从 FileBrowserDialog 拆出的纯数据部分：远端条目与自定义命令的结构、
 * 权限/大小格式化、路径拼接、窗口状态持久化（filebrowser.json）、
 * 文件夹上传的任务收集，以及本地列表的排序/呈现规则。
 */

// 远端目录列表的一个条目（SftpEntry 的界面侧视图）
struct RemoteItem
{
    QString name;
    bool isDir = false;
    bool isLink = false;
    quint32 permissions = 0;
    quint64 size = 0;
    qint64 mtime = 0;
};

// 文件自定义命令。! 当前文件，!& 全部选中，!/ 当前目录。
struct FileCommand
{
    QString name;
    QString command;
    bool remote = true;
};

// 远端条目在 QTreeWidgetItem 上携带的数据角色
constexpr int kRoleDir = Qt::UserRole;
constexpr int kRoleLink = Qt::UserRole + 1;
constexpr int kRoleSize = Qt::UserRole + 2;
constexpr int kRolePerm = Qt::UserRole + 3;
constexpr int kRoleMtime = Qt::UserRole + 4;

// ---- 纯工具函数 ----

QString permString(quint32 perm, bool isDir, bool isLink);
QString humanSize(quint64 size);
bool validFileName(const QString &name);
QByteArray fileDigest(const QString &path);
QString shellQuote(const QString &text);
QString cmdQuote(const QString &text);
bool commandUsesEachFile(const QString &command);
bool commandNeedsSelection(const QString &command);
QString remoteJoin(const QString &base, const QString &name);
QString localJoin(const QString &base, const QString &name);

// ---- 窗口状态持久化（AppConfigLocation/filebrowser.json）----

struct BrowserState
{
    QString local;                // 上次本地目录（空 = 停在「此电脑」，不覆盖旧值）
    QString remote;               // 最近一次列目录成功的远程路径
    QList<FileCommand> commands;  // 文件自定义命令
};

QString browserStateFilePath();
// legacySessionKey：旧版按 user@host:port 分条存储，读不到新字段时用它合并已有记录
BrowserState loadBrowserState(const QString &legacySessionKey);
void saveBrowserState(const BrowserState &state);

// ---- 文件夹上传任务收集（纯数据；覆盖/合并确认弹窗留给调用方）----

struct UploadTask
{
    QString local;
    QString remote;
    qint64 size = 0;
};

struct UploadDirTask
{
    QString local;
    QString remote;
};

struct UploadBatch
{
    QVector<UploadTask> files;
    QVector<UploadDirTask> dirs;   // 远程要新建的目录（按 remote 字典序，父目录天然在前）
    QStringList conflicts;         // 远程已存在的顶层目录名，合并前需确认
    int skippedLinks = 0;          // 跳过的符号链接/junction 数（可能成环，不递归）
};

UploadBatch collectUploadTasks(const QStringList &localPaths, const QString &remoteBase,
                               const std::function<bool(const QString &)> &remoteNameExists);

// ---- 本地列表呈现规则 ----

// 本地列表「大小」列右对齐（与远程端一致）；QFileSystemModel 默认左对齐
class SizeColumnDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override;
};

// 本地列表排序：目录永远在文件前面，名称不区分大小写按字母序。
// QFileSystemModel 的默认枚举序两者都不保证（NTFS 是纯字母混排），
// 列头也不参与排序——顺序恒定可预期。
class LocalSortProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
};

#endif // FILEBROWSERMODEL_H
