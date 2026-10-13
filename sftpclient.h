#ifndef SFTPCLIENT_H
#define SFTPCLIENT_H

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include "sshtarget.h"

/*
 * SftpEntry: 远程目录里的一条文件/子目录信息。
 */
struct SftpEntry
{
    QString name;
    quint64 size = 0;          // 字节数（目录为 0）
    qint64 mtime = 0;          // epoch 秒
    quint32 permissions = 0;   // POSIX 权限位（0644 之类）
    bool isDir = false;
    bool isSymlink = false;
};
Q_DECLARE_METATYPE(SftpEntry)
Q_DECLARE_METATYPE(QVector<SftpEntry>)

/*
 * SftpClient: 独立 SFTP 连接（libssh2，静态链接，WinCNG 加密后端）。
 *
 * 与终端会话（ConPTY + ssh.exe）互不相干：文件管理器走自己的第二条
 * SSH 连接，和 WinSCP 直连是同一回事。
 *
 * 线程模型：所有 libssh2 调用都发生在自带的 QThread 里（阻塞 API +
 * session 超时，不会永久卡死）；主线程只通过信号槽交互。传输取消是
 * 协作式的——每写完一个 32KB 块检查一次标志，取消后连接仍然完好。
 *
 * 主机指纹校验：首次连接把 SHA256 指纹记入 AppConfigLocation/
 * known_hosts；之后指纹变化则拒绝连接（防中间人），删该文件可重置。
 */
class SftpClient : public QObject
{
    Q_OBJECT

public:
    explicit SftpClient(QObject *parent = nullptr);
    ~SftpClient() override;

    static QString knownHostsPath();

signals:
    void connected(const QString &homePath, const QString &fingerprint);
    void connectFailed(const QString &error);
    void disconnected();

    void listed(const QString &path, const QVector<SftpEntry> &entries);
    void listedFailed(const QString &path, const QString &error);

    void transferProgress(int id, qint64 done, qint64 total);
    void transferFinished(int id, bool ok, const QString &error);

    void opFinished(const QString &op, const QString &arg, bool ok, const QString &error);

public slots:
    void connectToHost(const SshTarget &target);
    void disconnectFromHost();
    void listDir(const QString &path);
    void realPath(const QString &path);
    void uploadFile(int id, const QString &localPath, const QString &remotePath);
    void downloadFile(int id, const QString &remotePath, const QString &localPath);
    void cancelTransfer(int id);
    void makeDir(const QString &path);
    void removeEntry(const QString &path, bool isDir);
    void renameEntry(const QString &from, const QString &to);

private:
    bool ensureSession(QString *error);          // 没连接则填充错误并返回 false
    QString lastError(const char *fallback) const;
    QString localReadFile(const QString &path);  // known_hosts 读取（工作线程内）

    QThread *m_thread = nullptr;
    void *m_session = nullptr;                   // LIBSSH2_SESSION*
    void *m_sftp = nullptr;                      // LIBSSH2_SFTP*
    quintptr m_sock = 0;                         // WinSock SOCKET，0 = 未连接
    QString m_host;                              // "host:port"，指纹记录用
    std::atomic_bool m_cancel{false};            // 当前传输的取消标志
};

#endif // SFTPCLIENT_H
