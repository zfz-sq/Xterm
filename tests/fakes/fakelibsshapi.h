#pragma once

#include "ssh/libsshapi.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QList>
#include <QStringList>

class FakeLibsshApi final : public LibsshApi
{
public:
    FakeLibsshApi()
    {
        auto appendString = [](QByteArray *blob,
                               const QByteArray &value) {
            const quint32 size = static_cast<quint32>(value.size());
            blob->append(char((size >> 24) & 0xff));
            blob->append(char((size >> 16) & 0xff));
            blob->append(char((size >> 8) & 0xff));
            blob->append(char(size & 0xff));
            blob->append(value);
        };
        appendString(&serverKeyBlob, QByteArrayLiteral("ssh-ed25519"));
        appendString(&serverKeyBlob, QByteArray(32, 'm'));
        serverFingerprint = QString::fromLatin1(
            QCryptographicHash::hash(
                serverKeyBlob, QCryptographicHash::Sha256).toHex());
    }

    bool validateKnownHostKey(const QByteArray &algorithm,
                              const QByteArray &encodedKey) const override
    {
        calls.append(
            QStringLiteral("validateKnownHostKey:%1")
                .arg(QString::fromLatin1(algorithm)));
        Q_UNUSED(encodedKey);
        return take(keyValidationResults, true);
    }

    ssh_session newSession() override
    {
        calls.append(QStringLiteral("newSession"));
        return reinterpret_cast<ssh_session>(quintptr(1));
    }

    void freeSession(ssh_session) override { calls.append(QStringLiteral("freeSession")); }

    int setOptionString(ssh_session, enum ssh_options_e option,
                        const QString &value) override
    {
        calls.append(QStringLiteral("string:%1=%2").arg(int(option)).arg(value));
        stringOptions[option] = value;
        if (option == SSH_OPTIONS_HOST) {
            effectiveHostValue = value;
        }
        return takeOptionResult(option);
    }

    int setOptionUnsigned(ssh_session, enum ssh_options_e option,
                          unsigned int value) override
    {
        calls.append(QStringLiteral("unsigned:%1=%2").arg(int(option)).arg(value));
        unsignedOptions[option] = value;
        if (option == SSH_OPTIONS_PORT) {
            effectivePortValue = static_cast<quint16>(value);
        }
        return takeOptionResult(option);
    }

    int setOptionBool(ssh_session, enum ssh_options_e option, bool value) override
    {
        calls.append(QStringLiteral("bool:%1=%2").arg(int(option)).arg(value));
        boolOptions[option] = value;
        return takeOptionResult(option);
    }

    int parseConfig(ssh_session) override
    {
        calls.append(QStringLiteral("parseConfig"));
        if (!configuredHost.isEmpty()) {
            effectiveHostValue = configuredHost;
        }
        if (configuredPort != 0) {
            effectivePortValue = configuredPort;
        }
        return parseConfigResult;
    }

    QString effectiveHost(ssh_session) override
    {
        calls.append(QStringLiteral("effectiveHost"));
        return effectiveHostValue;
    }

    quint16 effectivePort(ssh_session) override
    {
        calls.append(QStringLiteral("effectivePort"));
        return effectivePortValue;
    }

    void setBlocking(ssh_session, bool blocking) override
    {
        calls.append(QStringLiteral("blocking:%1").arg(blocking));
    }

    int connect(ssh_session) override
    {
        calls.append(QStringLiteral("connect"));
        return take(connectResults, SSH_OK);
    }

    enum ssh_known_hosts_e knownHostState(ssh_session) override
    {
        calls.append(QStringLiteral("knownHostState"));
        if (knownState == SSH_KNOWN_HOSTS_OK
            && ensureKnownHostForOk
            && !appendCurrentKnownHost()) {
            return SSH_KNOWN_HOSTS_ERROR;
        }
        return knownState;
    }

    int updateKnownHosts(ssh_session) override
    {
        calls.append(QStringLiteral("updateKnownHosts"));
        ++updateKnownHostsCount;
        if (appendKnownHostOnUpdate && !appendCurrentKnownHost()) {
            return SSH_ERROR;
        }
        return updateKnownHostsResult;
    }

    QString serverFingerprintSha256(ssh_session) override
    {
        calls.append(QStringLiteral("serverFingerprint"));
        return serverFingerprint;
    }

    QString serverHostKeyType(ssh_session) override
    {
        calls.append(QStringLiteral("serverHostKeyType"));
        return serverKeyType;
    }

    int authAgent(ssh_session) override
    {
        calls.append(QStringLiteral("authAgent"));
        return take(agentResults, int(SSH_AUTH_DENIED));
    }

