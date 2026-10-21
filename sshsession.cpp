#include "sshsession.h"

#include "ssh/knownhostsstore.h"
#include "ssh/libsshapi.h"
#include "ssh/sshworker.h"

#include <QMetaType>
#include <QThread>
#include <QVector>

#include <libssh/libssh.h>

SshSession::SshSession(QObject *parent)
    : Transport(parent)
    , m_ownedApi(std::make_unique<LibsshApi>())
    , m_ownedStore(std::make_unique<KnownHostsStore>(*m_ownedApi))
{
    setupWorker(*m_ownedApi, *m_ownedStore);
}

SshSession::SshSession(LibsshApi &api, KnownHostsStore &knownHosts,
                       QObject *parent)
    : Transport(parent)
{
    setupWorker(api, knownHosts);
}

SshSession::~SshSession()
{
    teardownWorker();
}

QString SshSession::availabilityError()
{
    static const int initialized = ssh_init();
    if (initialized != SSH_OK)
        return QObject::tr("libssh 初始化失败");
    return {};
}

void SshSession::setupWorker(LibsshApi &api, KnownHostsStore &knownHosts)
{
    static const int registered = [] {
        qRegisterMetaType<SshTarget>();
        qRegisterMetaType<SshError>();
        qRegisterMetaType<SshHostKeyChange>();
        qRegisterMetaType<SshPhase>();
        qRegisterMetaType<SshRequestId>();
        qRegisterMetaType<QList<SshAuthPrompt>>();
        qRegisterMetaType<SftpRequest>();
        qRegisterMetaType<SftpResult>();
        qRegisterMetaType<QVector<SftpEntry>>();
        return 0;
    }();
    Q_UNUSED(registered);

    m_thread = new QThread(this);
    m_worker = new SshWorker(api, knownHosts);
    m_worker->moveToThread(m_thread);

    connect(this, &SshSession::requestConnect, m_worker,
            &SshWorker::connectToHost, Qt::QueuedConnection);
    connect(this, &SshSession::requestWrite, m_worker,
            &SshWorker::writeShell, Qt::QueuedConnection);
    connect(this, &SshSession::requestResize, m_worker,
            &SshWorker::resizeShell, Qt::QueuedConnection);
    connect(this, &SshSession::requestCloseConnection, m_worker,
            &SshWorker::closeConnection, Qt::QueuedConnection);
    connect(this, &SshSession::requestHostKeyDecision, m_worker,
            &SshWorker::submitHostKeyDecision, Qt::QueuedConnection);
    connect(this, &SshSession::requestAuthAnswers, m_worker,
            &SshWorker::submitAuthAnswers, Qt::QueuedConnection);
    connect(this, &SshSession::requestSftp, m_worker,
            &SshWorker::submitSftpRequest, Qt::QueuedConnection);
    connect(this, &SshSession::requestSftpCancel, m_worker,
            &SshWorker::cancelSftpRequest, Qt::QueuedConnection);
    connect(this, &SshSession::requestStartStats, m_worker,
            &SshWorker::startStats, Qt::QueuedConnection);
    connect(this, &SshSession::requestStopStats, m_worker,
            &SshWorker::stopStats, Qt::QueuedConnection);

    connect(m_worker, &SshWorker::connected, this,
            &SshSession::handleWorkerConnected, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::shellData, this,
            &SshSession::handleWorkerShellData, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::hostKeyDecisionRequired, this,
            &SshSession::handleWorkerHostKey, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::unknownHostKeyRecorded, this,
            &SshSession::handleWorkerUnknownHostKey, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::authAnswersRequired, this,
            &SshSession::handleWorkerAuth, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::connectionFailed, this,
            &SshSession::handleWorkerFailed, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::connectionClosed, this,
            &SshSession::handleWorkerClosed, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::sftpResult, this,
            &SshSession::handleWorkerSftpResult, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::statsSample, this,
            &SshSession::handleWorkerStatsSample, Qt::QueuedConnection);
    connect(m_worker, &SshWorker::statsDegraded, this,
            &SshSession::handleWorkerStatsDegraded, Qt::QueuedConnection);

    m_thread->start();
}

