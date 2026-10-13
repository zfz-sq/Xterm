#ifndef FILEBROWSERDIALOG_H
#define FILEBROWSERDIALOG_H

#include <QDialog>
#include <QFileIconProvider>

#include "sftpclient.h"
#include "sshtarget.h"

class QComboBox;
class QFileSystemModel;
class QLabel;
class QModelIndex;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTreeView;
class TransferDialog;

/*
 * FileBrowserDialog: SFTP 双栏文件管理器（WinSCP 风格）。
 *
 * 左栏本地文件系统（QFileSystemModel），右栏远程目录（SftpClient 列举），
 * 顶部功能栏：上传/下载/刷新/新建目录/重命名/删除 + 传输队列入口。
 * 与终端会话（ConPTY + ssh.exe）各自独立：这里走 libssh2 的第二条连接。
 */
class FileBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FileBrowserDialog(const SshTarget &target, QWidget *parent = nullptr);
    ~FileBrowserDialog() override;

private slots:
    // --- SftpClient 回调 ---
    void onConnected(const QString &homePath, const QString &fingerprint);
    void onConnectFailed(const QString &error);
    void onSftpDisconnected();
    void onListed(const QString &path, const QVector<SftpEntry> &entries);
    void onListedFailed(const QString &path, const QString &error);
    void onOpFinished(const QString &op, const QString &arg, bool ok, const QString &error);

    // --- 顶栏动作 ---
    void onUploadClicked();
    void onDownloadClicked();
    void onRefreshClicked();
    void onMkdirClicked();
    void onRenameClicked();
    void onDeleteClicked();
    void showTransferQueue();

    // --- 导航 ---
    void onLocalActivated(const QModelIndex &index);
    void onLocalPathChosen();
    void onLocalUp();
    void onRemoteItemActivated(QTreeWidgetItem *item, int column);
    void onRemotePathChosen();
    void onRemoteUp();
    void updateActionStates();

private:
    void setLocalPath(const QString &path);
    void setRemotePath(const QString &path);
    void refreshRemote();
    QString remoteJoin(const QString &base, const QString &name) const;
    static QString localJoin(const QString &base, const QString &name);
    static QString permString(quint32 perm, bool isDir, bool isLink);
    struct RemoteRowInfo { bool valid; bool isDir; QString name; }; // 选中项解析
    RemoteRowInfo remoteSelected(int *count) const;

    SshTarget m_target;
    SftpClient *m_client = nullptr;        // 自有
    TransferDialog *m_transfer = nullptr;  // 自有

    QFileSystemModel *m_fsModel = nullptr;
    QTreeView *m_localView = nullptr;
    QTreeWidget *m_remoteTree = nullptr;
    QComboBox *m_localPathBox = nullptr;
    QComboBox *m_remotePathBox = nullptr;
    QPushButton *m_uploadBtn = nullptr;
    QPushButton *m_downloadBtn = nullptr;
    QPushButton *m_mkdirBtn = nullptr;
    QPushButton *m_renameBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QLabel *m_status = nullptr;
    bool m_connected = false;
    QString m_remotePath;
    QString m_localPath;
    QFileIconProvider m_iconProvider;
};

#endif // FILEBROWSERDIALOG_H
