#include "sshworker.h"

#include "knownhostsstore.h"
#include "libsshapi.h"
#include "libsshconnection.h"
#include "sftpchannel.h"
#include "shellchannel.h"
#include "statschannel.h"

#include <QMetaType>
#include <QThread>
#include <QVector>

SshWorker::SshWorker(LibsshApi &api, KnownHostsStore &knownHosts,
                     QObject *parent)
    : QObject(parent), m_api(api), m_knownHosts(knownHosts)
{
    static const int registered = [] {
        qRegisterMetaType<SftpRequest>();
        qRegisterMetaType<SftpResult>();
        qRegisterMetaType<QVector<SftpEntry>>();
        return 0;
    }();
    Q_UNUSED(registered);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &SshWorker::advance);
}

SshWorker::~SshWorker()
{
    if (!isOwnerThread()) {
        qFatal("SshWorker must be destroyed on its owner thread");
    }
    m_timer->stop();
    forceCloseCurrent(false);
}

void SshWorker::connectToHost(quint64 generation, const SshTarget &target,
                              int cols, int rows)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, target, cols, rows] {
                connectToHost(generation, target, cols, rows);
            },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    m_timer->stop();
    forceCloseCurrent(true);

    m_generation = generation;
    m_initialCols = qMax(1, cols);
    m_initialRows = qMax(1, rows);
    m_closeNotified = false;
    m_notifyOnClose = false;
    resetBackoff();
    m_state = State::Connecting;
    m_connection =
        std::make_unique<LibsshConnection>(m_api, m_knownHosts);

    connect(m_connection.get(), &LibsshConnection::phaseChanged, this,
            [this, generation](SshPhase phase) {
                if (generation == m_generation
                    && m_state != State::Idle
                    && m_state != State::Closing) {
                    emit phaseChanged(generation, phase);
                }
            });
    connect(m_connection.get(),
            &LibsshConnection::hostKeyDecisionRequired, this,
            [this, generation](const SshHostKeyChange &change) {
                if (generation == m_generation
                    && m_state == State::Connecting) {
                    emit hostKeyDecisionRequired(generation, change);
                }
            });
    connect(m_connection.get(),
            &LibsshConnection::unknownHostKeyRecorded, this,
            [this, generation](const QString &host) {
                if (generation == m_generation
                    && m_state == State::Connecting) {
                    emit unknownHostKeyRecorded(generation, host);
                }
            });
    connect(m_connection.get(), &LibsshConnection::authAnswersRequired,
            this,
            [this, generation](
                SshRequestId requestId,
                const QList<SshAuthPrompt> &prompts) {
                if (generation == m_generation
                    && m_state == State::Connecting) {
                    emit authAnswersRequired(generation, requestId,
                                             prompts);
                }
            });
    connect(m_connection.get(), &LibsshConnection::finished, this,
            [this, generation](const SshError &error) {
                handleConnectionFinished(generation, error);
            });

    m_connection->begin(target);
    scheduleAgain();
}

void SshWorker::writeShell(quint64 generation, const QByteArray &data)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, data] { writeShell(generation, data); },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation || m_shell == nullptr
        || (m_state != State::OpeningShell
            && m_state != State::Ready)) {
        return;
    }
    if (!m_shell->queueInput(data)) {
        failAndClose({SshError::Domain::Shell, SSH_ERROR,
                      QStringLiteral(
                          "Remote shell input queue limit exceeded"),
                      false});
        return;
    }
    schedule();
}

void SshWorker::resizeShell(quint64 generation, int cols, int rows)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, cols, rows] {
                resizeShell(generation, cols, rows);
            },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation || m_shell == nullptr
        || (m_state != State::OpeningShell
            && m_state != State::Ready)) {
        return;
    }
    m_shell->resize(cols, rows);
    schedule();
}

void SshWorker::closeConnection(quint64 generation)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this,
            [this, generation] { closeConnection(generation); },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation || m_state == State::Idle) {
        return;
    }
    requestClose(true);
}

void SshWorker::submitHostKeyDecision(quint64 generation,
                                      SshRequestId requestId, bool accept)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, requestId, accept] {
                submitHostKeyDecision(generation, requestId, accept);
            },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation || m_state != State::Connecting
        || m_connection == nullptr) {
        return;
    }
    m_connection->submitHostKeyDecision(requestId, accept);
    schedule();
}