void SshSession::teardownWorker()
{
    if (m_thread == nullptr)
        return;

    if (m_worker != nullptr) {
        disconnect(m_worker, nullptr, this, nullptr);
        disconnect(this, nullptr, m_worker, nullptr);
    }

    if (m_thread->isRunning()) {
        if (m_worker != nullptr) {
            QMetaObject::invokeMethod(m_worker, &QObject::deleteLater,
                                      Qt::QueuedConnection);
            m_worker = nullptr;
        }
        m_thread->quit();
        if (!m_thread->wait(2000)) {
            m_thread->terminate();
            m_thread->wait(1000);
        }
    } else {
        delete m_worker;
        m_worker = nullptr;
    }
}

void SshSession::setPtySize(int cols, int rows)
{
    if (cols < 20 || rows < 4)
        return;
    if (m_cols == cols && m_rows == rows)
        return;
    m_cols = cols;
    m_rows = rows;
    if (state() == Connected && m_openedEmitted)
        emit requestResize(m_generation, m_cols, m_rows);
}

QString SshSession::open(const SshTarget &target)
{
    if (m_ownedApi) {
        if (const QString err = availabilityError(); !err.isEmpty()) {
            emitError(err);
            return err;
        }
    }
    if (target.host.trimmed().isEmpty()) {
        const QString err = tr("请先填写 SSH 主机地址");
        emitError(err);
        return err;
    }
    if (state() == Connecting) {
        const QString err = tr("上一个 SSH 会话还在处理中，请稍候");
        emitError(err);
        return err;
    }
    if (state() == Connected || state() == Disconnecting) {
        m_pendingReopen = target;
        m_hasPendingReopen = true;
        if (state() == Connected)
            requestCloseCurrent();
        return {};
    }

    startConnect(target);
    return {};
}

void SshSession::close()
{
    m_hasPendingReopen = false;
    requestCloseCurrent();
}

void SshSession::requestCloseCurrent()
{
    if (state() == Disconnected || state() == Disconnecting)
        return;
    m_userRequestedClose = true;
    m_lastError = {SshError::Domain::Cancelled, 0,
                   tr("SSH connection cancelled"), false};
    setState(Disconnecting);
    emit requestCloseConnection(m_generation);
}

void SshSession::startConnect(const SshTarget &target)
{
    m_target = target;
    m_target.host = target.host.trimmed();
    m_userRequestedClose = false;
    m_openedEmitted = false;
    m_lastError = {};
    ++m_generation;
    setState(Connecting);
    emit requestConnect(m_generation, m_target, m_cols, m_rows);
}

QString SshSession::send(const QByteArray &data)
{
    if (state() != Connected)
        return tr("SSH 未连接，无法发送");
    emit requestWrite(m_generation, data);
    return {};
}

QString SshSession::summary() const
{
    QString s;
    if (!m_target.user.isEmpty())
        s += m_target.user + QLatin1Char('@');
    s += m_target.host;
    if (m_target.port != 22)
        s += QLatin1Char(':') + QString::number(m_target.port);
    return s;
}

void SshSession::startStats()
{
    if (state() != Connected) {
        return;
    }
    emit requestStartStats(m_generation);
}

void SshSession::stopStats()
{
    emit requestStopStats(m_generation);
}

void SshSession::answerHostKey(SshRequestId requestId, bool accept)
{
    emit requestHostKeyDecision(m_generation, requestId, accept);
}

void SshSession::answerAuthentication(SshRequestId requestId,
                                      const QStringList &answers)
{
    emit requestAuthAnswers(m_generation, requestId, answers);
}

void SshSession::listDir(SshRequestId requestId, const QString &path)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::List;
    request.path = path;
    emit requestSftp(m_generation, request);
}

void SshSession::realPath(SshRequestId requestId, const QString &path)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::RealPath;
    request.path = path;
    emit requestSftp(m_generation, request);
}

void SshSession::uploadFile(SshRequestId requestId, const QString &localPath,
                            const QString &remotePath)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Upload;
    request.localPath = localPath;
    request.remotePath = remotePath;
    emit requestSftp(m_generation, request);
}

void SshSession::downloadFile(SshRequestId requestId, const QString &remotePath,
                              const QString &localPath)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Download;
    request.remotePath = remotePath;
    request.localPath = localPath;
    emit requestSftp(m_generation, request);
}

void SshSession::cancelTransfer(SshRequestId requestId)
{
    emit requestSftpCancel(m_generation, requestId);
}

void SshSession::makeDir(SshRequestId requestId, const QString &path)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Mkdir;
    request.path = path;
    emit requestSftp(m_generation, request);
}

