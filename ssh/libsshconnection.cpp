#include "libsshconnection.h"

#include "knownhostsstore.h"

#include <QScopeGuard>

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
    m_deferredTarget.password.clear();
    m_deferredPassword.fill('\0');
    m_deferredPassword.clear();
}

void LibsshConnection::begin(const SshTarget &target)
{
    beginOperation();
    const auto operation = qScopeGuard([this] { endOperation(); });
    m_deferredPassword.fill('\0');
    m_deferredPassword.clear();
    m_deferredTarget.password.clear();
    m_deferredTarget = target;
    m_deferredPassword = target.password.toUtf8();
    m_deferredTarget.password.clear();
    m_hasDeferredBegin = true;
}

void LibsshConnection::beginOperation()
{
    ++m_operationDepth;
}

void LibsshConnection::endOperation()
{
    --m_operationDepth;
    if (m_operationDepth == 0) {
        drainDeferredBegins();
    }
}

void LibsshConnection::drainDeferredBegins()
{
    if (m_operationDepth != 0 || m_drainingBegins) {
        return;
    }

    m_drainingBegins = true;
    ++m_operationDepth;
    while (m_hasDeferredBegin) {
        SshTarget next = m_deferredTarget;
        QByteArray nextPassword = std::move(m_deferredPassword);
        m_deferredTarget.password.clear();
        m_hasDeferredBegin = false;

        if (m_session != nullptr && !m_finished) {
            close();
            if (m_hasDeferredBegin) {
                next.password.clear();
                nextPassword.fill('\0');
                nextPassword.clear();
                continue;
            }
        } else {
            releaseSession();
        }
        initializeBegin(next, std::move(nextPassword));
        next.password.clear();
    }
    --m_operationDepth;
    m_drainingBegins = false;
}

