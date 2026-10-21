#include "sftpclient.h"

#include "sshsession.h"

SftpClient::SftpClient(SshSession *session, QObject *parent)
    : QObject(parent), m_session(session)
{
    Q_ASSERT(m_session != nullptr);

    connect(m_session, &SshSession::sftpListed, this,
            &SftpClient::onSessionListed);
    connect(m_session, &SshSession::sftpListedFailed, this,
            &SftpClient::onSessionListedFailed);
    connect(m_session, &SshSession::sftpTransferProgress, this,
            &SftpClient::onSessionTransferProgress);
    connect(m_session, &SshSession::sftpTransferFinished, this,
            &SftpClient::onSessionTransferFinished);
    connect(m_session, &SshSession::sftpOpFinished, this,
            &SftpClient::onSessionOpFinished);
}

SftpClient::~SftpClient()
{
    // 只取消自己发起的请求；会话与 shell 原样保留。
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
        m_session->cancelTransfer(it.key());
}

void SftpClient::listDir(const QString &path)
{
    SftpRequest request;
    request.op = SftpOp::List;
    request.path = path;
    submit(request);
}

void SftpClient::realPath(const QString &path)
{
    SftpRequest request;
    request.op = SftpOp::RealPath;
    request.path = path;
    submit(request);
}

void SftpClient::uploadFile(int id, const QString &localPath,
                            const QString &remotePath)
{
    SftpRequest request;
    request.op = SftpOp::Upload;
    request.localPath = localPath;
    request.remotePath = remotePath;
    submit(request, id);
}

void SftpClient::downloadFile(int id, const QString &remotePath,
                              const QString &localPath)
{
    SftpRequest request;
    request.op = SftpOp::Download;
    request.remotePath = remotePath;
    request.localPath = localPath;
    submit(request, id);
}

void SftpClient::cancelTransfer(int id)
{
    const auto it = m_transferRequests.constFind(id);
    if (it == m_transferRequests.constEnd())
        return;
    // 映射保留到终止结果到达（transferFinished）时再移除。
    m_session->cancelTransfer(it.value());
}

void SftpClient::makeDir(const QString &path)
{
    SftpRequest request;
    request.op = SftpOp::Mkdir;
    request.path = path;
    submit(request);
}

void SftpClient::createFile(const QString &path)
{
    SftpRequest request;
    request.op = SftpOp::Create;
    request.path = path;
    submit(request);
}

void SftpClient::removeEntry(const QString &path, bool isDir)
{
    SftpRequest request;
    request.op = SftpOp::Delete;
    request.path = path;
    request.isDir = isDir;
    submit(request);
}

void SftpClient::renameEntry(const QString &from, const QString &to)
{
    SftpRequest request;
    request.op = SftpOp::Rename;
    request.path = from;
    request.dest = to;
    submit(request);
}

void SftpClient::copyRemote(const QString &from, const QString &to, bool isDir)
{
    SftpRequest request;
    request.op = SftpOp::Copy;
    request.path = from;
    request.dest = to;
    request.isDir = isDir;
    submit(request);
}

void SftpClient::setPermissions(const QString &path, quint32 mode)
{
    SftpRequest request;
    request.op = SftpOp::Chmod;
    request.path = path;
    request.mode = mode;
    submit(request);
}

void SftpClient::execCommand(const QString &command)
{
    SftpRequest request;
    request.op = SftpOp::Exec;
    request.command = command;
    submit(request);
}

void SftpClient::submit(SftpRequest request, int transferId)
{
    request.requestId = m_nextId++;
    if (!m_session->isOpen()) {
        // 同步失败，界面拿到和旧 ensureSession 一样的“未连接”回音。
        const QString error = QStringLiteral("SSH 会话未连接");
        switch (request.op) {
        case SftpOp::List:
            emit listedFailed(request.path, error);
            break;
        case SftpOp::Upload:
        case SftpOp::Download:
            emit transferFinished(transferId, false, error);
            break;
        default:
            emit opFinished(sftpOpName(request.op),
                            request.op == SftpOp::Exec ? request.command
                                                       : request.path,
                            false, error);
            break;
        }
        return;
    }

    TrackedRequest tracked;
    tracked.transferId = transferId;
    m_pending.insert(request.requestId, tracked);
    if (transferId != 0)
        m_transferRequests.insert(transferId, request.requestId);

    switch (request.op) {
    case SftpOp::List:
        m_session->listDir(request.requestId, request.path);
        break;
    case SftpOp::RealPath:
        m_session->realPath(request.requestId, request.path);
        break;
    case SftpOp::Upload:
        m_session->uploadFile(request.requestId, request.localPath,
                              request.remotePath);
        break;
    case SftpOp::Download:
        m_session->downloadFile(request.requestId, request.remotePath,
                                request.localPath);
        break;
    case SftpOp::Mkdir:
        m_session->makeDir(request.requestId, request.path);
        break;
    case SftpOp::Create:
        m_session->createFile(request.requestId, request.path);
        break;
    case SftpOp::Delete:
        m_session->removeEntry(request.requestId, request.path, request.isDir);
        break;
    case SftpOp::Rename:
        m_session->renameEntry(request.requestId, request.path, request.dest);
        break;
    case SftpOp::Copy:
        m_session->copyRemote(request.requestId, request.path, request.dest,
                              request.isDir);
        break;
    case SftpOp::Chmod:
        m_session->setPermissions(request.requestId, request.path,
                                  request.mode);
        break;
    case SftpOp::Exec:
        m_session->execCommand(request.requestId, request.command);
        break;
    }
}

bool SftpClient::trackedTake(SshRequestId requestId, int *transferId)
{
    const auto it = m_pending.constFind(requestId);
    if (it == m_pending.constEnd())
        return false;   // 不是本适配器发起的（或已终止）
    const int id = it->transferId;
    if (transferId != nullptr)
        *transferId = id;
    m_pending.erase(it);
    if (id != 0)
        m_transferRequests.remove(id);
    return true;
}

void SftpClient::onSessionListed(SshRequestId requestId, const QString &path,
                                 const QVector<SftpEntry> &entries)
{
    if (!trackedTake(requestId, nullptr))
        return;
    emit listed(path, entries);
}

void SftpClient::onSessionListedFailed(SshRequestId requestId,
                                       const QString &path,
                                       const QString &error)
{
    if (!trackedTake(requestId, nullptr))
        return;
    emit listedFailed(path, error);
}

void SftpClient::onSessionTransferProgress(SshRequestId requestId,
                                           qint64 done, qint64 total)
{
    const auto it = m_pending.constFind(requestId);
    if (it == m_pending.constEnd() || it->transferId == 0)
        return;
    emit transferProgress(it->transferId, done, total);
}

void SftpClient::onSessionTransferFinished(SshRequestId requestId, bool ok,
                                           const QString &error)
{
    int transferId = 0;
    if (!trackedTake(requestId, &transferId) || transferId == 0)
        return;
    emit transferFinished(transferId, ok, error);
}

void SftpClient::onSessionOpFinished(SshRequestId requestId, const QString &op,
                                     const QString &arg, bool ok,
                                     const QString &error)
{
    if (!trackedTake(requestId, nullptr))
        return;
    emit opFinished(op, arg, ok, error);
}
