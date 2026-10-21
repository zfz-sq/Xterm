#pragma once

#include "sshtypes.h"

#include <QMetaType>
#include <QString>
#include <QVector>

/*
 * SftpEntry: 远程目录里的一条文件/子目录信息。
 * 定义在这里（ssh/ 侧），GUI 头文件 sftpclient.h 只做转发包含。
 */
struct SftpEntry
{
    QString name;
    quint64 size = 0;          // 字节数（目录为 0）
    qint64 mtime = 0;          // epoch 秒
    quint32 permissions = 0;   // POSIX 权限位（0644 之类）
    bool isDir = false;
    bool isSymlink = false;
};

Q_DECLARE_METATYPE(SftpEntry)
Q_DECLARE_METATYPE(QVector<SftpEntry>)

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

inline QString sftpOpName(SftpOp op)
{
    switch (op) {
    case SftpOp::RealPath:
        return QStringLiteral("realpath");
    case SftpOp::Mkdir:
        return QStringLiteral("mkdir");
    case SftpOp::Create:
        return QStringLiteral("create");
    case SftpOp::Delete:
        return QStringLiteral("remove");
    case SftpOp::Rename:
        return QStringLiteral("rename");
    case SftpOp::Copy:
        return QStringLiteral("copy");
    case SftpOp::Chmod:
        return QStringLiteral("chmod");
    case SftpOp::Exec:
        return QStringLiteral("exec");
    case SftpOp::List:
    case SftpOp::Upload:
    case SftpOp::Download:
        break;
    }
    return {};
}

// 终止性取消结果：List/传输/其余操作各对应一种界面语义，
// 与 SftpChannel 内部取消、worker 拒收时共用同一形状。
inline SftpResult sftpCancelledResult(const SftpRequest &request)
{
    SftpResult result;
    result.requestId = request.requestId;
    result.ok = false;
    result.error = QStringLiteral("已取消");
    switch (request.op) {
    case SftpOp::List:
        result.kind = SftpResult::Kind::ListedFailed;
        result.path = request.path;
        break;
    case SftpOp::Upload:
    case SftpOp::Download:
        result.kind = SftpResult::Kind::TransferFinished;
        break;
    default:
        result.kind = SftpResult::Kind::OpFinished;
        result.op = sftpOpName(request.op);
        result.arg = request.op == SftpOp::Exec ? request.command
                                                : request.path;
        break;
    }
    return result;
}
