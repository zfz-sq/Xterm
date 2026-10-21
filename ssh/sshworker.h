#pragma once

#include "sshtypes.h"
#include "sshtarget.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

class KnownHostsStore;
class LibsshApi;
class LibsshConnection;
class ShellChannel;

class SshWorker final : public QObject
{
    Q_OBJECT

public:
    explicit SshWorker(LibsshApi &api, KnownHostsStore &knownHosts,
                       QObject *parent = nullptr);
    ~SshWorker() override;

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

signals:
    void connected(quint64 generation);
    void shellData(quint64 generation, const QByteArray &data);
    void phaseChanged(quint64 generation, SshPhase phase);
    void hostKeyDecisionRequired(quint64 generation,
                                 const SshHostKeyChange &change);
    void authAnswersRequired(quint64 generation, SshRequestId requestId,
                             const QList<SshAuthPrompt> &prompts);
    void connectionFailed(quint64 generation, const SshError &error);
    void connectionClosed(quint64 generation);

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
    static constexpr int MinAgainBackoffMs = 5;
    static constexpr int MaxAgainBackoffMs = 50;

    bool isOwnerThread() const;
    void assertOwnerThread() const;
    void schedule(int delayMs = 0);
    void scheduleAgain();
    void resetBackoff();
    void handleConnectionFinished(quint64 generation,
                                  const SshError &error);
    void failAndClose(const SshError &error);
    void requestClose(bool notify);
    void forceCloseCurrent(bool notify);

    LibsshApi &m_api;
    KnownHostsStore &m_knownHosts;
    QTimer *m_timer = nullptr;
    std::unique_ptr<LibsshConnection> m_connection;
    std::unique_ptr<ShellChannel> m_shell;
    quint64 m_generation = 0;
    int m_initialCols = 80;
    int m_initialRows = 24;
    int m_againBackoffMs = MinAgainBackoffMs;
    State m_state = State::Idle;
    bool m_closeNotified = false;
    bool m_notifyOnClose = false;
};