    int authExplicitKey(ssh_session, const QString &path,
                        const QByteArray &passphrase) override
    {
        Q_UNUSED(passphrase);
        calls.append(QStringLiteral("authExplicit:%1").arg(path));
        return take(explicitKeyResults, int(SSH_AUTH_DENIED));
    }

    QString authenticationErrorString(ssh_session) const override
    {
        return explicitKeyErrorText.isEmpty() ? errorText
                                              : explicitKeyErrorText;
    }

    int authAutomaticPublicKey(ssh_session,
                               const QByteArray &passphrase) override
    {
        Q_UNUSED(passphrase);
        calls.append(QStringLiteral("authAuto"));
        return take(autoKeyResults, int(SSH_AUTH_DENIED));
    }

    int authKeyboardInteractive(ssh_session) override
    {
        calls.append(QStringLiteral("authKeyboard"));
        return take(keyboardResults, int(SSH_AUTH_DENIED));
    }

    QList<SshAuthPrompt> keyboardInteractivePrompts(ssh_session) override
    {
        calls.append(QStringLiteral("keyboardPrompts"));
        return prompts;
    }

    int setKeyboardInteractiveAnswer(ssh_session, unsigned int index,
                                     const QByteArray &answer) override
    {
        calls.append(QStringLiteral("keyboardAnswer:%1").arg(index));
        answers.append(QString::fromUtf8(answer));
        return SSH_OK;
    }

    int authPassword(ssh_session, const QByteArray &password) override
    {
        Q_UNUSED(password);
        calls.append(QStringLiteral("authPassword"));
        return take(passwordResults, int(SSH_AUTH_DENIED));
    }

    int sendKeepalive(ssh_session) override
    {
        calls.append(QStringLiteral("keepalive"));
        return keepaliveResult;
    }

    void disconnect(ssh_session) override
    {
        calls.append(QStringLiteral("disconnect"));
        ++disconnectCount;
    }

    QString errorString(ssh_session) const override { return errorText; }
    int errorCode(ssh_session) const override { return errorNumber; }

    template<typename T>
    static T take(QList<T> &values, T fallback)
    {
        if (values.isEmpty()) {
            return fallback;
        }
        return values.takeFirst();
    }

    mutable QStringList calls;
    QHash<enum ssh_options_e, QString> stringOptions;
    QHash<enum ssh_options_e, unsigned int> unsignedOptions;
    QHash<enum ssh_options_e, bool> boolOptions;
    QHash<enum ssh_options_e, QList<int>> optionResults;
    int parseConfigResult = SSH_OK;
    QString configuredHost;
    quint16 configuredPort = 0;
    QString effectiveHostValue;
    quint16 effectivePortValue = 22;
    QList<int> connectResults;
    enum ssh_known_hosts_e knownState = SSH_KNOWN_HOSTS_OK;
    int updateKnownHostsResult = SSH_OK;
    int updateKnownHostsCount = 0;
    bool appendKnownHostOnUpdate = true;
    bool ensureKnownHostForOk = true;
    QString serverFingerprint;
    QString serverKeyType = QStringLiteral("ssh-ed25519");
    QByteArray serverKeyBlob;
    mutable QList<bool> keyValidationResults;
    QList<int> agentResults;
    QList<int> explicitKeyResults;
    QList<int> autoKeyResults;
    QList<int> keyboardResults;
    QList<int> passwordResults;
    QList<SshAuthPrompt> prompts;
    QStringList answers;
    int keepaliveResult = SSH_OK;
    QString errorText = QStringLiteral("scripted libssh failure");
    QString explicitKeyErrorText;
    int errorNumber = SSH_ERROR;
    int disconnectCount = 0;

private:
    bool appendCurrentKnownHost()
    {
        QFile file(stringOptions.value(SSH_OPTIONS_KNOWNHOSTS));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            return false;
        }
        const QString hostPattern =
            effectivePortValue == 22
            ? effectiveHostValue.toLower()
            : QStringLiteral("[%1]:%2")
                  .arg(effectiveHostValue.toLower())
                  .arg(effectivePortValue);
        const QByteArray line =
            hostPattern.toUtf8() + ' ' + serverKeyType.toLatin1()
            + ' ' + serverKeyBlob.toBase64() + '\n';
        return file.write(line) == line.size();
    }

    int takeOptionResult(enum ssh_options_e option)
    {
        auto it = optionResults.find(option);
        return it == optionResults.end() ? SSH_OK : take(it.value(), SSH_OK);
    }
};
