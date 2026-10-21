#include "libsshapi.h"

#include <QByteArray>
#include <QFileInfo>

namespace {

QString fingerprintForKey(ssh_key key)
{
    if (key == nullptr) {
        return {};
    }

    unsigned char *hash = nullptr;
    size_t length = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash,
                               &length) != SSH_OK) {
        return {};
    }
    const QString fingerprint = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(hash),
                   qsizetype(length)).toHex());
    ssh_clean_pubkey_hash(&hash);
    return fingerprint;
}

} // namespace

bool LibsshApi::validateKnownHostKey(
    const QByteArray &algorithm,
    const QByteArray &encodedKey) const
{
    const enum ssh_keytypes_e keyType =
        ssh_key_type_from_name(algorithm.constData());
    if (keyType == SSH_KEYTYPE_UNKNOWN) {
        return false;
    }

    ssh_key key = nullptr;
    const int result = ssh_pki_import_pubkey_base64(
        encodedKey.constData(), keyType, &key);
    const bool valid = result == SSH_OK && key != nullptr;
    if (key != nullptr) {
        ssh_key_free(key);
    }
    return valid;
}

ssh_session LibsshApi::newSession()
{
    return ssh_new();
}

void LibsshApi::freeSession(ssh_session session)
{
    m_authenticationErrors.remove(session);
    ssh_free(session);
}

int LibsshApi::setOptionString(ssh_session session,
                               enum ssh_options_e option,
                               const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    return ssh_options_set(session, option, bytes.constData());
}

int LibsshApi::setOptionUnsigned(ssh_session session,
                                 enum ssh_options_e option,
                                 unsigned int value)
{
    return ssh_options_set(session, option, &value);
}

int LibsshApi::setOptionBool(ssh_session session, enum ssh_options_e option,
                             bool value)
{
    const int enabled = value ? 1 : 0;
    return ssh_options_set(session, option, &enabled);
}

int LibsshApi::parseConfig(ssh_session session)
{
    return ssh_options_parse_config(session, nullptr);
}

QString LibsshApi::effectiveHost(ssh_session session)
{
    char *host = nullptr;
    const int optionResult =
        ssh_options_get(session, SSH_OPTIONS_HOST, &host);
    if (optionResult != SSH_OK || host == nullptr) {
        if (host != nullptr) {
            ssh_string_free_char(host);
        }
        return {};
    }
    const QString result = QString::fromUtf8(host);
    ssh_string_free_char(host);
    return result;
}

quint16 LibsshApi::effectivePort(ssh_session session)
{
    unsigned int port = 0;
    if (ssh_options_get_port(session, &port) != SSH_OK
        || port > 65535) {
        return 0;
    }
    return static_cast<quint16>(port);
}

void LibsshApi::setBlocking(ssh_session session, bool blocking)
{
    ssh_set_blocking(session, blocking ? 1 : 0);
}

int LibsshApi::connect(ssh_session session)
{
    return ssh_connect(session);
}

enum ssh_known_hosts_e LibsshApi::knownHostState(ssh_session session)
{
    return ssh_session_is_known_server(session);
}

int LibsshApi::updateKnownHosts(ssh_session session)
{
    return ssh_session_update_known_hosts(session);
}

QString LibsshApi::serverFingerprintSha256(ssh_session session)
{
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(session, &key) != SSH_OK) {
        if (key != nullptr) {
            ssh_key_free(key);
        }
        return {};
    }
    const QString fingerprint = fingerprintForKey(key);
    ssh_key_free(key);
    return fingerprint;
}

QString LibsshApi::serverHostKeyType(ssh_session session)
{
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(session, &key) != SSH_OK
        || key == nullptr) {
        if (key != nullptr) {
            ssh_key_free(key);
        }
        return {};
    }
    const char *typeName = ssh_key_type_to_char(ssh_key_type(key));
    const QString result =
        QString::fromLatin1(typeName == nullptr ? "" : typeName);
    ssh_key_free(key);
    return result;
}

int LibsshApi::authAgent(ssh_session session)
{
    m_authenticationErrors.remove(session);
    return ssh_userauth_agent(session, nullptr);
}

