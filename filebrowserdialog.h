#ifndef FILEBROWSERDIALOG_H
#define FILEBROWSERDIALOG_H

#include <QByteArray>
#include <QDialog>
#include <QFileIconProvider>
#include <QHash>

#include "sftpclient.h"
#include "sshtarget.h"

class PathBar;
class QFileSystemModel;
class QFileSystemWatcher;
class QLabel;
class QMenu;
class QModelIndex;
class QProcess;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTreeView;
class SshSession;
class TransferDialog;

// 文件自定义命令。! 当前文件，!& 全部选中，!/ 当前目录。
struct FileCommand
{
    QString name;
    QString command;
    bool remote = true;
};

/*
 * FileBrowserDialog: SFTP 双栏文件管理器（WinSCP 风格）。
 *
 * 左栏本地、右栏远程，各自有工具栏和右键菜单。
 * 上次打开的本地/远程目录只记一份，写在
 * AppConfigLocation/filebrowser.json。主机 IP 变化后仍回到该目录。
 * 直接复用终端的 SshSession（同一条 SSH 连接上的共享 SFTP 通道）：
 * 会话断开时本窗口一并关闭；关闭窗口只取消文件请求，shell 不受影响。
 */
class FileBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FileBrowserDialog(SshSession *session, QWidget *parent = nullptr);
    ~FileBrowserDialog() override;

private slots:
    void onListed(const QString &path, const QVector<SftpEntry> &entries);
    void onListedFailed(const QString &path, const QString &error);
    void onOpFinished(const QString &op, const QString &arg, bool ok, const QString &error);
    void onTransferFinished(int id, bool ok, const QString &error);

    void showTransferQueue();
    void onLocalActivated(const QModelIndex &index);
    void onLocalPathChosen();
    void onLocalUp();
    void onRemoteItemActivated(QTreeWidgetItem *item, int column);
    void onRemotePathChosen();
    void onRemoteUp();
    void updateActionStates();

private:
    struct RemoteItem
    {
        QString name;
        bool isDir = false;
        bool isLink = false;
        quint32 permissions = 0;
        quint64 size = 0;
        qint64 mtime = 0;
    };
    struct PendingFetch
    {
        bool edit = false;       // 下载后打开编辑器并监视回传
        bool uploadBack = false; // 这次传输是编辑回传
        QString local;
        QString remote;
        QByteArray previousDigest;
    };

    QToolButton *makePaneButton(const QString &iconName, const QString &text);
    QToolButton *makeNewButton(bool remote);

    void loadBrowserState();
    void saveBrowserState() const;
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

    void onEditFileChanged(const QString &path);
    void commitEditUpload();
    void enqueueFetch(bool edit, const QString &remotePath, qint64 size);
    QString cacheFile(bool edit, const QString &remotePath) const;
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

    void setLocalPath(const QString &path);
    void setRemotePath(const QString &path);
    void syncLocalBar();
    void syncRemoteBar();
    QString remoteJoin(const QString &base, const QString &name) const;
    static QString localJoin(const QString &base, const QString &name);
    static QString permString(quint32 perm, bool isDir, bool isLink);
    QStringList selectedLocalPaths() const;
    QVector<RemoteItem> selectedRemote() const;

    SshSession *m_session = nullptr;   // 共享的终端会话，不拥有
    SshTarget m_target;                // 会话目标的副本，仅用于标题/路径键/URL
    SftpClient *m_client = nullptr;    // 会话之上的 GUI 适配器（parent=this）
    TransferDialog *m_transfer = nullptr;

    QFileSystemModel *m_fsModel = nullptr;
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

    bool m_connected = false;
    bool m_remoteRestorePending = false;
    QString m_remotePath;
    QString m_localPath;
    QString m_homePath;
    QString m_persistedRemote; // 最近一次列目录成功的远程路径，写入配置
    QString m_savedRemote;     // 启动时读到的上次远程路径
    QFileIconProvider m_iconProvider;

    QList<FileCommand> m_commands;
    QFileSystemWatcher *m_editWatcher = nullptr;
    QTimer *m_editTimer = nullptr;
    QString m_editTimerPath;
    QHash<QString, QString> m_watchedEdit;          // 本地缓存 -> 远程路径
    QHash<QString, QByteArray> m_editDigest;        // 上次已同步内容
    QHash<int, PendingFetch> m_pendingFetch;

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