void SshWorker::submitAuthAnswers(quint64 generation,
                                  SshRequestId requestId,
                                  const QStringList &answers)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, requestId, answers] {
                submitAuthAnswers(generation, requestId, answers);
            },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation || m_state != State::Connecting
        || m_connection == nullptr) {
        return;
    }
    m_connection->submitAuthAnswers(requestId, answers);
    schedule();
}

void SshWorker::submitSftpRequest(quint64 generation,
                                  const SftpRequest &request)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, request] {
                submitSftpRequest(generation, request);
            },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation) {
        return;
    }
    if (m_state != State::Ready || m_connection == nullptr) {
        // 不在服务状态也要给调用方恰好一次终止结果，否则界面上
        // 的等待方永远收不到回音（shell 通道不受影响）。
        QList<SftpResult> results;
        results.append(sftpCancelledResult(request));
        emitSftpResults(results);
        return;
    }
    if (m_sftp == nullptr) {
        m_sftp = std::make_unique<SftpChannel>(
            m_api, m_connection->sessionForWorker());
    }
    m_sftp->enqueue(request);
    schedule();
}

void SshWorker::cancelSftpRequest(quint64 generation, SshRequestId requestId)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, requestId] {
                cancelSftpRequest(generation, requestId);
            },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation || m_sftp == nullptr) {
        return;
    }
    m_sftp->cancel(requestId);
    schedule();
}

void SshWorker::startStats(quint64 generation)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this, [this, generation] { startStats(generation); },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation || m_state != State::Ready
        || m_connection == nullptr) {
        return;
    }
    if (m_stats == nullptr) {
        m_stats = std::make_unique<StatsChannel>(
            m_api, m_connection->sessionForWorker());
    }
    m_stats->start();
    schedule();
}

void SshWorker::stopStats(quint64 generation)
{
    if (!isOwnerThread()) {
        QMetaObject::invokeMethod(
            this, [this, generation] { stopStats(generation); },
            Qt::QueuedConnection);
        return;
    }
    assertOwnerThread();
    if (generation != m_generation || m_stats == nullptr) {
        return;
    }
    m_stats->stop();
}

void SshWorker::advance()
{
    assertOwnerThread();
    if (m_state == State::Connecting) {
        if (m_connection != nullptr) {
            m_connection->advance();
        }
        if (m_state == State::Connecting) {
            scheduleAgain();
        } else if (m_state == State::OpeningShell) {
            resetBackoff();
            schedule();
        }
        return;
    }

    if (m_state == State::Closing && m_shell == nullptr) {
        forceCloseCurrent(m_notifyOnClose);
        return;
    }

    if ((m_state != State::OpeningShell && m_state != State::Ready
         && m_state != State::Closing)
        || m_shell == nullptr) {
        return;
    }

    QList<QByteArray> output;
    QString errorMessage;
    const ShellChannel::Result result =
        m_shell->advance(&output, &errorMessage);
    for (const QByteArray &bytes : output) {
        emit shellData(m_generation, bytes);
    }

    if (m_state == State::Ready) {
        serviceSftp();
        serviceStats();
    }

    switch (result) {
    case ShellChannel::Result::Ready:
        if (m_state == State::OpeningShell) {
            m_state = State::Ready;
            emit phaseChanged(m_generation, SshPhase::Ready);
            emit connected(m_generation);
        }
        resetBackoff();
        schedule(m_state == State::Ready ? ReadyPollDelayMs : 0);
        return;
    case ShellChannel::Result::Progress:
        resetBackoff();
        if (m_state == State::Closing) {
            schedule();
            return;
        }
        schedule(m_state == State::Ready ? ReadyPollDelayMs : 0);
        return;
    case ShellChannel::Result::Again:
        scheduleAgain();
        return;
    case ShellChannel::Result::Closed:
        if (m_state != State::Closing) {
            m_state = State::Closing;
            m_notifyOnClose = true;
        }
        forceCloseCurrent(m_notifyOnClose);
        return;
    case ShellChannel::Result::Error:
        if (m_state == State::Closing) {
            forceCloseCurrent(m_notifyOnClose);
            return;
        }
        failAndClose({SshError::Domain::Shell, m_api.errorCode(
                          m_connection->sessionForWorker()),
                      errorMessage, true});
        return;
    }
}

