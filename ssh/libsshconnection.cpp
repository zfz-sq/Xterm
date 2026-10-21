#include "libsshconnection.h"

#include "knownhostsstore.h"

#include <utility>

LibsshConnection::LibsshConnection(LibsshApi &api,
                                   KnownHostsStore &knownHosts,
                                   QObject *parent)
    : QObject(parent), m_api(api), m_knownHosts(knownHosts)
{
}

LibsshConnection::~LibsshConnection()
{
    releaseSession();
    clearSecrets();
}

void LibsshConnection::begin(const SshTarget &target)
{
    releaseSession();
    clearSecrets();

    m_target = target;
    m_password = target.password.toUtf8();
    m_target.password.clear();
    m_finished = false;
    m_pendingRequestId = 0;
    m_pendingPrompts.clear();
    m_currentFingerprint.clear();

    m_session = m_api.newSession();
    if (m_session == nullptr) {
        fail(SshError::Domain::Internal,
             QStringLiteral("Cannot allocate libssh session"));
        return;
    }

    m_step = Step::Configure;
    setPhase(SshPhase::ResolvingConfig);
}

void LibsshConnection::advance()
{
    if (m_finished || m_session == nullptr) {
        return;
    }

    switch (m_step) {
    case Step::Configure:
        configure();
        return;
    case Step::Connect: {
        const int result = m_api.connect(m_session);
        if (result == SSH_AGAIN) {
            return;
        }
        if (result != SSH_OK) {
            const SshError error = apiError(SshError::Domain::Network, true);
            fail(error.domain, error.message, error.retryable, error.code);
            return;
        }
        m_step = Step::VerifyHost;
        setPhase(SshPhase::VerifyingHost);
        return;
    }
    case Step::VerifyHost:
        verifyHost();
        return;
    case Step::AcceptChangedHost:
        if (m_api.updateKnownHosts(m_session) != SSH_OK) {
            const SshError error = apiError(SshError::Domain::HostKey, false);
            fail(error.domain, error.message, error.retryable, error.code);
            return;
        }
        m_step = Step::AuthAgent;
        setPhase(SshPhase::Authenticating);
        return;
    case Step::WaitForHostDecision:
    case Step::WaitForAuthAnswers:
        return;
    case Step::AuthAgent:
        handleAuthResult(m_api.authAgent(m_session),
                         Step::AuthExplicitKey);
        return;
    case Step::AuthExplicitKey:
        if (m_target.privateKey.isEmpty()) {
            m_step = Step::AuthAutomaticKey;
            return;
        }
        handleAuthResult(
            m_api.authExplicitKey(m_session, m_target.privateKey,
                                  m_password),
            Step::AuthAutomaticKey);
        return;
    case Step::AuthAutomaticKey:
        handleAuthResult(
            m_api.authAutomaticPublicKey(m_session, m_password),
            Step::AuthKeyboardInteractive);
        return;
    case Step::AuthKeyboardInteractive: {
        const int result = m_api.authKeyboardInteractive(m_session);
        if (result == SSH_AUTH_AGAIN) {
            return;
        }
        if (result == SSH_AUTH_INFO) {
            m_pendingPrompts =
                m_api.keyboardInteractivePrompts(m_session);
            if (m_pendingPrompts.isEmpty()) {
                fail(SshError::Domain::Authentication,
                     QStringLiteral(
                         "Keyboard-interactive requested no prompts"));
                return;
            }
            m_pendingRequestId = m_nextRequestId++;
            m_step = Step::WaitForAuthAnswers;
            emit authAnswersRequired(m_pendingRequestId, m_pendingPrompts);
            return;
        }
        handleAuthResult(result, Step::AuthPassword);
        return;
    }
    case Step::AuthPassword:
        handleAuthResult(m_api.authPassword(m_session, m_password),
                         Step::Complete);
        return;
    case Step::Idle:
    case Step::Complete:
    case Step::Closed:
        return;
    }
}

void LibsshConnection::submitHostKeyDecision(SshRequestId requestId,
                                              bool accept)
{
    if (m_finished || m_step != Step::WaitForHostDecision
        || requestId != m_pendingRequestId) {
        return;
    }
    m_pendingRequestId = 0;
    if (!accept) {
        fail(SshError::Domain::HostKey,
             QStringLiteral("Changed host key was rejected"));
        return;
    }
    m_step = Step::AcceptChangedHost;
}