int LibsshApi::authExplicitKey(ssh_session session, const QString &path,
                               const QByteArray &passphrase)
{
    m_authenticationErrors.remove(session);
    const QByteArray pathBytes = path.toUtf8();
    ssh_key privateKey = nullptr;
    const int importResult = ssh_pki_import_privkey_file(
        pathBytes.constData(),
        passphrase.isEmpty() ? nullptr : passphrase.constData(),
        nullptr, nullptr, &privateKey);
    if (importResult != SSH_OK || privateKey == nullptr) {
        if (privateKey != nullptr) {
            ssh_key_free(privateKey);
        }
        const QFileInfo keyFile(path);
        if (!keyFile.exists()) {
            m_authenticationErrors[session] =
                QStringLiteral("Private key file does not exist: %1")
                    .arg(path);
        } else if (!keyFile.isReadable()) {
            m_authenticationErrors[session] =
                QStringLiteral("Private key file is not readable: %1")
                    .arg(path);
        } else {
            m_authenticationErrors[session] =
                QStringLiteral(
                    "Private key is invalid or could not be decrypted: %1")
                    .arg(path);
        }
        return SSH_AUTH_ERROR;
    }
    const int result = ssh_userauth_publickey(session, nullptr, privateKey);
    ssh_key_free(privateKey);
    return result;
}

QString LibsshApi::authenticationErrorString(ssh_session session) const
{
    const QString localError = m_authenticationErrors.value(session);
    return localError.isEmpty()
        ? errorString(session)
        : localError;
}

int LibsshApi::authAutomaticPublicKey(ssh_session session,
                                      const QByteArray &passphrase)
{
    m_authenticationErrors.remove(session);
    return ssh_userauth_publickey_auto(
        session, nullptr,
        passphrase.isEmpty() ? nullptr : passphrase.constData());
}

int LibsshApi::authKeyboardInteractive(ssh_session session)
{
    m_authenticationErrors.remove(session);
    return ssh_userauth_kbdint(session, nullptr, nullptr);
}

QList<SshAuthPrompt>
LibsshApi::keyboardInteractivePrompts(ssh_session session)
{
    QList<SshAuthPrompt> prompts;
    const int count = ssh_userauth_kbdint_getnprompts(session);
    if (count <= 0) {
        return prompts;
    }
    prompts.reserve(count);
    for (int i = 0; i < count; ++i) {
        char echo = 0;
        const char *text = ssh_userauth_kbdint_getprompt(
            session, static_cast<unsigned int>(i), &echo);
        prompts.append(
            {QString::fromUtf8(text == nullptr ? "" : text), echo != 0});
    }
    return prompts;
}

int LibsshApi::setKeyboardInteractiveAnswer(ssh_session session,
                                            unsigned int index,
                                            const QByteArray &answer)
{
    return ssh_userauth_kbdint_setanswer(session, index,
                                         answer.constData());
}

int LibsshApi::authPassword(ssh_session session,
                            const QByteArray &password)
{
    m_authenticationErrors.remove(session);
    return ssh_userauth_password(session, nullptr, password.constData());
}

ssh_channel LibsshApi::newChannel(ssh_session session)
{
    return ssh_channel_new(session);
}

void LibsshApi::freeChannel(ssh_channel channel)
{
    ssh_channel_free(channel);
}

int LibsshApi::openChannelSession(ssh_channel channel)
{
    return ssh_channel_open_session(channel);
}

int LibsshApi::requestPtySize(ssh_channel channel, const QByteArray &term,
                              int cols, int rows)
{
    return ssh_channel_request_pty_size(channel, term.constData(), cols,
                                        rows);
}

int LibsshApi::requestEnvironment(ssh_channel channel,
                                  const QByteArray &name,
                                  const QByteArray &value)
{
    return ssh_channel_request_env(channel, name.constData(),
                                   value.constData());
}

int LibsshApi::requestShell(ssh_channel channel)
{
    return ssh_channel_request_shell(channel);
}

int LibsshApi::writeChannel(ssh_channel channel, const char *data,
                            quint32 size)
{
    return ssh_channel_write(channel, data, size);
}

int LibsshApi::readChannelNonblocking(ssh_channel channel, char *data,
                                      quint32 size, bool stderrStream)
{
    return ssh_channel_read_nonblocking(channel, data, size,
                                        stderrStream ? 1 : 0);
}

int LibsshApi::changePtySize(ssh_channel channel, int cols, int rows)
{
    return ssh_channel_change_pty_size(channel, cols, rows);
}

bool LibsshApi::channelIsEof(ssh_channel channel)
{
    return ssh_channel_is_eof(channel) != 0;
}

int LibsshApi::sendChannelEof(ssh_channel channel)
{
    return ssh_channel_send_eof(channel);
}

int LibsshApi::closeChannel(ssh_channel channel)
{
    return ssh_channel_close(channel);
}

int LibsshApi::sendKeepalive(ssh_session session)
{
    return ssh_send_ignore(session, nullptr);
}

void LibsshApi::disconnect(ssh_session session)
{
    ssh_disconnect(session);
}

QString LibsshApi::errorString(ssh_session session) const
{
    const char *message = ssh_get_error(session);
    return QString::fromUtf8(message == nullptr ? "" : message);
}

int LibsshApi::errorCode(ssh_session session) const
{
    return ssh_get_error_code(session);
}