bool SshWorker::isOwnerThread() const
{
    return thread() == QThread::currentThread();
}

void SshWorker::assertOwnerThread() const
{
    if (!isOwnerThread()) {
        qFatal("SshWorker called from non-owner thread");
    }
}

void SshWorker::schedule(int delayMs)
{
    if (!m_timer->isActive()) {
        m_timer->start(delayMs);
    }
}

void SshWorker::scheduleAgain()
{
    const int delay = m_state == State::Ready
        ? ReadyPollDelayMs
        : m_againBackoffMs;
    schedule(delay);
    if (m_state != State::Ready) {
        m_againBackoffMs =
            qMin(m_againBackoffMs * 2, MaxAgainBackoffMs);
    }
}

void SshWorker::resetBackoff()
{
    m_againBackoffMs = MinAgainBackoffMs;
}

void SshWorker::handleConnectionFinished(quint64 generation,
                                         const SshError &error)
{
    assertOwnerThread();
    if (generation != m_generation || m_state != State::Connecting
        || m_connection == nullptr) {
        return;
    }
    if (error.isError()) {
        emit connectionFailed(generation, error);
        forceCloseCurrent(true);
        return;
    }

    m_shell = std::make_unique<ShellChannel>(
        m_api, m_connection->sessionForWorker(), m_initialCols,
        m_initialRows);
    m_state = State::OpeningShell;
    resetBackoff();
    schedule();
}

void SshWorker::failAndClose(const SshError &error)
{
    assertOwnerThread();
    emit connectionFailed(m_generation, error);
    forceCloseCurrent(true);
}

void SshWorker::requestClose(bool notify)
{
    assertOwnerThread();
    if (m_state == State::Idle && m_connection == nullptr
        && m_shell == nullptr) {
        return;
    }

    m_notifyOnClose = m_notifyOnClose || notify;
    m_state = State::Closing;
    m_timer->stop();
    resetBackoff();
    if (m_sftp != nullptr) {
        QList<SftpResult> results;
        m_sftp->close(&results);
        emitSftpResults(results);
        m_sftp.reset();
    }
    m_stats.reset();
    if (m_shell != nullptr) {
        m_shell->close();
        schedule();
        return;
    }
    forceCloseCurrent(m_notifyOnClose);
}

void SshWorker::forceCloseCurrent(bool notify)
{
    assertOwnerThread();
    if (m_state == State::Idle && m_connection == nullptr
        && m_shell == nullptr) {
        return;
    }

    const quint64 closingGeneration = m_generation;
    m_timer->stop();
    if (m_sftp != nullptr) {
        QList<SftpResult> results;
        m_sftp->close(&results);
        emitSftpResults(results);
        m_sftp.reset();
    }
    // stats 通道要在 session 释放前释放（freeChannel 依赖会话）。
    m_stats.reset();
    m_shell.reset();
    if (m_connection != nullptr) {
        disconnect(m_connection.get(), nullptr, this, nullptr);
        m_connection->close();
        m_connection.reset();
    }
    m_state = State::Idle;
    resetBackoff();
    if (notify && !m_closeNotified) {
        m_closeNotified = true;
        emit connectionClosed(closingGeneration);
    }
    m_notifyOnClose = false;
}

void SshWorker::serviceSftp()
{
    if (m_sftp == nullptr) {
        return;
    }
    QList<SftpResult> results;
    m_sftp->advance(&results);
    emitSftpResults(results);
}

void SshWorker::emitSftpResults(const QList<SftpResult> &results)
{
    for (const SftpResult &result : results) {
        emit sftpResult(m_generation, result);
    }
}

void SshWorker::serviceStats()
{
    if (m_stats == nullptr) {
        return;
    }
    QList<QString> samples;
    bool degraded = false;
    bool recovered = false;
    m_stats->advance(&samples, &degraded, &recovered);
    if (degraded) {
        emit statsDegraded(m_generation);
    }
    for (const QString &sample : samples) {
        emit statsSample(m_generation, sample);
    }
}
