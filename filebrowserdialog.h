#ifndef FILEBROWSERDIALOG_H
#define FILEBROWSERDIALOG_H

#include <QFileIconProvider>

#include "filebrowsermodel.h"
#include "framelessdialog.h"
#include "sftpclient.h"
#include "sshtarget.h"

class PathBar;
class QFileSystemModel;
class QLabel;
class QMenu;
class QModelIndex;
class QSortFilterProxyModel;
class QProcess;
class QProgressBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTreeView;
class SshSession;
class TransferQueue;
struct PendingFetch;

/*
 * FileBrowserDialog: SFTP 双栏文件管理器（WinSCP 风格）。
 *
 * 左栏本地、右栏远程，各自有工具栏和右键菜单。
 * 上次打开的本地/远程目录只记一份，写在
 * AppConfigLocation/filebrowser.json。主机 IP 变化后仍回到该目录。
 * 直接复用终端的 SshSession（同一条 SSH 连接上的共享 SFTP 通道）：
 * 会话断开时本窗口一并关闭；关闭窗口只取消文件请求，shell 不受影响。
 *
 * 组成（见各文件头注释）：
 *  - filebrowsermodel.*   数据与模型工具（条目/命令结构、持久化、上传任务收集）
 *  - transferqueue.*      传输队列网关（入队、编辑回传监视、文件夹批次跟踪）
 *  - filebrowserwidgets.* 支撑控件（路径栏、chmod/属性/命令管理对话框）
 *  - 本文件               UI 编排（工具栏、菜单、导航与操作分发）
 */
class FileBrowserDialog : public FramelessDialog
{
    Q_OBJECT

public:
    explicit FileBrowserDialog(SshSession *session, QWidget *parent = nullptr);
    ~FileBrowserDialog() override;

    // 终端 OSC 8 链接（file:// 本地不存在）触发的单文件取回：走与「编辑」
    // 相同的 fetch 通道（edit=false），下载完成后自动用系统关联程序打开
    void fetchRemoteFile(const QString &remotePath);

private slots:
    void onListed(const QString &path, const QVector<SftpEntry> &entries);
    void onListedFailed(const QString &path, const QString &error);
    void onOpFinished(const QString &op, const QString &arg, bool ok, const QString &error);
    void onTransferProgressInfo(const QString &text, int percent, bool active); // 底栏进度

    void showTransferQueue();
    void onLocalActivated(const QModelIndex &index);
    void onLocalPathChosen();
    void onLocalUp();
    void onRemoteItemActivated(QTreeWidgetItem *item, int column);
    void onRemotePathChosen();
    void onRemoteUp();
    void updateActionStates();

private:
    // TransferQueue 的 fetchFinished：下载完成后的打开/编辑登记（弹窗、openUrl）
    void onFetchFinished(const PendingFetch &job, bool ok, const QString &error);

    QToolButton *makePaneButton(const QString &iconName, const QString &text);
    QToolButton *makeNewButton(bool remote);

    void restoreBrowserState();
    void persistBrowserState() const;
    QString sessionKey() const;

    void onLocalContextMenu(const QPoint &pos);
    void onRemoteContextMenu(const QPoint &pos);
    void fillCommandMenu(QMenu *menu, bool remoteSide);

    void uploadSelected();
    void downloadSelected();
    void deleteLocal();
    void deleteRemote();
    void renameLocal();
    void renameRemote();
    void editLocal();
    void editRemote();
    void openLocal();
    void openRemote();
    void copyRemoteSelected();
    void moveRemoteSelected();
    void propsLocal();
    void propsRemote();
    void newLocalDir();
    void newLocalFile();
    void newRemoteDir();
    void newRemoteFile();
    void refreshLocal();
    void refreshRemote();
    void manageCommands();
    void runCustomCommand(int index);
    void showSystemMenu();
    void copyLocalNames(bool fullPath);
    void copyRemoteNames(bool fullPath, bool asUrl);

    void beginCommandBatch();
    void queueRemoteCommand(const QString &command);
    void endCommandBatch();
    void flushExecUi();
    void handleExecFinished(bool ok, const QString &command, const QString &detail);
    void enqueueLocalCommand(const QString &command);
    void pumpLocalCommand();

    bool askName(const QString &title, const QString &initial, QString *out);
    bool askRemoteDest(const QString &title, const QString &defaultName, bool editName,
                       QString *dirOut, QString *nameOut);
    bool remoteNameExists(const QString &name) const;
    // 视图（经排序代理）索引 → QFileSystemModel 源索引
    QModelIndex mapToSource(const QModelIndex &viewIndex) const;

    void setLocalPath(const QString &path);
    void setRemotePath(const QString &path);
    void syncLocalBar();
    void syncRemoteBar();
    QStringList selectedLocalPaths() const;
    QVector<RemoteItem> selectedRemote() const;

    SshSession *m_session = nullptr;   // 共享的终端会话，不拥有
    SshTarget m_target;                // 会话目标的副本，仅用于标题/路径键/URL
    SftpClient *m_client = nullptr;    // 会话之上的 GUI 适配器（parent=this）
    TransferQueue *m_queue = nullptr;  // 传输队列网关（parent=this，内部持有队列窗口）

    QFileSystemModel *m_fsModel = nullptr;
    QSortFilterProxyModel *m_fsProxy = nullptr; // 本地列表排序：目录置顶 + 字母序
    QTreeView *m_localView = nullptr;
    QTreeWidget *m_remoteTree = nullptr;
    PathBar *m_localPathBar = nullptr;
    PathBar *m_remotePathBar = nullptr;
    QToolButton *m_localUpload = nullptr;
    QToolButton *m_localEdit = nullptr;
    QToolButton *m_localDelete = nullptr;
    QToolButton *m_localRename = nullptr;
    QToolButton *m_localProps = nullptr;
    QToolButton *m_remoteDownload = nullptr;
    QToolButton *m_remoteEdit = nullptr;
    QToolButton *m_remoteDelete = nullptr;
    QToolButton *m_remoteRename = nullptr;
    QToolButton *m_remoteProps = nullptr;
    QToolButton *m_remoteNew = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_transferText = nullptr;    // 底栏传输进度文案（队列空时为空）
    QProgressBar *m_transferBar = nullptr; // 底栏传输进度条，仅活动传输时可见

    bool m_connected = false;
    bool m_remoteRestorePending = false;
    QString m_remotePath;
    QString m_localPath;
    QString m_homePath;
    QString m_persistedRemote; // 最近一次列目录成功的远程路径，写入配置
    QString m_savedRemote;     // 启动时读到的上次远程路径
    QFileIconProvider m_iconProvider;

    QList<FileCommand> m_commands;
    QProcess *m_localProc = nullptr;
    QStringList m_localCmdQueue;
    QString m_localLog;
    bool m_localFailed = false;
    int m_execLeft = 0;
    bool m_execBatching = false;
    bool m_execFailed = false;
    bool m_execRemote = false;
    QString m_execLog;
    QString m_localOutput;
    QString m_localRunningCmd;
    QPoint m_shellMenuPos;
};

#endif // FILEBROWSERDIALOG_H
