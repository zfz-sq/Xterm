#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

using SshRequestId = quint64;

struct SshError
{
    enum class Domain {
        None,
        Configuration,
        Network,
        HostKey,
        Authentication,
        Shell,
        Sftp,
        Stats,
        Cancelled,
        Internal
    };

    Domain domain = Domain::None;
    int code = 0;
    QString message;
    bool retryable = false;

    bool isError() const { return domain != Domain::None; }
};

enum class SshPhase {
    Idle,
    ResolvingConfig,
    Connecting,
    VerifyingHost,
    Authenticating,
    OpeningShell,
    Ready,
    Closing
};

struct SshAuthPrompt
{
    QString text;
    bool echo = false;
};

struct SshHostKeyChange
{
    SshRequestId requestId = 0;
    QString host;
    QString oldFingerprint;
    QString newFingerprint;
};

Q_DECLARE_METATYPE(SshRequestId)
Q_DECLARE_METATYPE(SshError)
Q_DECLARE_METATYPE(SshError::Domain)
Q_DECLARE_METATYPE(SshPhase)
Q_DECLARE_METATYPE(SshAuthPrompt)
Q_DECLARE_METATYPE(SshHostKeyChange)
