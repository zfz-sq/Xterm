#pragma once

#include "knownhostsstore.h"
#include "sshtypes.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

class LibsshApi : public KnownHostKeyValidator
{
public:
    LibsshApi();
    virtual ~LibsshApi() = default;

    bool validateKnownHostKey(
        const QByteArray &algorithm,
        const QByteArray &encodedKey) const override;

    virtual ssh_session newSession();
    virtual void freeSession(ssh_session session);

    virtual int setOptionString(ssh_session session,
                                enum ssh_options_e option,
                                const QString &value);
    virtual int setOptionUnsigned(ssh_session session,
                                  enum ssh_options_e option,
                                  unsigned int value);
    virtual int setOptionBool(ssh_session session,
                              enum ssh_options_e option, bool value);
    virtual int parseConfig(ssh_session session);
    virtual QString effectiveHost(ssh_session session);
    virtual quint16 effectivePort(ssh_session session);
    virtual void setBlocking(ssh_session session, bool blocking);
    virtual int connect(ssh_session session);

    virtual enum ssh_known_hosts_e knownHostState(ssh_session session);
    virtual int updateKnownHosts(ssh_session session);
    virtual QString serverFingerprintSha256(ssh_session session);
    virtual QString serverHostKeyType(ssh_session session);

    virtual int authAgent(ssh_session session);
    virtual int authExplicitKey(ssh_session session, const QString &path,
                                const QByteArray &passphrase);
    virtual QString authenticationErrorString(ssh_session session) const;
    virtual int authAutomaticPublicKey(ssh_session session,
                                       const QByteArray &passphrase);
    virtual int authKeyboardInteractive(ssh_session session);
    virtual QList<SshAuthPrompt>
    keyboardInteractivePrompts(ssh_session session);
    virtual int setKeyboardInteractiveAnswer(ssh_session session,
                                             unsigned int index,
                                             const QByteArray &answer);
    virtual int authPassword(ssh_session session,
                             const QByteArray &password);

    virtual ssh_channel newChannel(ssh_session session);
    virtual void freeChannel(ssh_channel channel);
    virtual int openChannelSession(ssh_channel channel);
    virtual int requestPtySize(ssh_channel channel, const QByteArray &term,
                               int cols, int rows);
    virtual int requestEnvironment(ssh_channel channel,
                                   const QByteArray &name,
                                   const QByteArray &value);
    virtual int requestShell(ssh_channel channel);
    virtual int writeChannel(ssh_channel channel, const char *data,
                             quint32 size);
    virtual int readChannelNonblocking(ssh_channel channel, char *data,
                                       quint32 size, bool stderrStream);
    virtual int changePtySize(ssh_channel channel, int cols, int rows);
    virtual bool channelIsEof(ssh_channel channel);
    virtual int sendChannelEof(ssh_channel channel);
    virtual int closeChannel(ssh_channel channel);

    virtual int sendKeepalive(ssh_session session);
    virtual void disconnect(ssh_session session);
    virtual QString errorString(ssh_session session) const;
    virtual int errorCode(ssh_session session) const;

    virtual int requestExec(ssh_channel channel, const QByteArray &command);
    virtual int channelGetExitStatus(ssh_channel channel);

    virtual int channelRequestSftp(ssh_channel channel);
    virtual sftp_session sftpNewChannel(ssh_session session,
                                        ssh_channel channel);
    virtual void sftpFree(sftp_session sftp);
    virtual int sftpInit(sftp_session sftp);
    virtual int sftpGetError(sftp_session sftp) const;

    virtual sftp_dir sftpOpendir(sftp_session sftp, const QByteArray &path);
    virtual sftp_attributes sftpReaddir(sftp_session sftp, sftp_dir dir);
    virtual int sftpDirEof(sftp_dir dir);
    virtual int sftpClosedir(sftp_dir dir);
    virtual void sftpAttributesFree(sftp_attributes attrs);

    virtual int sftpCanonicalizePath(sftp_session sftp, const QByteArray &path,
                                     QString *out);

    virtual int sftpMkdir(sftp_session sftp, const QByteArray &path,
                          mode_t mode);
    virtual int sftpUnlink(sftp_session sftp, const QByteArray &path);
    virtual int sftpRmdir(sftp_session sftp, const QByteArray &path);
    virtual int sftpRename(sftp_session sftp, const QByteArray &from,
                           const QByteArray &to);
    virtual int sftpChmod(sftp_session sftp, const QByteArray &path,
                          mode_t mode);

    virtual sftp_file sftpOpen(sftp_session sftp, const QByteArray &path,
                               int accesstype, mode_t mode);
    virtual int sftpCloseFile(sftp_file file);
    virtual sftp_attributes sftpFstat(sftp_file file);
    virtual void sftpFileSetNonblocking(sftp_file file);

    virtual ssize_t sftpAioBeginRead(sftp_file file, size_t len,
                                     sftp_aio *aio);
    virtual ssize_t sftpAioWaitRead(sftp_aio *aio, void *buf, size_t bufSize);
    virtual ssize_t sftpAioBeginWrite(sftp_file file, const void *buf,
                                      size_t len, sftp_aio *aio);
    virtual ssize_t sftpAioWaitWrite(sftp_aio *aio);
    virtual void sftpAioFree(sftp_aio aio);

private:
    QHash<ssh_session, QString> m_authenticationErrors;
};
