#pragma once

#include "knownhostsstore.h"
#include "sshtypes.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <libssh/libssh.h>

class LibsshApi : public KnownHostKeyValidator
{
public:
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
    virtual void setBlocking(ssh_session session, bool blocking);
    virtual int connect(ssh_session session);

    virtual enum ssh_known_hosts_e knownHostState(ssh_session session);
    virtual int updateKnownHosts(ssh_session session);
    virtual QString serverFingerprintSha256(ssh_session session);

    virtual int authAgent(ssh_session session);
    virtual int authExplicitKey(ssh_session session, const QString &path,
                                const QByteArray &passphrase);
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

    virtual int sendKeepalive(ssh_session session);
    virtual void disconnect(ssh_session session);
    virtual QString errorString(ssh_session session) const;
    virtual int errorCode(ssh_session session) const;
};
