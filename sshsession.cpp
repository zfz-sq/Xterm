#include "sshsession.h"

#include "ssh/knownhostsstore.h"
#include "ssh/libsshapi.h"
#include "ssh/sshworker.h"

#include <QMetaType>
#include <QThread>

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

void SshSession::answerHostKey(SshRequestId requestId, bool accept)
{
    emit requestHostKeyDecision(m_generation, requestId, accept);
}

void SshSession::answerAuthentication(SshRequestId requestId,
                                      const QStringList &answers)
{
    emit requestAuthAnswers(m_generation, requestId, answers);
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
