#pragma once

#include "libsshapi.h"
#include "sshtypes.h"
#include "sshtarget.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>

class KnownHostsStore;
class SshWorker;

class LibsshConnection final : public QObject
{
    Q_OBJECT

public:
    explicit LibsshConnection(LibsshApi &api, KnownHostsStore &knownHosts,
                              QObject *parent = nullptr);
    ~LibsshConnection() override;

    void begin(const SshTarget &target);
    void advance();
    void submitHostKeyDecision(SshRequestId requestId, bool accept);
    void submitAuthAnswers(SshRequestId requestId,
                           const QStringList &answers);
    SshError sendKeepalive();
    void close();

    SshPhase phase() const { return m_phase; }

signals:
    void phaseChanged(SshPhase phase);
    void hostKeyDecisionRequired(const SshHostKeyChange &change);
    void unknownHostKeyRecorded(const QString &host);
    void authAnswersRequired(SshRequestId requestId,
                             const QList<SshAuthPrompt> &prompts);
    void finished(const SshError &error);

private:
    enum class Step {
        Idle,
        Configure,
        Connect,
        VerifyHost,
        AcceptChangedHost,
        WaitForHostDecision,
        AuthAgent,
        AuthExplicitKey,
        AuthAutomaticKey,
        AuthKeyboardInteractive,
        WaitForAuthAnswers,
        AuthPassword,
        Complete,
        Closed
    };

    friend class SshWorker;
    ssh_session sessionForWorker() const { return m_session; }

    bool setStringOption(enum ssh_options_e option, const QString &value);
    bool setUnsignedOption(enum ssh_options_e option, unsigned int value);
    bool setBoolOption(enum ssh_options_e option, bool value);
    void beginOperation();
    void endOperation();
    void drainDeferredBegins();
    void initializeBegin(const SshTarget &target, QByteArray password);
    void configure();
    void verifyHost();
    bool validateUpdatedHostKey(QString *error) const;
    bool normalizeCurrentHostKeys(QString *error);
    void handleAuthResult(int result, Step nextStep);
    void setPhase(SshPhase phase);
    void fail(SshError::Domain domain, const QString &message,
              bool retryable = false, int code = SSH_ERROR);
    void complete(const SshError &error);
    SshError apiError(SshError::Domain domain, bool retryable) const;
    void releaseSession();
    void clearSecrets();

    LibsshApi &m_api;
    KnownHostsStore &m_knownHosts;
    ssh_session m_session = nullptr;
    SshTarget m_target;
    QByteArray m_password;
    QList<QByteArray> m_authAnswers;
    SshPhase m_phase = SshPhase::Idle;
    Step m_step = Step::Idle;
    SshRequestId m_nextRequestId = 1;
    SshRequestId m_pendingRequestId = 0;
    QList<SshAuthPrompt> m_pendingPrompts;
    QString m_currentFingerprint;
    QString m_serverKeyType;
    QString m_effectiveHost;
    quint16 m_effectivePort = 0;
    quint16 m_legacyPort = 0;
    QString m_pendingLegacyFingerprint;
    int m_operationDepth = 0;
    bool m_drainingBegins = false;
    bool m_hasDeferredBegin = false;
    SshTarget m_deferredTarget;
    QByteArray m_deferredPassword;
    bool m_finished = false;
};
