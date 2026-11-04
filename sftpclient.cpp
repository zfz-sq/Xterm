#include "sftpclient.h"

#include "fsbridge.h"
#include "hostbridge.h"

#include "hoststats.h"
#include "sshsession.h"

#include <QEventLoop>
#include <QTimer>

#include <memory>

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

// —— FsBridge 真实后端（M2.b）：把本类的异步信号桥接成同步 FsHooks ——
// AgentBridge 在 GUI 线程串行处理请求，同一时刻至多一个 FS 操作挂起，
// 嵌套事件循环等单个操作完成即可；超时兜底防远端卡死拖住界面。
FsBridge::FsHooks FsBridge::hooksForSftp(SftpClient *client)
{
    constexpr int kTimeoutMs = 15000;
    FsHooks h;

    h.realPath = [client](const QString &path, QString *err) {
        QString result;
        QEventLoop loop;
        bool ok = false;
        bool done = false;
        const auto c = QObject::connect(
            client, &SftpClient::opFinished, client,
            [&](const QString &op, const QString &arg, bool succeeded, const QString &error) {
                if (op != QLatin1String("realpath"))
                    return; // 本实例只有 FsBridge 在用，不会有别的 op 插入
                done = true;
                ok = succeeded && !arg.isEmpty();
                if (ok)
                    result = arg;
                else
                    *err = error.isEmpty() ? QStringLiteral("realpath 失败") : error;
                loop.quit();
            });
        QTimer::singleShot(kTimeoutMs, &loop, &QEventLoop::quit);
        client->realPath(path);
        if (!done) // 离线时 submit() 同步回结果；exec 前的 quit 会丢，别进空循环
            loop.exec();
        QObject::disconnect(c);
        if (!done && err->isEmpty())
            *err = QStringLiteral("realpath 超时");
        return result;
    };

    h.list = [client](const QString &path, QString *err) {
        QVector<SftpEntry> entries;
        QEventLoop loop;
        bool done = false;
        const auto c1 = QObject::connect(
            client, &SftpClient::listed, client,
            [&](const QString &p, const QVector<SftpEntry> &es) {
                if (p != path)
                    return;
                entries = es;
                done = true;
                loop.quit();
            });
        const auto c2 = QObject::connect(
            client, &SftpClient::listedFailed, client,
            [&](const QString &p, const QString &error) {
                if (p != path)
                    return;
                *err = error;
                done = true;
                loop.quit();
            });
        QTimer::singleShot(kTimeoutMs, &loop, &QEventLoop::quit);
        client->listDir(path);
        if (!done) // 离线同步失败不进事件循环（同 realPath 注释）
            loop.exec();
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        if (!done && err->isEmpty())
            *err = QStringLiteral("列目录超时");
        return entries;
    };

    // 传输 id 与界面无关（独立 SftpClient 实例），共享计数器够用
    const auto nextId = std::make_shared<int>(1);
    auto bridgeTransfer = [client, nextId](bool upload) {
        return [client, nextId, upload](const QString &a, const QString &b, QString *err) {
            QEventLoop loop;
            bool ok = false;
            bool done = false;
            const int id = (*nextId)++;
            const auto c = QObject::connect(
                client, &SftpClient::transferFinished, client,
                [&](int tid, bool succeeded, const QString &error) {
                    if (tid != id)
                        return;
                    ok = succeeded;
                    done = true;
                    if (!succeeded)
                        *err = error;
                    loop.quit();
                });
            QTimer::singleShot(kTimeoutMs, &loop, &QEventLoop::quit);
            if (upload)
                client->uploadFile(id, a, b); // a=local b=remote
            else
                client->downloadFile(id, a, b); // a=remote b=local
            if (!done) // 离线同步失败不进事件循环（同 realPath 注释）
                loop.exec();
            QObject::disconnect(c);
            if (!done)
                *err = QString::fromUtf8(upload ? "上传超时" : "下载超时");
            return ok;
        };
    };
    h.download = bridgeTransfer(false);
    h.upload = bridgeTransfer(true);

    return h;
}

// —— HostBridge 真实后端（M2.c）：stats 缓存直读 + 按需 exec 的同步桥接 ——
// exec 结果经 opFinished(op="exec", arg=命令) 回来；成功时 error 字段装
// stdout（SftpChannel 的既有约定，详见 advanceExec 的 completeOp）。
HostBridge::HostHooks HostBridge::hooksForStats(HostStats *stats, SftpClient *client)
{
    constexpr int kTimeoutMs = 15000;
    HostHooks h;
    h.snapshot = [stats]() {
        HostSnapshot snap;
        if (stats != nullptr)
            snap = stats->snapshot();
        return snap;
    };
    h.exec = [client](const QString &command, QString *err) {
        QString out;
        if (client == nullptr) {
            *err = QStringLiteral("Host 后端未挂载");
            return out;
        }
        QEventLoop loop;
        bool done = false;
        const auto c = QObject::connect(
            client, &SftpClient::opFinished, client,
            [&](const QString &op, const QString &arg, bool succeeded, const QString &error) {
                if (op != QLatin1String("exec") || arg != command)
                    return; // 本实例只有 HostBridge 在用
                done = true;
                if (succeeded)
                    out = error; // exec 约定：成功时 error 字段装 stdout
                else
                    *err = error.isEmpty() ? QStringLiteral("远程命令失败") : error;
                loop.quit();
            });
        QTimer::singleShot(kTimeoutMs, &loop, &QEventLoop::quit);
        client->execCommand(command);
        if (!done) // 离线时 submit() 同步回结果；exec 前的 quit 会丢，别进空循环
            loop.exec();
        QObject::disconnect(c);
        if (!done && err->isEmpty())
            *err = QStringLiteral("远程命令超时");
        return out;
    };
    return h;
}
