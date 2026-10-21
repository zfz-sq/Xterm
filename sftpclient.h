#ifndef SFTPCLIENT_H
#define SFTPCLIENT_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include "ssh/sftprequest.h"
#include "sshtarget.h"

class SshSession;

/*
 * SftpClient: 文件管理器的 GUI 适配器。
 *
 * 不再自建第二条 SSH 连接：所有操作都转发给已连接的 SshSession，
 * 由 worker 里的共享 SFTP 通道在同一条连接上串行完成，shell 读
 * 仍然每个调度 tick 优先。界面信号保持旧形状，对话框改动最小。
 *
 * 请求号映射：SshSession 用 SshRequestId，界面沿用旧的 int 传输
 * id。适配器分配请求号并记录在册；结果到达时只回放自己发起的
 * 请求（同一会话上可能有多个适配器）。析构时取消全部未完成请
 * 求——只取消文件请求，shell 不受影响。
 */
class SftpClient : public QObject
{
    Q_OBJECT

public:
    explicit SftpClient(SshSession *session, QObject *parent = nullptr);
    ~SftpClient() override;

signals:
    void listed(const QString &path, const QVector<SftpEntry> &entries);
    void listedFailed(const QString &path, const QString &error);

    void transferProgress(int id, qint64 done, qint64 total);
    void transferFinished(int id, bool ok, const QString &error);

    void opFinished(const QString &op, const QString &arg, bool ok, const QString &error);

public slots:
    void listDir(const QString &path);
    void realPath(const QString &path);
    void uploadFile(int id, const QString &localPath, const QString &remotePath);
    void downloadFile(int id, const QString &remotePath, const QString &localPath);
    void cancelTransfer(int id);
    void makeDir(const QString &path);
    void createFile(const QString &path);
    void removeEntry(const QString &path, bool isDir);
    void renameEntry(const QString &from, const QString &to);
    void copyRemote(const QString &from, const QString &to, bool isDir);
    void setPermissions(const QString &path, quint32 mode);
    void execCommand(const QString &command);

private slots:
    void onSessionListed(SshRequestId requestId, const QString &path,
                         const QVector<SftpEntry> &entries);
    void onSessionListedFailed(SshRequestId requestId, const QString &path,
                               const QString &error);
    void onSessionTransferProgress(SshRequestId requestId, qint64 done,
                                   qint64 total);
    void onSessionTransferFinished(SshRequestId requestId, bool ok,
                                   const QString &error);
    void onSessionOpFinished(SshRequestId requestId, const QString &op,
                             const QString &arg, bool ok, const QString &error);

private:
    struct TrackedRequest
    {
        int transferId = 0;   // 非 0 表示传输类请求，映射回界面 id
    };

    void submit(SftpRequest request, int transferId = 0);
    bool trackedTake(SshRequestId requestId, int *transferId);

    SshSession *m_session = nullptr;
    SshRequestId m_nextId = 1;
    QHash<SshRequestId, TrackedRequest> m_pending;
    QHash<int, SshRequestId> m_transferRequests;
};

#endif // SFTPCLIENT_H
