#pragma once

#include "libsshapi.h"
#include "sftprequest.h"

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QQueue>
#include <QString>
#include <QVector>

class QThread;

class SftpChannel final
{
public:
    enum class Result {
        Progress,
        Again,
        Idle,
        Error
    };

    static constexpr int MaxInFlightAio = 4;
    static constexpr int TickByteBudget = 32 * 1024;
    static constexpr int TickRequestBudget = 4;
    static constexpr int ChunkSize = 16 * 1024;

    SftpChannel(LibsshApi &api, ssh_session session);
    ~SftpChannel();

    SftpChannel(const SftpChannel &) = delete;
    SftpChannel &operator=(const SftpChannel &) = delete;

    void enqueue(const SftpRequest &request);
    void cancel(SshRequestId requestId);
    Result advance(QList<SftpResult> *results);
    void close(QList<SftpResult> *results);

private:
    struct AioRequest {
        sftp_aio aio = nullptr;
        QByteArray buffer;
        ssize_t requested = 0;
        bool reading = false;
    };

    enum class ChannelStep {
        Open,
        Subsystem,
        Wrap,
        Init,
        Done
    };

    enum class Phase {
        None,
        ListOpen,
        ListRead,
        ListClose,
        RealPath,
        Mkdir,
        CreateOpen,
        CreateClose,
        Delete,
        Rename,
        Chmod,
        TransferPrepare,
        TransferIo,
        TransferClose,
        CopyOpenSrc,
        CopyOpenDst,
        CopyIo,
        CopyClose,
        ExecOpen,
        ExecSession,
        ExecRequest,
        ExecRead,
        ExecClose
    };

    Result ensureSession(QList<SftpResult> *results);
    Result advanceActive(QList<SftpResult> *results);
    Result advanceList(QList<SftpResult> *results);
    Result advanceRealPath(QList<SftpResult> *results);
    Result advanceSimple(QList<SftpResult> *results);
    Result advanceCreate(QList<SftpResult> *results);
    Result advanceUpload(QList<SftpResult> *results);
    Result advanceDownload(QList<SftpResult> *results);
    Result advanceCopy(QList<SftpResult> *results);
    Result advanceExec(QList<SftpResult> *results);
    Result waitAio(AioRequest *request, QList<SftpResult> *results);
    Result beginRead(sftp_file file, QList<SftpResult> *results);
    Result beginWrite(sftp_file file, const QByteArray &bytes,
                      QList<SftpResult> *results);
    Result failActive(QList<SftpResult> *results, const QString &operation);
    void completeOp(QList<SftpResult> *results, const QString &op,
                    const QString &arg, bool ok, const QString &error = {});
    void completeTransfer(QList<SftpResult> *results, bool ok,
                          const QString &error = {});
    void completeList(QList<SftpResult> *results, bool ok,
                      const QString &error = {});
    void emitCancelled(QList<SftpResult> *results, const SftpRequest &request);
    void abortActive(QList<SftpResult> *results, const QString &error);
    void cleanupTransfer(bool cancelled);
    void freeAio();
    void closeRemoteFiles();
    void releaseSftp();
    void resetActive();
    bool isAgain() const;
    bool onOwnerThread() const;
    QString apiError(const QString &fallback) const;
    static QString shellQuote(const QString &text);

    LibsshApi &m_api;
    QThread *m_ownerThread = nullptr;
    ssh_session m_session = nullptr;
    sftp_session m_sftp = nullptr;
    ssh_channel m_sftpChannel = nullptr;
    ChannelStep m_channelStep = ChannelStep::Open;
    bool m_sftpReady = false;
    QQueue<SftpRequest> m_queue;
    QList<SftpRequest> m_cancelledQueued;
    SftpRequest m_active;
    bool m_hasActive = false;
    bool m_cancelled = false;
    Phase m_phase = Phase::None;
    sftp_dir m_dir = nullptr;
    sftp_file m_file = nullptr;
    sftp_file m_destFile = nullptr;
    ssh_channel m_exec = nullptr;
    QVector<SftpEntry> m_entries;
    QFile m_local;
    QString m_tempPath;
    QByteArray m_pending;
    QByteArray m_execOutput;
    QList<AioRequest> m_aio;
    qint64 m_done = 0;
    qint64 m_total = 0;
    int m_tickBytes = 0;
    int m_tickRequests = 0;
    bool m_readEof = false;
    quint32 m_copyMode = 0644;
};
