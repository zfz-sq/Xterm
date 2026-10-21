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

int LibsshApi::requestExec(ssh_channel channel, const QByteArray &command)
{
    return ssh_channel_request_exec(channel, command.constData());
}

int LibsshApi::channelGetExitStatus(ssh_channel channel)
{
    uint32_t exitCode = 0;
    if (ssh_channel_get_exit_state(channel, &exitCode, nullptr, nullptr)
        != SSH_OK) {
        return -1;
    }
    return int(exitCode);
}

sftp_session LibsshApi::sftpNew(ssh_session session)
{
    return sftp_new(session);
}

void LibsshApi::sftpFree(sftp_session sftp)
{
    sftp_free(sftp);
}

int LibsshApi::sftpInit(sftp_session sftp)
{
    return sftp_init(sftp);
}

int LibsshApi::sftpGetError(sftp_session sftp) const
{
    return sftp_get_error(sftp);
}

sftp_dir LibsshApi::sftpOpendir(sftp_session sftp, const QByteArray &path)
{
    return sftp_opendir(sftp, path.constData());
}

sftp_attributes LibsshApi::sftpReaddir(sftp_session sftp, sftp_dir dir)
{
    return sftp_readdir(sftp, dir);
}

int LibsshApi::sftpDirEof(sftp_dir dir)
{
    return sftp_dir_eof(dir);
}

int LibsshApi::sftpClosedir(sftp_dir dir)
{
    return sftp_closedir(dir);
}

void LibsshApi::sftpAttributesFree(sftp_attributes attrs)
{
    sftp_attributes_free(attrs);
}

int LibsshApi::sftpCanonicalizePath(sftp_session sftp, const QByteArray &path,
                                    QString *out)
{
    char *resolved = sftp_canonicalize_path(sftp, path.constData());
    if (resolved == nullptr) {
        return SSH_ERROR;
    }
    if (out != nullptr) {
        *out = QString::fromUtf8(resolved);
    }
    ssh_string_free_char(resolved);
    return SSH_OK;
}

int LibsshApi::sftpMkdir(sftp_session sftp, const QByteArray &path, mode_t mode)
{
    return sftp_mkdir(sftp, path.constData(), mode);
}

int LibsshApi::sftpUnlink(sftp_session sftp, const QByteArray &path)
{
    return sftp_unlink(sftp, path.constData());
}

int LibsshApi::sftpRmdir(sftp_session sftp, const QByteArray &path)
{
    return sftp_rmdir(sftp, path.constData());
}

int LibsshApi::sftpRename(sftp_session sftp, const QByteArray &from,
                          const QByteArray &to)
{
    return sftp_rename(sftp, from.constData(), to.constData());
}

int LibsshApi::sftpChmod(sftp_session sftp, const QByteArray &path, mode_t mode)
{
    return sftp_chmod(sftp, path.constData(), mode);
}

sftp_file LibsshApi::sftpOpen(sftp_session sftp, const QByteArray &path,
                              int accesstype, mode_t mode)
{
    return sftp_open(sftp, path.constData(), accesstype, mode);
}

int LibsshApi::sftpCloseFile(sftp_file file)
{
    return sftp_close(file);
}

sftp_attributes LibsshApi::sftpFstat(sftp_file file)
{
    return sftp_fstat(file);
}

void LibsshApi::sftpFileSetNonblocking(sftp_file file)
{
    sftp_file_set_nonblocking(file);
}

ssize_t LibsshApi::sftpAioBeginRead(sftp_file file, size_t len, sftp_aio *aio)
{
    return sftp_aio_begin_read(file, len, aio);
}

ssize_t LibsshApi::sftpAioWaitRead(sftp_aio *aio, void *buf, size_t bufSize)
{
    return sftp_aio_wait_read(aio, buf, bufSize);
}

ssize_t LibsshApi::sftpAioBeginWrite(sftp_file file, const void *buf, size_t len,
                                     sftp_aio *aio)
{
    return sftp_aio_begin_write(file, buf, len, aio);
}

ssize_t LibsshApi::sftpAioWaitWrite(sftp_aio *aio)
{
    return sftp_aio_wait_write(aio);
}

void LibsshApi::sftpAioFree(sftp_aio aio)
{
    sftp_aio_free(aio);
}
