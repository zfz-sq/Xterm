#ifndef SSHSESSION_H
#define SSHSESSION_H

#include <memory>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "ssh/sftprequest.h"
#include "ssh/sshtypes.h"
#include "sshtarget.h"
#include "transport.h"

class KnownHostsStore;
class LibsshApi;
class QThread;
class SshWorker;

/*
 * SshSession: SSH 后端，实现 Transport 契约。
 *
 * GUI 线程 facade：创建 SshWorker 并移到私有 QThread，命令和事件全部
 * queued 连接。认证成功后 worker 的 connected 映射为 opened()，原始
 * bytes 原样转发到 received()。
 */
class SshSession : public Transport
{
    Q_OBJECT

public:
    explicit SshSession(QObject *parent = nullptr);
    SshSession(LibsshApi &api, KnownHostsStore &knownHosts,
               QObject *parent = nullptr);
    ~SshSession() override;

    Kind kind() const override { return SshLink; }

    static QString availabilityError();

    void setPtySize(int cols, int rows);

    void listDir(SshRequestId requestId, const QString &path);
    void realPath(SshRequestId requestId, const QString &path);
    void uploadFile(SshRequestId requestId, const QString &localPath,
                    const QString &remotePath);
    void downloadFile(SshRequestId requestId, const QString &remotePath,
                      const QString &localPath);
    void cancelTransfer(SshRequestId requestId);
    void makeDir(SshRequestId requestId, const QString &path);
    void createFile(SshRequestId requestId, const QString &path);
    void removeEntry(SshRequestId requestId, const QString &path, bool isDir);
    void renameEntry(SshRequestId requestId, const QString &from,
                     const QString &to);
    void copyRemote(SshRequestId requestId, const QString &from,
                    const QString &to, bool isDir);
    void setPermissions(SshRequestId requestId, const QString &path,
                        quint32 mode);
    void execCommand(SshRequestId requestId, const QString &command);

    QString open(const SshTarget &target);
    void close() override;
    QString send(const QByteArray &data) override;

    SshTarget target() const { return m_target; }
    QString summary() const;
    SshError lastError() const { return m_lastError; }

public slots:
    void answerHostKey(SshRequestId requestId, bool accept);
    void answerAuthentication(SshRequestId requestId,
                              const QStringList &answers);

signals:
    void sessionEnded(int exitCode, bool userRequested);
    void hostKeyDecisionRequired(const SshHostKeyChange &change);
    void unknownHostKeyRecorded(const QString &host);
    void authAnswersRequired(SshRequestId requestId,
                             const QList<SshAuthPrompt> &prompts);
    void sftpListed(SshRequestId requestId, const QString &path,
                    const QVector<SftpEntry> &entries);
    void sftpListedFailed(SshRequestId requestId, const QString &path,
                          const QString &error);
    void sftpTransferProgress(SshRequestId requestId, qint64 done,
                              qint64 total);
    void sftpTransferFinished(SshRequestId requestId, bool ok,
                              const QString &error);
    void sftpOpFinished(SshRequestId requestId, const QString &op,
                        const QString &arg, bool ok, const QString &error);

private:
    friend class TestSshSession;
    friend class TestSftpChannel;

    void setupWorker(LibsshApi &api, KnownHostsStore &knownHosts);
    void teardownWorker();
    void startConnect(const SshTarget &target);
    void requestCloseCurrent();

private slots:
    void handleWorkerConnected(quint64 generation);
    void handleWorkerShellData(quint64 generation, const QByteArray &data);
    void handleWorkerClosed(quint64 generation);
    void handleWorkerFailed(quint64 generation, const SshError &error);
    void handleWorkerHostKey(quint64 generation,
                             const SshHostKeyChange &change);
    void handleWorkerUnknownHostKey(quint64 generation, const QString &host);
    void handleWorkerAuth(quint64 generation, SshRequestId requestId,
                          const QList<SshAuthPrompt> &prompts);
    void handleWorkerSftpResult(quint64 generation, const SftpResult &result);

signals:
    void requestConnect(quint64 generation, const SshTarget &target,
                        int cols, int rows);
    void requestWrite(quint64 generation, const QByteArray &data);
    void requestResize(quint64 generation, int cols, int rows);
    void requestCloseConnection(quint64 generation);
    void requestHostKeyDecision(quint64 generation, SshRequestId requestId,
                                bool accept);
    void requestAuthAnswers(quint64 generation, SshRequestId requestId,
                            const QStringList &answers);
    void requestSftp(quint64 generation, const SftpRequest &request);
    void requestSftpCancel(quint64 generation, SshRequestId requestId);

private:
    std::unique_ptr<LibsshApi> m_ownedApi;
    std::unique_ptr<KnownHostsStore> m_ownedStore;
    QThread *m_thread = nullptr;
    SshWorker *m_worker = nullptr;
    SshTarget m_target;
    SshTarget m_pendingReopen;
    quint64 m_generation = 0;
    int m_cols = 80;
    int m_rows = 24;
    bool m_userRequestedClose = false;
    bool m_hasPendingReopen = false;
    bool m_openedEmitted = false;
    SshError m_lastError;
};

#endif // SSHSESSION_H
