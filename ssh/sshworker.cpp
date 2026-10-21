#include "sshworker.h"

#include "knownhostsstore.h"
#include "libsshapi.h"
#include "libsshconnection.h"
#include "shellchannel.h"

#include <QThread>

SshWorker::SshWorker(LibsshApi &api, KnownHostsStore &knownHosts,
                     QObject *parent)
    : QObject(parent), m_api(api), m_knownHosts(knownHosts)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &SshWorker::advance);
}

SshWorker::~SshWorker()
{
    assertOwnerThread();
    closeCurrent(false);
}

void SshWorker::connectToHost(quint64 generation, const SshTarget &target,
                              int cols, int rows)
{
    assertOwnerThread();
    m_timer->stop();
    closeCurrent(true);

    m_generation = generation;
    m_initialCols = qMax(1, cols);
    m_initialRows = qMax(1, rows);
    m_closeNotified = false;
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
    schedule();
}

void SshWorker::writeShell(quint64 generation, const QByteArray &data)
{
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
    assertOwnerThread();
    if (generation != m_generation || m_state == State::Idle) {
        return;
    }
    closeCurrent(true);
}

void SshWorker::submitHostKeyDecision(quint64 generation,
                                      SshRequestId requestId, bool accept)
{
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
    assertOwnerThread();
    if (generation != m_generation || m_state != State::Connecting
        || m_connection == nullptr) {
        return;
    }
    m_connection->submitAuthAnswers(requestId, answers);
    schedule();
}

void SshWorker::advance()
{
    assertOwnerThread();
    if (m_state == State::Connecting) {
        if (m_connection != nullptr) {
            m_connection->advance();
        }
        if (m_state == State::Connecting) {
            schedule();
        } else if (m_state == State::OpeningShell) {
            schedule();
        }
        return;
    }

    if ((m_state != State::OpeningShell && m_state != State::Ready)
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

    switch (result) {
    case ShellChannel::Result::Ready:
        if (m_state == State::OpeningShell) {
            m_state = State::Ready;
            emit phaseChanged(m_generation, SshPhase::Ready);
            emit connected(m_generation);
        }
        schedule();
        return;
    case ShellChannel::Result::Progress:
        schedule();
        return;
    case ShellChannel::Result::Again:
        schedule(m_state == State::Ready ? 5 : 0);
        return;
    case ShellChannel::Result::Closed:
        closeCurrent(true);
        return;
    case ShellChannel::Result::Error:
        failAndClose({SshError::Domain::Shell, m_api.errorCode(
                          m_connection->sessionForWorker()),
                      errorMessage, true});
        return;
    }
}

void SshWorker::assertOwnerThread() const
{
    Q_ASSERT(thread() == QThread::currentThread());
}

void SshWorker::schedule(int delayMs)
{
    if (!m_timer->isActive()) {
        m_timer->start(delayMs);
    }
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
        closeCurrent(true);
        return;
    }

    m_shell = std::make_unique<ShellChannel>(
        m_api, m_connection->sessionForWorker(), m_initialCols,
        m_initialRows);
    m_state = State::OpeningShell;
    schedule();
}

void SshWorker::failAndClose(const SshError &error)
{
    assertOwnerThread();
    emit connectionFailed(m_generation, error);
    closeCurrent(true);
}

void SshWorker::closeCurrent(bool notify)
{
    assertOwnerThread();
    if (m_state == State::Idle && m_connection == nullptr
        && m_shell == nullptr) {
        return;
    }

    const quint64 closingGeneration = m_generation;
    m_state = State::Closing;
    m_timer->stop();
    m_shell.reset();
    if (m_connection != nullptr) {
        disconnect(m_connection.get(), nullptr, this, nullptr);
        m_connection->close();
        m_connection.reset();
    }
    m_state = State::Idle;
    if (notify && !m_closeNotified) {
        m_closeNotified = true;
        emit connectionClosed(closingGeneration);
    }
}
