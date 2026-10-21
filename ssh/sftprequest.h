#pragma once

#include "sftpclient.h"
#include "sshtypes.h"

#include <QMetaType>
#include <QString>
#include <QVector>

enum class SftpOp {
    List,
    RealPath,
    Upload,
    Download,
    Mkdir,
    Create,
    Delete,
    Rename,
    Copy,
    Chmod,
    Exec
};

struct SftpRequest {
    SshRequestId requestId = 0;
    SftpOp op = SftpOp::List;
    QString path;
    QString dest;
    QString localPath;
    QString remotePath;
    QString command;
    quint32 mode = 0;
    bool isDir = false;
};

struct SftpResult {
    enum class Kind {
        Listed,
        ListedFailed,
        Progress,
        TransferFinished,
        OpFinished
    };

    Kind kind = Kind::OpFinished;
    SshRequestId requestId = 0;
    QString op;
    QString arg;
    QString path;
    QVector<SftpEntry> entries;
    qint64 done = 0;
    qint64 total = 0;
    bool ok = false;
    QString error;
};

Q_DECLARE_METATYPE(SftpOp)
Q_DECLARE_METATYPE(SftpRequest)
Q_DECLARE_METATYPE(SftpResult)
