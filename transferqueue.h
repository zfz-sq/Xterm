#ifndef TRANSFERQUEUE_H
#define TRANSFERQUEUE_H

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class QFileSystemWatcher;
class QTimer;
class SftpClient;

/*
 * TransferQueue: 文件管理器的传输任务网关（原传输队列窗口的 headless 版）。
 *
 * 从 FileBrowserDialog 拆出的传输相关状态：
 *  - 上传/下载/建目录/队列删除的入队与串行泵（同一时刻一个任务在飞，
 *    完成的任务自动让位给下一个排队任务）；
 *  - 队列状态快照 progressChanged（底栏文案 + 百分比）与一键取消全部
 *    （cancelAll：当前 + 排队的上传/下载/删除一并取消）；
 *  - 下载后打开/编辑的 PendingFetch 登记（编辑完成自动回传、失败回滚摘要）；
 *  - 文件夹上传批次跟踪（全部任务收尾时发 folderBatchFinished，界面刷一次列表）。
 *
 * SftpClient::transferFinished / transferProgress 由本类接住并转成语义
 * 信号与底栏进度；纯 UI 反馈（弹窗、状态栏、openUrl）留在 FileBrowserDialog。
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
    explicit TransferQueue(SftpClient *client, const QString &sessionKey,
                           QObject *parent);

    // ---- 入队（返回任务 id，调用方据此跟踪批次）----
    int upload(const QString &local, const QString &remote, qint64 size);
    int download(const QString &local, const QString &remote, qint64 size);
    // 文件夹上传的建目录任务：与文件传输同一队列，严格排队保证
    // 「先建目录后传里面的文件」。localDir 仅作提示展示。
    int uploadDir(const QString &localDir, const QString &remoteDir);
    // 队列删除（目录递归清空带进度）；整批收尾时发 removeFinished
    int removeEntry(const QString &remotePath, bool isDir);
    // 下载到缓存目录并登记 PendingFetch（edit=true 时完成后开始监视回传）
    int fetch(bool edit, const QString &remotePath, qint64 size);

    // ---- 取消当前与排队的全部任务（上传/下载/建目录/删除）----
    void cancelAll();

    // ---- 下载完成后的登记（fetchFinished 的 ok 分支里调用）----
    void watchEditFile(const QString &local, const QString &remote);

    // ---- 文件夹上传批次 ----
    void trackFolderJob(int id);

signals:
    // 队列状态快照：active=有任务在跑（percent 0-100），text 为展示文案；
    // 队列收尾时 active=false、text 为汇总。供底栏进度条与取消按钮消费。
    void progressChanged(const QString &text, int percent, bool active);
    // 整批（建目录 + 文件）收尾：界面刷新一次远程列表
    void folderBatchFinished();
    // 本轮队列删除全部收尾：界面刷新一次远程列表
    void removeFinished();
    // fetch 登记的传输到达终态（uploadBack 除外）。失败弹窗、openUrl 由界面处理
    void fetchFinished(const PendingFetch &job, bool ok, const QString &error);
    // 编辑回传开始（状态栏文案）
    void editUploadStarted(const QString &fileName);
    // 编辑回传完成（ok=false 时 error/remote 用于失败弹窗）
    void editUploadFinished(bool ok, const QString &fileName, const QString &remote,
                            const QString &error);

private slots:
    void onTransferProgress(int id, qint64 done, qint64 total);
    void onTransferFinished(int id, bool ok, const QString &error);
    void onEditFileChanged(const QString &path);

private:
    struct Job
    {
        int id = 0;
        bool upload = false;
        bool makeDir = false;      // 建目录任务（无传输、无进度，瞬时完成）
        bool removeTask = false;   // 队列删除任务（目录递归清空，进度=条目数）
        bool removeIsDir = false;  // 删除目标是否为目录
        QString localPath;
        QString remotePath;
        qint64 size = 0;
        qint64 done = 0;
        qint64 total = 0;           // 文件字节数；删除任务=条目数（0=进度未知）
        int state = 0;              // 0=排队 1=进行中 2=完成 3=失败 4=取消
        QString error;
    };
    Job *jobById(int id);
    int appendJob(const Job &job);  // 公共入队尾巴：加任务、启动
    void pump();                    // 无活动任务时启动下一个
    void finishJob(int id, bool ok, const QString &error); // 置终态 + 语义信号 + 接力
    void emitProgress();            // 按当前队列状态发一次 progressChanged
    void commitEditUpload();
    QString cacheFile(bool edit, const QString &remotePath) const;

    SftpClient *m_client = nullptr;
    QString m_sessionKey;               // 编辑缓存目录的一段（见 cacheFile）
    QVector<Job> m_jobs;                // 任务表（串行泵的状态）
    int m_nextId = 1;
    int m_activeId = 0;                 // 0 = 空闲
    QHash<int, PendingFetch> m_pendingFetch;
    QSet<int> m_folderUploadIds;   // 本轮文件夹上传的全部任务 id（建目录 + 文件）
    QSet<int> m_removeIds;         // 本轮队列删除的任务 id（全部收尾发 removeFinished）
    QFileSystemWatcher *m_editWatcher = nullptr;
    QTimer *m_editTimer = nullptr;
    QString m_editTimerPath;
    QHash<QString, QString> m_watchedEdit;    // 本地缓存 -> 远程路径
    QHash<QString, QByteArray> m_editDigest;  // 上次已同步内容
};

#endif // TRANSFERQUEUE_H