void LibsshConnection::submitAuthAnswers(SshRequestId requestId,
                                         const QStringList &answers)
{
    if (m_finished || m_step != Step::WaitForAuthAnswers
        || requestId != m_pendingRequestId) {
        return;
    }
    if (answers.size() != m_pendingPrompts.size()) {
        fail(SshError::Domain::Authentication,
             QStringLiteral(
                 "Keyboard-interactive answer count does not match prompts"));
        return;
    }

    m_authAnswers.reserve(answers.size());
    for (const QString &answer : answers) {
        m_authAnswers.append(answer.toUtf8());
    }
    for (qsizetype i = 0; i < m_authAnswers.size(); ++i) {
        const QByteArray &answer = m_authAnswers.at(i);
        const int result = m_api.setKeyboardInteractiveAnswer(
            m_session, static_cast<unsigned int>(i), answer);
        if (result != SSH_OK) {
            const SshError error =
                apiError(SshError::Domain::Authentication, false);
            fail(error.domain, error.message, error.retryable, error.code);
            return;
        }
    }

    for (QByteArray &answer : m_authAnswers) {
        answer.fill('\0');
    }
    m_authAnswers.clear();
    m_pendingPrompts.clear();
    m_pendingRequestId = 0;
    m_step = Step::AuthKeyboardInteractive;
}

SshError LibsshConnection::sendKeepalive()
{
    if (m_session == nullptr || m_step != Step::Complete) {
        return {SshError::Domain::Network, SSH_ERROR,
                QStringLiteral("SSH session is not connected"), true};
    }

    const int result = m_api.sendKeepalive(m_session);
    if (result == SSH_OK || result == SSH_AGAIN) {
        return {};
    }
    return apiError(SshError::Domain::Network, true);
}

void LibsshConnection::close()
{
    if (m_step == Step::Closed) {
        return;
    }

    const bool shouldReportCancellation = !m_finished;
    releaseSession();
    clearSecrets();
    m_step = Step::Closed;
    setPhase(SshPhase::Closing);
    if (shouldReportCancellation) {
        complete({SshError::Domain::Cancelled, 0,
                  QStringLiteral("SSH connection cancelled"), false});
    }
}

bool LibsshConnection::setStringOption(enum ssh_options_e option,
                                       const QString &value)
{
    if (m_api.setOptionString(m_session, option, value) == SSH_OK) {
        return true;
    }
    const SshError error =
        apiError(SshError::Domain::Configuration, false);
    fail(error.domain, error.message, error.retryable, error.code);
    return false;
}

bool LibsshConnection::setUnsignedOption(enum ssh_options_e option,
                                         unsigned int value)
{
    if (m_api.setOptionUnsigned(m_session, option, value) == SSH_OK) {
        return true;
    }
    const SshError error =
        apiError(SshError::Domain::Configuration, false);
    fail(error.domain, error.message, error.retryable, error.code);
    return false;
}

bool LibsshConnection::setBoolOption(enum ssh_options_e option, bool value)
{
    if (m_api.setOptionBool(m_session, option, value) == SSH_OK) {
        return true;
    }
    const SshError error =
        apiError(SshError::Domain::Configuration, false);
    fail(error.domain, error.message, error.retryable, error.code);
    return false;
}

void LibsshConnection::configure()
{
    QString migrationError;
    if (!m_knownHosts.prepareMigration(&migrationError)) {
        fail(SshError::Domain::Configuration, migrationError);
        return;
    }
    if (m_target.host.isEmpty()) {
        fail(SshError::Domain::Configuration,
             QStringLiteral("SSH host is empty"));
        return;
    }
    if (!setStringOption(SSH_OPTIONS_HOST, m_target.host)) {
        return;
    }
    if (m_api.parseConfig(m_session) != SSH_OK) {
        const SshError error =
            apiError(SshError::Domain::Configuration, false);
        fail(error.domain, error.message, error.retryable, error.code);
        return;
    }

    if (!m_target.user.isEmpty()
        && !setStringOption(SSH_OPTIONS_USER, m_target.user)) {
        return;
    }
    if (!m_target.privateKey.isEmpty()
        && !setStringOption(SSH_OPTIONS_IDENTITY, m_target.privateKey)) {
        return;
    }
    if (m_target.port > 0 && m_target.port <= 65535
        && !setUnsignedOption(SSH_OPTIONS_PORT,
                              static_cast<unsigned int>(m_target.port))) {
        return;
    }
    if (m_target.connectTimeoutSec > 0
        && !setUnsignedOption(
            SSH_OPTIONS_TIMEOUT,
            static_cast<unsigned int>(m_target.connectTimeoutSec))) {
        return;
    }

    // Configuration was parsed explicitly above. Disable the implicit second
    // parse during ssh_connect(), otherwise it could overwrite UI values.
    if (!setBoolOption(SSH_OPTIONS_PROCESS_CONFIG, false)
        || !setStringOption(SSH_OPTIONS_KNOWNHOSTS, m_knownHosts.path())) {
        return;
    }

    m_api.setBlocking(m_session, false);
    m_step = Step::Connect;
    setPhase(SshPhase::Connecting);
}