void SshSession::createFile(SshRequestId requestId, const QString &path)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Create;
    request.path = path;
    emit requestSftp(m_generation, request);
}

void SshSession::removeEntry(SshRequestId requestId, const QString &path,
                             bool isDir)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Delete;
    request.path = path;
    request.isDir = isDir;
    emit requestSftp(m_generation, request);
}

void SshSession::renameEntry(SshRequestId requestId, const QString &from,
                             const QString &to)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Rename;
    request.path = from;
    request.dest = to;
    emit requestSftp(m_generation, request);
}

void SshSession::copyRemote(SshRequestId requestId, const QString &from,
                            const QString &to, bool isDir)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Copy;
    request.path = from;
    request.dest = to;
    request.isDir = isDir;
    emit requestSftp(m_generation, request);
}

void SshSession::setPermissions(SshRequestId requestId, const QString &path,
                                quint32 mode)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Chmod;
    request.path = path;
    request.mode = mode;
    emit requestSftp(m_generation, request);
}

void SshSession::execCommand(SshRequestId requestId, const QString &command)
{
    SftpRequest request;
    request.requestId = requestId;
    request.op = SftpOp::Exec;
    request.command = command;
    emit requestSftp(m_generation, request);
}

void SshSession::handleWorkerConnected(quint64 generation)
{
    if (generation != m_generation || m_openedEmitted
        || state() != Connecting) {
        return;
    }
    m_openedEmitted = true;
    emitOpened();
    emit requestResize(m_generation, m_cols, m_rows);
}

void SshSession::handleWorkerShellData(quint64 generation,
                                       const QByteArray &data)
{
    if (generation != m_generation || state() != Connected)
        return;
    emit received(data);
}

void SshSession::handleWorkerFailed(quint64 generation,
                                    const SshError &error)
{
    if (generation != m_generation)
        return;
    m_lastError = error;
    emitError(error.message);
}

void SshSession::handleWorkerHostKey(quint64 generation,
                                     const SshHostKeyChange &change)
{
    if (generation != m_generation || state() != Connecting)
        return;
    emit hostKeyDecisionRequired(change);
}

void SshSession::handleWorkerUnknownHostKey(quint64 generation,
                                            const QString &host)
{
    if (generation != m_generation || state() != Connecting)
        return;
    emit unknownHostKeyRecorded(host);
}

void SshSession::handleWorkerAuth(quint64 generation,
                                  SshRequestId requestId,
                                  const QList<SshAuthPrompt> &prompts)
{
    if (generation != m_generation || state() != Connecting)
        return;
    emit authAnswersRequired(requestId, prompts);
}

void SshSession::handleWorkerSftpResult(quint64 generation,
                                        const SftpResult &result)
{
    if (generation != m_generation)
        return;
    switch (result.kind) {
    case SftpResult::Kind::Listed:
        emit sftpListed(result.requestId, result.path, result.entries);
        break;
    case SftpResult::Kind::ListedFailed:
        emit sftpListedFailed(result.requestId, result.path, result.error);
        break;
    case SftpResult::Kind::Progress:
        emit sftpTransferProgress(result.requestId, result.done, result.total);
        break;
    case SftpResult::Kind::TransferFinished:
        emit sftpTransferFinished(result.requestId, result.ok, result.error);
        break;
    case SftpResult::Kind::OpFinished:
        emit sftpOpFinished(result.requestId, result.op, result.arg, result.ok,
                            result.error);
        break;
    }
}

void SshSession::handleWorkerStatsSample(quint64 generation,
                                         const QString &sample)
{
    if (generation != m_generation || state() != Connected) {
        return;
    }
    emit statsSample(sample);
}

void SshSession::handleWorkerStatsDegraded(quint64 generation)
{
    if (generation != m_generation || state() != Connected) {
        return;
    }
    emit statsDegraded();
}

void SshSession::handleWorkerClosed(quint64 generation)
{
    if (generation != m_generation)
        return;

    const bool wasOpen = m_openedEmitted;
    const bool userRequested = m_userRequestedClose;
    const int exitCode = m_lastError.isError() ? m_lastError.code : 0;
    m_openedEmitted = false;
    m_userRequestedClose = false;
    emitClosed();
    if (wasOpen)
        emit sessionEnded(exitCode, userRequested);

    if (m_hasPendingReopen) {
        m_hasPendingReopen = false;
        const SshTarget next = m_pendingReopen;
        m_pendingReopen = {};
        startConnect(next);
    }
}
