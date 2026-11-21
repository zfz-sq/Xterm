#pragma once

#include "sftprequest.h"
#include "sshtypes.h"
#include "sshtarget.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

class KnownHostsStore;
class LibsshApi;
class LibsshConnection;
class SftpChannel;
class ShellChannel;
class StatsChannel;

class SshWorker final : public QObject
{
    Q_OBJECT

public:
    explicit SshWorker(LibsshApi &api, KnownHostsStore &knownHosts,
                       QObject *parent = nullptr);
    ~SshWorker() override;

    // 仅测试用：收紧死链探测节奏，避免真实 10s/30s 的等待。
    void setDeadLinkTimingForTest(int probeIntervalMs, int quietTimeoutMs);

public slots:
    void connectToHost(quint64 generation, const SshTarget &target,
                       int cols, int rows);
    void writeShell(quint64 generation, const QByteArray &data);
    void resizeShell(quint64 generation, int cols, int rows);
    void closeConnection(quint64 generation);
    void submitHostKeyDecision(quint64 generation, SshRequestId requestId,
                               bool accept);
    void submitAuthAnswers(quint64 generation, SshRequestId requestId,
                           const QStringList &answers);
    void submitSftpRequest(quint64 generation, const SftpRequest &request);
    void cancelSftpRequest(quint64 generation, SshRequestId requestId);
    void startStats(quint64 generation);
    void stopStats(quint64 generation);

signals:
    void connected(quint64 generation);
    void shellData(quint64 generation, const QByteArray &data);
    void phaseChanged(quint64 generation, SshPhase phase);
    void hostKeyDecisionRequired(quint64 generation,
                                 const SshHostKeyChange &change);
    void unknownHostKeyRecorded(quint64 generation, const QString &host);
    void authAnswersRequired(quint64 generation, SshRequestId requestId,
                             const QList<SshAuthPrompt> &prompts);
    void connectionFailed(quint64 generation, const SshError &error);
    void connectionClosed(quint64 generation);
    void sftpResult(quint64 generation, const SftpResult &result);
    void statsSample(quint64 generation, const QString &sample);
    void statsDegraded(quint64 generation);

private slots:
    void advance();

private:
    enum class State {
        Idle,
        Connecting,
        OpeningShell,
        Ready,
        Closing
    };

    static constexpr int ReadyPollDelayMs = 5;
    static constexpr int MaxReadyPollDelayMs = 25;
    static constexpr int MinAgainBackoffMs = 5;
    static constexpr int MaxAgainBackoffMs = 50;
    // 死链检测：设备掉电后的半开 TCP 上应用层读永远“无数据”，状态会
    // 永远卡在 Connected（此时点 SFTP 在死会话上开通道直接闪退）。
    // 引爆靠内核层 TCP keepalive（Ready 时启用，见 enableShortTcpKeepalive），
    // 裁决靠 ssh_get_status 纯查询；stats 入站帧停流（静默窗口）叠加兜底。
    static constexpr int StatusProbeIntervalMs = 10000;
    static constexpr int QuietLinkTimeoutMs = 15000;

    bool isOwnerThread() const;
    void assertOwnerThread() const;
    void schedule(int delayMs = 0);
    void scheduleAgain();
    void resetBackoff();
    void resetReadyIdlePoll();
    int nextReadyPollDelayMs();
    void markInboundProgress();
    void probeLink();
    void handleConnectionFinished(quint64 generation,
                                  const SshError &error);
    void failAndClose(const SshError &error);
    void requestClose(bool notify);
    void forceCloseCurrent(bool notify);
    bool serviceSftp();
    void emitSftpResults(const QList<SftpResult> &results);
    bool serviceStats();

    LibsshApi &m_api;
    KnownHostsStore &m_knownHosts;
    QTimer *m_timer = nullptr;
    std::unique_ptr<LibsshConnection> m_connection;
    std::unique_ptr<ShellChannel> m_shell;
    std::unique_ptr<SftpChannel> m_sftp;
    std::unique_ptr<StatsChannel> m_stats;
    quint64 m_generation = 0;
    int m_initialCols = 80;
    int m_initialRows = 24;
    int m_againBackoffMs = MinAgainBackoffMs;
    State m_state = State::Idle;
    int m_readyIdlePolls = 0;
    QElapsedTimer m_clock;
    int m_probeIntervalMs = StatusProbeIntervalMs;
    int m_quietTimeoutMs = QuietLinkTimeoutMs;
    qint64 m_lastProbeMs = 0;
    qint64 m_lastInboundMs = 0;
    bool m_statsStreamed = false;
    bool m_statsDegrading = false;
    bool m_closeNotified = false;
    bool m_notifyOnClose = false;
};
