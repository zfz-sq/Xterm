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

    m_clock.start();
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
    m_statsStreamed = false;
    m_statsDegrading = false;
    m_lastProbeMs = m_clock.elapsed();
    m_lastInboundMs = m_lastProbeMs;
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
    m_statsStreamed = false;   // 静默判定重新武装：等下一帧真正到来
    m_statsDegrading = false;
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
    if (m_state == State::Ready) {
        probeLink();
        if (m_state != State::Ready) {
            return;   // 探测判死：failAndClose 已收尾连接与定时器
        }
    }
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
    if (!output.isEmpty()) {
        markInboundProgress();
    }

    bool sftpActive = false;
    bool statsProgress = false;
    if (m_state == State::Ready) {
        sftpActive = serviceSftp();
        statsProgress = serviceStats();
    }
    const bool backgroundActive = sftpActive || statsProgress;

    switch (result) {
    case ShellChannel::Result::Ready:
        if (m_state == State::OpeningShell) {
            m_state = State::Ready;
            emit phaseChanged(m_generation, SshPhase::Ready);
            emit connected(m_generation);
        }
        resetBackoff();
        resetReadyIdlePoll();
        schedule(m_state == State::Ready ? ReadyPollDelayMs : 0);
        return;
    case ShellChannel::Result::Progress:
        resetBackoff();
        // 任何推进（含 shell 数据）都算活动：空闲轮询从 5ms 重新爬坡，
        // 否则回显数据落地后下一次 Again 又立即回到 25ms 慢轮询。
        resetReadyIdlePoll();
        if (m_state == State::Closing) {
            schedule();
            return;
        }
        schedule(m_state == State::Ready ? ReadyPollDelayMs : 0);
        return;
    case ShellChannel::Result::Again:
        if (m_state == State::Ready && backgroundActive)
            resetReadyIdlePoll();
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
    if (!m_timer->isActive() || delayMs < m_timer->remainingTime()) {
        m_timer->start(delayMs);
    }
}

void SshWorker::scheduleAgain()
{
    const int delay = m_state == State::Ready
        ? nextReadyPollDelayMs()
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

void SshWorker::resetReadyIdlePoll()
{
    m_readyIdlePolls = 0;
}

int SshWorker::nextReadyPollDelayMs()
{
    const int delay = m_readyIdlePolls < 4
        ? ReadyPollDelayMs
        : (m_readyIdlePolls < 12 ? 10 : MaxReadyPollDelayMs);
    ++m_readyIdlePolls;
    return delay;
}

void SshWorker::setDeadLinkTimingForTest(int probeIntervalMs,
                                         int quietTimeoutMs)
{
    m_probeIntervalMs = probeIntervalMs;
    m_quietTimeoutMs = quietTimeoutMs;
}

void SshWorker::markInboundProgress()
{
    m_lastInboundMs = m_clock.elapsed();
}

void SshWorker::probeLink()
{
    const qint64 nowMs = m_clock.elapsed();
    if (nowMs - m_lastProbeMs >= m_probeIntervalMs) {
        m_lastProbeMs = nowMs;
        // 纯查询裁决：TCP keepalive（内核层）引爆后的 socket 错误由
        // ssh_get_status 报出（SSH_CLOSED(_ERROR)）。这里不做任何主动
        // 写——曾用 ssh_send_ignore 做探测，0.12.2 里它对 data 无 NULL
        // 防护（strlen(NULL) 直接闪退）、返回值也不可信（socket 已关
        // 仍返回“成功”），已彻底移除。
        const int status = m_api.getStatus(m_connection->sessionForWorker());
        if ((status & (SSH_CLOSED | SSH_CLOSED_ERROR)) != 0) {
            failAndClose({SshError::Domain::Network, SSH_ERROR,
                          QStringLiteral("SSH session closed unexpectedly"),
                          true});
            return;
        }
    }
    // 静默判定只对 stats 流产过帧且未在降级重试的会话生效：这类会话
    // 正常每 3s 必有入站帧，静默超过窗口即链路死亡（路由级静默丢包时
    // 内核 keepalive 也可能长时间报不出错）。stats 自身降级（exec 被拒、
    // 远端断流）不是链路死，排除以免误杀；空闲会话本来就没有入站流量，
    // 同样不武装。
    const bool statsStreaming = m_stats != nullptr && m_stats->isActive()
        && m_statsStreamed && !m_statsDegrading;
    if (statsStreaming && nowMs - m_lastInboundMs >= m_quietTimeoutMs) {
        failAndClose({SshError::Domain::Network, SSH_ERROR,
                      QStringLiteral("SSH connection is not responding"),
                      true});
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
        forceCloseCurrent(true);
        return;
    }

    m_shell = std::make_unique<ShellChannel>(
        m_api, m_connection->sessionForWorker(), m_initialCols,
        m_initialRows);
    // 内核层 TCP keepalive：断电后半开链路的引爆器，探测失败由内核
    // 直接置 socket 错误；失败（拿不到 fd 等）不影响会话，静默兜底仍在。
    m_api.enableShortTcpKeepalive(m_connection->sessionForWorker());
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

bool SshWorker::serviceSftp()
{
    if (m_sftp == nullptr) {
        return false;
    }
    QList<SftpResult> results;
    const SftpChannel::Result result = m_sftp->advance(&results);
    if (!results.isEmpty()) {
        markInboundProgress();
    }
    emitSftpResults(results);
    return result != SftpChannel::Result::Idle;
}

void SshWorker::emitSftpResults(const QList<SftpResult> &results)
{
    for (const SftpResult &result : results) {
        emit sftpResult(m_generation, result);
    }
}

bool SshWorker::serviceStats()
{
    if (m_stats == nullptr) {
        return false;
    }
    QList<QString> samples;
    bool degraded = false;
    bool recovered = false;
    const StatsChannel::Result result =
        m_stats->advance(&samples, &degraded, &recovered);
    if (degraded) {
        m_statsDegrading = true;
        emit statsDegraded(m_generation);
    }
    if (!samples.isEmpty()) {
        m_statsStreamed = true;
        m_statsDegrading = false;
        markInboundProgress();
    }
    for (const QString &sample : samples) {
        emit statsSample(m_generation, sample);
    }
    return result == StatsChannel::Result::Progress;
}
