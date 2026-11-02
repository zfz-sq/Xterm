#ifndef TRANSFERQUEUE_H
#define TRANSFERQUEUE_H

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

class QFileSystemWatcher;
class QTimer;
class SftpClient;
class TransferDialog;
struct QWidget;

/*
 * TransferQueue: 文件管理器的传输队列网关。
 *
 * 从 FileBrowserDialog 拆出的传输相关状态：
 *  - 封装 TransferDialog（队列窗口）的入队（上传/下载/建目录）；
 *  - 下载后打开/编辑的 PendingFetch 登记（编辑完成自动回传、失败回滚摘要）；
 *  - 「编辑远端文件」的本地缓存监视（QFileSystemWatcher + 防抖定时器）；
 *  - 文件夹上传批次跟踪（全部任务收尾时发 folderBatchFinished，界面刷一次列表）。
 *
 * SftpClient::transferFinished / TransferDialog::jobFinished 由本类接住并
 * 转成语义信号；纯 UI 反馈（弹窗、状态栏、openUrl）留在 FileBrowserDialog。
 */

struct PendingFetch
{
    bool edit = false;       // 下载后打开编辑器并监视回传
    bool uploadBack = false; // 这次传输是编辑回传
    QString local;
    QString remote;
    QByteArray previousDigest;
};

class TransferQueue : public QObject
{
    Q_OBJECT

public:
    // dialogParent：TransferDialog 的父窗口（用于窗口居中/内存归属）
    explicit TransferQueue(SftpClient *client, const QString &sessionKey,
                           QWidget *dialogParent, QObject *parent);
    ~TransferQueue() override = default;

    TransferDialog *dialog() const { return m_dialog; }

    // ---- 入队 ----
    int upload(const QString &local, const QString &remote, qint64 size);
    int download(const QString &local, const QString &remote, qint64 size);
    int uploadDir(const QString &localDir, const QString &remoteDir);
    // 下载到缓存目录并登记 PendingFetch（edit=true 时完成后开始监视回传）
    int fetch(bool edit, const QString &remotePath, qint64 size);

    // ---- 下载完成后的登记（fetchFinished 的 ok 分支里调用）----
    void watchEditFile(const QString &local, const QString &remote);

    // ---- 文件夹上传批次 ----
    void noteFolderMkdir(const QString &remoteDir); // 该 mkdir 的结果在 opFinished 里静默放行
    void trackFolderJob(int id);
    // opFinished 收到 mkdir 结果时调用：属于文件夹上传批次则吞掉（队列窗口已展示），返回 true
    bool consumeFolderMkdir(const QString &remoteDir);

signals:
    // 整批（建目录 + 文件）收尾：界面刷新一次远程列表
    void folderBatchFinished();
    // fetch 登记的传输到达终态（uploadBack 除外）。失败弹窗、openUrl 由界面处理
    void fetchFinished(const PendingFetch &job, bool ok, const QString &error);
    // 编辑回传开始（状态栏文案）
    void editUploadStarted(const QString &fileName);
    // 编辑回传完成（ok=false 时 error/remote 用于失败弹窗）
    void editUploadFinished(bool ok, const QString &fileName, const QString &remote,
                            const QString &error);

private slots:
    void onTransferFinished(int id, bool ok, const QString &error);
    void onJobFinished(int id);
    void onEditFileChanged(const QString &path);

private:
    void commitEditUpload();
    QString cacheFile(bool edit, const QString &remotePath) const;

    SftpClient *m_client = nullptr;      // 不拥有
    TransferDialog *m_dialog = nullptr;  // 队列窗口（parent 为 dialogParent）
    QString m_sessionKey;               // 编辑缓存目录的一段（见 cacheFile）
    QHash<int, PendingFetch> m_pendingFetch;
    QSet<int> m_folderUploadIds;   // 本轮文件夹上传的全部任务 id（建目录 + 文件）
    QSet<QString> m_folderMkdirs;  // 文件夹上传要建的远程目录，opFinished 里静默放行
    QFileSystemWatcher *m_editWatcher = nullptr;
    QTimer *m_editTimer = nullptr;
    QString m_editTimerPath;
    QHash<QString, QString> m_watchedEdit;    // 本地缓存 -> 远程路径
    QHash<QString, QByteArray> m_editDigest;  // 上次已同步内容
};

#endif // TRANSFERQUEUE_H