void LibsshConnection::initializeBegin(const SshTarget &target,
                                       QByteArray password)
{
    clearSecrets();
    m_target = target;
    m_password = std::move(password);
    m_target.password.clear();
    m_finished = false;
    m_pendingRequestId = 0;
    m_pendingPrompts.clear();
    m_currentFingerprint.clear();
    m_serverKeyType.clear();
    m_effectiveHost.clear();
    m_effectivePort = 0;
    m_legacyPort = 0;
    m_pendingLegacyFingerprint.clear();

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
    beginOperation();
    const auto operation = qScopeGuard([this] { endOperation(); });
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
    case Step::AcceptChangedHost: {
        if (m_api.updateKnownHosts(m_session) != SSH_OK) {
            const SshError error = apiError(SshError::Domain::HostKey, false);
            fail(error.domain, error.message, error.retryable, error.code);
            return;
        }
        QString validationError;
        if (!validateUpdatedHostKey(&validationError)
            || !normalizeCurrentHostKeys(&validationError)) {
            fail(SshError::Domain::HostKey, validationError);
            return;
        }
        if (!m_pendingLegacyFingerprint.isEmpty()) {
            QString migrationError;
            if (!m_knownHosts.consumeLegacyRecord(
                    m_target.host, m_legacyPort,
                    m_pendingLegacyFingerprint, m_effectiveHost,
                    m_effectivePort, m_serverKeyType,
                    m_currentFingerprint, &migrationError)) {
                fail(SshError::Domain::HostKey, migrationError);
                return;
            }
            m_pendingLegacyFingerprint.clear();
        }
        m_step = Step::AuthAgent;
        setPhase(SshPhase::Authenticating);
        return;
    }
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
    beginOperation();
    const auto operation = qScopeGuard([this] { endOperation(); });
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
    beginOperation();
    const auto operation = qScopeGuard([this] { endOperation(); });
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
    beginOperation();
    const auto operation = qScopeGuard([this] { endOperation(); });
    if (m_step == Step::Idle && m_session == nullptr) {
        return;
    }
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
        || !setBoolOption(SSH_OPTIONS_STRICTHOSTKEYCHECK, true)
        || !setStringOption(SSH_OPTIONS_KNOWNHOSTS, m_knownHosts.path())
        || !setStringOption(SSH_OPTIONS_GLOBAL_KNOWNHOSTS,
                            m_knownHosts.globalPath())) {
        return;
    }

    m_effectiveHost = m_api.effectiveHost(m_session).toLower();
    m_effectivePort = m_api.effectivePort(m_session);
    if (m_effectiveHost.isEmpty() || m_effectivePort == 0) {
        fail(SshError::Domain::Configuration,
             QStringLiteral("Cannot resolve effective SSH host and port"));
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
    m_serverKeyType = m_api.serverHostKeyType(m_session);
    if (m_serverKeyType.isEmpty()) {
        fail(SshError::Domain::HostKey,
             QStringLiteral("Cannot determine server host-key type"));
        return;
    }

    m_legacyPort =
        m_target.port >= 1 && m_target.port <= 65535
        ? static_cast<quint16>(m_target.port)
        : m_effectivePort;
    const QString legacyFingerprint =
        m_knownHosts.legacyFingerprint(m_target.host, m_legacyPort);
    if (state == SSH_KNOWN_HOSTS_OK) {
        QString normalizationError;
        if (!m_knownHosts.retryPendingNormalization(
                m_effectiveHost, m_effectivePort, m_serverKeyType,
                m_currentFingerprint, &normalizationError)) {
            fail(SshError::Domain::HostKey, normalizationError);
            return;
        }
        if (!legacyFingerprint.isEmpty()) {
            if (legacyFingerprint.compare(m_currentFingerprint,
                                          Qt::CaseInsensitive) != 0) {
                m_pendingRequestId = m_nextRequestId++;
                m_pendingLegacyFingerprint = legacyFingerprint;
                m_step = Step::WaitForHostDecision;
                emit hostKeyDecisionRequired(
                    {m_pendingRequestId, m_target.host,
                     legacyFingerprint, m_currentFingerprint});
                return;
            }
            QString migrationError;
            if (!m_knownHosts.consumeLegacyRecord(
                    m_target.host, m_legacyPort,
                    m_currentFingerprint, m_effectiveHost,
                    m_effectivePort, m_serverKeyType,
                    m_currentFingerprint, &migrationError)) {
                fail(SshError::Domain::HostKey, migrationError);
                return;
            }
        }
        m_step = Step::AuthAgent;
        setPhase(SshPhase::Authenticating);
        return;
    }

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
            QString validationError;
            if (!validateUpdatedHostKey(&validationError)) {
                fail(SshError::Domain::HostKey, validationError);
                return;
            }
            if (!normalizeCurrentHostKeys(&validationError)) {
                fail(SshError::Domain::HostKey, validationError);
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
            QString validationError;
            if (!validateUpdatedHostKey(&validationError)
                || !normalizeCurrentHostKeys(&validationError)) {
                fail(SshError::Domain::HostKey, validationError);
                return;
            }
            QString migrationError;
            if (!m_knownHosts.consumeLegacyRecord(
                    m_target.host, m_legacyPort,
                    m_currentFingerprint, m_effectiveHost,
                    m_effectivePort, m_serverKeyType,
                    m_currentFingerprint, &migrationError)) {
                fail(SshError::Domain::HostKey, migrationError);
                return;
            }
            m_step = Step::AuthAgent;
            setPhase(SshPhase::Authenticating);
            return;
        }

        m_pendingRequestId = m_nextRequestId++;
        m_pendingLegacyFingerprint = legacyFingerprint;
        m_step = Step::WaitForHostDecision;
        emit hostKeyDecisionRequired(
            {m_pendingRequestId, m_target.host, legacyFingerprint,
             m_currentFingerprint});
        return;
    }

    if (state == SSH_KNOWN_HOSTS_CHANGED) {
        QString fingerprintError;
        const QString oldFingerprint =
            m_knownHosts.standardFingerprint(
                m_effectiveHost, m_effectivePort, m_serverKeyType,
                &fingerprintError);
        if (oldFingerprint.isEmpty()) {
            fail(SshError::Domain::HostKey, fingerprintError);
            return;
        }
        m_pendingRequestId = m_nextRequestId++;
        m_step = Step::WaitForHostDecision;
        emit hostKeyDecisionRequired(
            {m_pendingRequestId, m_target.host,
             oldFingerprint, m_currentFingerprint});
        return;
    }

    if (state == SSH_KNOWN_HOSTS_OTHER) {
        QString keysError;
        const QList<KnownHostKeyInfo> keys =
            m_knownHosts.standardKeys(m_effectiveHost, m_effectivePort,
                                      &keysError);
        QStringList oldKeys;
        for (const KnownHostKeyInfo &key : keys) {
            if (key.keyType != m_serverKeyType) {
                oldKeys.append(
                    QStringLiteral("%1 %2")
                        .arg(key.keyType, key.fingerprint));
            }
        }
        if (oldKeys.isEmpty()) {
            fail(SshError::Domain::HostKey, keysError.isEmpty()
                     ? QStringLiteral(
                           "No different stored host key for %1:%2")
                           .arg(m_effectiveHost)
                           .arg(m_effectivePort)
                     : keysError);
            return;
        }
        m_pendingRequestId = m_nextRequestId++;
        m_step = Step::WaitForHostDecision;
        emit hostKeyDecisionRequired(
            {m_pendingRequestId, m_target.host,
             oldKeys.join(QLatin1Char('\n')), m_currentFingerprint});
        return;
    }

    const SshError error = apiError(SshError::Domain::HostKey, false);
    fail(error.domain, error.message, error.retryable, error.code);
}

bool LibsshConnection::validateUpdatedHostKey(QString *error) const
{
    return m_knownHosts.containsStandardKey(
        m_effectiveHost, m_effectivePort, m_serverKeyType,
        m_currentFingerprint, error);
}

bool LibsshConnection::normalizeCurrentHostKeys(QString *error)
{
    return m_knownHosts.normalizeHostKeys(
        m_effectiveHost, m_effectivePort, m_serverKeyType,
        m_currentFingerprint, error);
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
        SshError error =
            apiError(SshError::Domain::Authentication, false);
        error.message =
            m_api.authenticationErrorString(m_session);
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
