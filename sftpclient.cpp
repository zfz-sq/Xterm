#include "sftpclient.h"

#include "fsbridge.h"
#include "hostbridge.h"

#include "hoststats.h"
#include "sshsession.h"

#include <QList>
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

namespace {

// 一次 SFTP 回调的座位：超时与结果只结算一次，不进入嵌套事件循环。
// 离线时 submit() 会在发起调用里同步 emit，连接必须先于发起。
struct BridgeSeat
{
    bool settled = false;
    QMetaObject::Connection c1;
    QMetaObject::Connection c2;
    QTimer *timer = nullptr;

    void cancel()
    {
        if (settled)
            return;
        settled = true;
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        if (timer) {
            timer->stop();
            timer->deleteLater();
            timer = nullptr;
        }
    }
};

constexpr int kIoTimeoutMs = 15000;

// realpath 的 opFinished 参数是解析后的路径，不能拿它和请求路径比对。
// worker 按提交顺序回结果，用队列把结果交回对应的等待者。
struct RealWait
{
    std::shared_ptr<BridgeSeat> seat;
    std::function<void(const QString &, const QString &)> done;
};

} // namespace

// —— FsBridge 真实后端：信号转回调。不在界面线程上 QEventLoop::exec ——
FsBridge::FsHooks FsBridge::hooksForSftp(SftpClient *client)
{
    FsHooks h;

    auto realWaits = std::make_shared<QList<RealWait>>();
    QObject::connect(
        client, &SftpClient::opFinished, client,
        [realWaits](const QString &op, const QString &arg, bool succeeded, const QString &error) {
            if (op != QLatin1String("realpath") || realWaits->isEmpty())
                return;
            const RealWait wait = realWaits->takeFirst();
            if (!wait.seat || wait.seat->settled)
                return; // 超时后迟到的结果：丢掉，别交给下一次请求
            wait.seat->cancel();
            if (succeeded && !arg.isEmpty())
                wait.done(arg, {});
            else
                wait.done({}, error.isEmpty() ? QStringLiteral("realpath 失败") : error);
        });

    h.realPath = [client, realWaits](const QString &path,
                                     const std::function<void(const QString &, const QString &)> &done) {
        RealWait wait;
        wait.seat = std::make_shared<BridgeSeat>();
        wait.done = done;
        wait.seat->timer = new QTimer(client);
        wait.seat->timer->setSingleShot(true);
        QObject::connect(wait.seat->timer, &QTimer::timeout, client, [wait] {
            if (!wait.seat || wait.seat->settled)
                return;
            // 留在队列里当墓碑：迟到的结果被丢掉，不会错配给下一次 realpath。
            wait.seat->cancel();
            wait.done({}, QStringLiteral("realpath 超时"));
        });
        realWaits->append(wait);
        wait.seat->timer->start(kIoTimeoutMs);
        client->realPath(path);
    };

    h.list = [client](const QString &path,
                      const std::function<void(const QVector<SftpEntry> &, const QString &)> &done) {
        auto seat = std::make_shared<BridgeSeat>();
        auto finish = [seat, done](const QVector<SftpEntry> &entries, const QString &err) {
            if (seat->settled)
                return;
            seat->cancel();
            done(entries, err);
        };
        seat->c1 = QObject::connect(
            client, &SftpClient::listed, client,
            [path, finish](const QString &p, const QVector<SftpEntry> &entries) {
                if (p != path)
                    return;
                finish(entries, {});
            });
        seat->c2 = QObject::connect(
            client, &SftpClient::listedFailed, client,
            [path, finish](const QString &p, const QString &error) {
                if (p != path)
                    return;
                finish({}, error);
            });
        seat->timer = new QTimer(client);
        seat->timer->setSingleShot(true);
        QObject::connect(seat->timer, &QTimer::timeout, client, [seat, finish] {
            if (seat->settled)
                return;
            finish({}, QStringLiteral("列目录超时"));
        });
        seat->timer->start(kIoTimeoutMs);
        client->listDir(path);
    };

    const auto nextId = std::make_shared<int>(1);
    auto bridgeTransfer = [client, nextId](bool upload) {
        return [client, nextId, upload](const QString &a, const QString &b,
                                        const std::function<void(bool, const QString &)> &done) {
            auto seat = std::make_shared<BridgeSeat>();
            const int id = (*nextId)++;
            auto finish = [seat, done](bool ok, const QString &err) {
                if (seat->settled)
                    return;
                seat->cancel();
                done(ok, err);
            };
            seat->c1 = QObject::connect(
                client, &SftpClient::transferFinished, client,
                [id, finish](int tid, bool succeeded, const QString &error) {
                    if (tid != id)
                        return;
                    finish(succeeded, succeeded ? QString() : error);
                });
            seat->timer = new QTimer(client);
            seat->timer->setSingleShot(true);
            QObject::connect(seat->timer, &QTimer::timeout, client, [seat, finish, upload] {
                if (seat->settled)
                    return;
                finish(false, QString::fromUtf8(upload ? "上传超时" : "下载超时"));
            });
            seat->timer->start(kIoTimeoutMs);
            if (upload)
                client->uploadFile(id, a, b); // a=local b=remote
            else
                client->downloadFile(id, a, b); // a=remote b=local
        };
    };
    h.download = bridgeTransfer(false);
    h.upload = bridgeTransfer(true);
    return h;
}

// —— HostBridge 真实后端：exec 走回调。成功时 error 字段装 stdout ——
// （SftpChannel 的既有约定，详见 advanceExec 的 completeOp）。
HostBridge::HostHooks HostBridge::hooksForStats(HostStats *stats, SftpClient *client)
{
    HostHooks h;
    h.snapshot = [stats]() {
        HostSnapshot snap;
        if (stats != nullptr)
            snap = stats->snapshot();
        return snap;
    };
    h.exec = [client](const QString &command,
                      const std::function<void(const QString &, const QString &)> &done) {
        if (client == nullptr) {
            done({}, QStringLiteral("Host 后端未挂载"));
            return;
        }
        auto seat = std::make_shared<BridgeSeat>();
        auto finish = [seat, done](const QString &out, const QString &err) {
            if (seat->settled)
                return;
            seat->cancel();
            done(out, err);
        };
        seat->c1 = QObject::connect(
            client, &SftpClient::opFinished, client,
            [command, finish](const QString &op, const QString &arg, bool succeeded,
                              const QString &error) {
                if (op != QLatin1String("exec") || arg != command)
                    return;
                if (succeeded)
                    finish(error, {}); // exec 约定：成功时 error 字段装 stdout
                else
                    finish({}, error.isEmpty() ? QStringLiteral("远程命令失败") : error);
            });
        seat->timer = new QTimer(client);
        seat->timer->setSingleShot(true);
        QObject::connect(seat->timer, &QTimer::timeout, client, [seat, finish] {
            if (seat->settled)
                return;
            finish({}, QStringLiteral("远程命令超时"));
        });
        seat->timer->start(kIoTimeoutMs);
        client->execCommand(command);
    };
    return h;
}