void LibsshConnection::verifyHost()
{
    const enum ssh_known_hosts_e state =
        m_api.knownHostState(m_session);
    m_currentFingerprint =
        m_api.serverFingerprintSha256(m_session);
    if (m_currentFingerprint.isEmpty()) {
        fail(SshError::Domain::HostKey,
             QStringLiteral("Cannot calculate server host-key fingerprint"));
        return;
    }

    if (state == SSH_KNOWN_HOSTS_OK) {
        m_step = Step::AuthAgent;
        setPhase(SshPhase::Authenticating);
        return;
    }

    const QString legacyFingerprint =
        m_knownHosts.legacyFingerprint(
            m_target.host, static_cast<quint16>(m_target.port));
    if (state == SSH_KNOWN_HOSTS_UNKNOWN
        || state == SSH_KNOWN_HOSTS_NOT_FOUND) {
        if (legacyFingerprint.isEmpty()) {
            if (m_api.updateKnownHosts(m_session) != SSH_OK) {
                const SshError error =
                    apiError(SshError::Domain::HostKey, false);
                fail(error.domain, error.message, error.retryable,
                     error.code);
                return;
            }
            m_step = Step::AuthAgent;
            setPhase(SshPhase::Authenticating);
            return;
        }

        if (legacyFingerprint.compare(m_currentFingerprint,
                                      Qt::CaseInsensitive) == 0) {
            if (m_api.updateKnownHosts(m_session) != SSH_OK) {
                const SshError error =
                    apiError(SshError::Domain::HostKey, false);
                fail(error.domain, error.message, error.retryable,
                     error.code);
                return;
            }
            QString migrationError;
            if (!m_knownHosts.rememberLegacyMatch(
                    m_target.host, static_cast<quint16>(m_target.port),
                    m_currentFingerprint, &migrationError)) {
                fail(SshError::Domain::HostKey, migrationError);
                return;
            }
            m_step = Step::AuthAgent;
            setPhase(SshPhase::Authenticating);
            return;
        }

        m_pendingRequestId = m_nextRequestId++;
        m_step = Step::WaitForHostDecision;
        emit hostKeyDecisionRequired(
            {m_pendingRequestId, m_target.host, legacyFingerprint,
             m_currentFingerprint});
        return;
    }

    if (state == SSH_KNOWN_HOSTS_CHANGED
        || state == SSH_KNOWN_HOSTS_OTHER) {
        m_pendingRequestId = m_nextRequestId++;
        m_step = Step::WaitForHostDecision;
        emit hostKeyDecisionRequired(
            {m_pendingRequestId, m_target.host,
             m_api.knownHostFingerprintSha256(m_session),
             m_currentFingerprint});
        return;
    }

    const SshError error = apiError(SshError::Domain::HostKey, false);
    fail(error.domain, error.message, error.retryable, error.code);
}

void LibsshConnection::handleAuthResult(int result, Step nextStep)
{
    if (result == SSH_AUTH_AGAIN) {
        return;
    }
    if (result == SSH_AUTH_SUCCESS) {
        m_step = Step::Complete;
        setPhase(SshPhase::OpeningShell);
        complete({});
        return;
    }
    if (result == SSH_AUTH_ERROR) {
        const SshError error =
            apiError(SshError::Domain::Authentication, false);
        fail(error.domain, error.message, error.retryable, error.code);
        return;
    }
    m_step = nextStep;
    if (nextStep == Step::Complete) {
        fail(SshError::Domain::Authentication,
             QStringLiteral("All SSH authentication methods were rejected"));
    }
}

void LibsshConnection::setPhase(SshPhase phase)
{
    if (m_phase == phase) {
        return;
    }
    m_phase = phase;
    emit phaseChanged(phase);
}

void LibsshConnection::fail(SshError::Domain domain,
                            const QString &message, bool retryable, int code)
{
    if (m_finished) {
        return;
    }
    releaseSession();
    m_step = Step::Closed;
    setPhase(SshPhase::Closing);
    complete({domain, code, message, retryable});
}

void LibsshConnection::complete(const SshError &error)
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    clearSecrets();
    emit finished(error);
}

SshError LibsshConnection::apiError(SshError::Domain domain,
                                    bool retryable) const
{
    return {domain, m_api.errorCode(m_session),
            m_api.errorString(m_session), retryable};
}

void LibsshConnection::releaseSession()
{
    if (m_session == nullptr) {
        return;
    }
    m_api.disconnect(m_session);
    m_api.freeSession(m_session);
    m_session = nullptr;
}

void LibsshConnection::clearSecrets()
{
    m_password.fill('\0');
    m_password.clear();
    for (QByteArray &answer : m_authAnswers) {
        answer.fill('\0');
    }
    m_authAnswers.clear();
}
