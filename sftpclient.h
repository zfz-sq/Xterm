#ifndef SFTPCLIENT_H
#define SFTPCLIENT_H

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
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
 * SftpClient: 独立 SFTP 连接（libssh2，静态链接，WinCNG 加密后端，含 ECDH）。
 *
 * 与终端会话（ConPTY + ssh.exe）互不相干：文件管理器走自己的第二条
 * SSH 连接，和 WinSCP 直连是同一回事。
 *
 * 线程模型：所有 libssh2 调用都发生在自带的 QThread 里（阻塞 API +
 * session 超时，不会永久卡死）；主线程只通过信号槽交互。传输取消是
 * 协作式的——每写完一个 32KB 块检查一次标志，取消后连接仍然完好。
 *
 * 主机指纹校验：首次连接把 SHA256 指纹记入 AppConfigLocation/
 * known_hosts。之后指纹变化时询问界面（setHostKeyPrompter）；
 * 用户确认后改写记录并继续，取消则断开。没有询问回调时直接拒绝。
 */
class SftpClient : public QObject
{
    Q_OBJECT

public:
    explicit SftpClient(QObject *parent = nullptr);
    ~SftpClient() override;

    static QString knownHostsPath();

    // 返回 true 表示信任新指纹并改写记录。由界面线程执行对话框。
    using HostKeyPrompter = std::function<bool(const QString &host,
                                               const QString &storedFingerprint,
                                               const QString &currentFingerprint)>;
    void setHostKeyPrompter(HostKeyPrompter prompter);

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
    void createFile(const QString &path);
    void removeEntry(const QString &path, bool isDir);
    void renameEntry(const QString &from, const QString &to);
    void copyRemote(const QString &from, const QString &to, bool isDir);
    void setPermissions(const QString &path, quint32 mode);
    void execCommand(const QString &command);

private:
    bool ensureSession(QString *error);          // 没连接则填充错误并返回 false
    QString lastError(const char *fallback) const;
    QString localReadFile(const QString &path);  // known_hosts 读取（工作线程内）
    bool confirmHostKey(const QString &host, const QString &stored, const QString &current);
    bool rememberHost(const QString &key, const QString &fingerprint);
    bool deferToWorker(const std::function<void()> &fn);

    HostKeyPrompter m_prompter;

    QThread *m_thread = nullptr;
    void *m_session = nullptr;                   // LIBSSH2_SESSION*
    void *m_sftp = nullptr;                      // LIBSSH2_SFTP*
    quintptr m_sock = 0;                         // WinSock SOCKET，0 = 未连接
    QString m_host;                              // "host:port"，指纹记录用
    std::atomic_bool m_cancel{false};            // 当前传输的取消标志
};

#endif // SFTPCLIENT_H
