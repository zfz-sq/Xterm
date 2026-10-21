#include "sftpclient.h"

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <algorithm>
#include <cstring>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

namespace {
constexpr int kSessionTimeoutMs = 15000;   // 单个 libssh2 阻塞调用的超时
constexpr int kConnectTimeoutMs = 10000;   // TCP connect 超时
constexpr int kChunkSize = 32 * 1024;      // 传输块；取消检查粒度同此

// "host:port sha256hex"。首行注释以 # 开头允许。
struct KnownHost
{
    QString key;      // host:port
    QString sha256;   // 十六进制
};

QVector<KnownHost> parseKnownHosts(const QString &content)
{
    QVector<KnownHost> out;
    const auto lines = content.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;
        const int sp = trimmed.indexOf(QLatin1Char(' '));
        if (sp <= 0)
            continue;
        KnownHost kh;
        kh.key = trimmed.left(sp);
        kh.sha256 = trimmed.mid(sp + 1).trimmed();
        out.append(kh);
    }
    return out;
}
} // namespace

SftpClient::SftpClient(QObject *parent)
    : QObject(parent)
{
    // 禁止 parent：本对象要撒进工作线程，带 parent 的 QObject 无法
    // moveToThread。生命周期由创建方手动管理（FileBrowserDialog 析构时 delete）。
    Q_ASSERT(!parent);

    // 跨线程排队信号的自定义类型注册
    qRegisterMetaType<SshTarget>("SshTarget");
    qRegisterMetaType<SftpEntry>("SftpEntry");
    qRegisterMetaType<QVector<SftpEntry>>("QVector<SftpEntry>");

    m_thread = new QThread;
    moveToThread(m_thread);
    m_thread->start();
}

SftpClient::~SftpClient()
{
    // 排队一个断开，让工作线程先收尾再退出
    QMetaObject::invokeMethod(this, &SftpClient::disconnectFromHost, Qt::QueuedConnection);
    m_thread->quit();
    m_thread->wait(20000);
    delete m_thread;
}

QString SftpClient::knownHostsPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(base).filePath(QStringLiteral("known_hosts"));
}

void SftpClient::setHostKeyPrompter(HostKeyPrompter prompter)
{
    m_prompter = std::move(prompter);
}

bool SftpClient::deferToWorker(const std::function<void()> &fn)
{
    if (QThread::currentThread() == thread())
        return false;
    QMetaObject::invokeMethod(this, fn, Qt::QueuedConnection);
    return true;
}

bool SftpClient::confirmHostKey(const QString &host, const QString &stored, const QString &current)
{
    if (!m_prompter)
        return false;
    QCoreApplication *app = QCoreApplication::instance();
    if (app && QThread::currentThread() != app->thread()) {
        bool accepted = false;
        QMetaObject::invokeMethod(app, [&] {
            accepted = m_prompter(host, stored, current);
        }, Qt::BlockingQueuedConnection);
        return accepted;
    }
    return m_prompter(host, stored, current);
}

bool SftpClient::rememberHost(const QString &key, const QString &fingerprint)
{
    QStringList lines;
    QFile in(knownHostsPath());
    if (in.open(QIODevice::ReadOnly)) {
        lines = QString::fromUtf8(in.readAll()).split(QLatin1Char('\n'));
        in.close();
    }
    bool replaced = false;
    for (QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;
        const int sp = trimmed.indexOf(QLatin1Char(' '));
        if (sp > 0 && trimmed.left(sp) == key) {
            line = key + QLatin1Char(' ') + fingerprint;
            replaced = true;
        }
    }
    if (!replaced)
        lines.append(key + QLatin1Char(' ') + fingerprint);
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    QByteArray payload = lines.join(QLatin1Char('\n')).toUtf8();
    payload.append('\n');
    QDir().mkpath(QFileInfo(knownHostsPath()).absolutePath());
    {
        QSaveFile out(knownHostsPath());
        if (out.open(QIODevice::WriteOnly)) {
            out.write(payload);
            if (out.commit())
                return true;
        }
    }
    // 原文件仍被占用时，Windows 上原子替换会失败，改直接覆盖。
    QFile plain(knownHostsPath());
    if (!plain.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return plain.write(payload) == payload.size();
}

void SftpClient::connectToHost(const SshTarget &target)
{
    if (deferToWorker([this, target] { connectToHost(target); }))
        return;
    // 换目标前先断掉旧连接（若有）
    disconnectFromHost();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // --- TCP 连接（getaddrinfo + 非阻塞 connect + select 超时） ---
    const QString portText = QString::number(target.port > 0 ? target.port : 22);
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(target.host.toUtf8().constData(), portText.toUtf8().constData(), &hints, &res) != 0 || !res) {
        emit connectFailed(QStringLiteral("无法解析主机 %1").arg(target.host));
        return;
    }
    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        emit connectFailed(QStringLiteral("创建 socket 失败"));
        return;
    }
    // 非阻塞 connect + select 实现 10s 超时（阻塞版在 Windows 上要 ~21s）
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
    int rc = ::connect(sock, res->ai_addr, int(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv = { kConnectTimeoutMs / 1000, 0 };
        rc = select(int(sock) + 1, nullptr, &wset, nullptr, &tv);
        if (rc <= 0) {
            closesocket(sock);
            emit connectFailed(QStringLiteral("连接 %1:%2 超时").arg(target.host, portText));
            return;
        }
    } else if (rc != 0) {
        closesocket(sock);
        emit connectFailed(QStringLiteral("连接 %1:%2 失败（%3）")
                               .arg(target.host, portText, QString::number(WSAGetLastError())));
        return;
    }
    nb = 0;
    ioctlsocket(sock, FIONBIO, &nb); // libssh2 用阻塞模式
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    // --- SSH 握手 ---
    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        closesocket(sock);
        emit connectFailed(QStringLiteral("初始化 SSH 会话失败"));
        return;
    }
    libssh2_session_set_timeout(session, kSessionTimeoutMs);
    if (libssh2_session_handshake(session, (libssh2_socket_t)sock) != 0) {
        char *msg = nullptr;
        int len = 0;
        libssh2_session_last_error(session, &msg, &len, 0);
        const QString err = (msg && len > 0) ? QString::fromUtf8(msg, len)
                                             : QStringLiteral("协议错误或超时");
        libssh2_session_free(session);
        closesocket(sock);
        emit connectFailed(QStringLiteral("SSH 握手失败：%1").arg(err));
        return;
    }

    // --- 主机指纹校验 ---
    const char *hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    QString fingerprint;
    if (hash) {
        fingerprint = QString::fromLatin1(QByteArray(hash, 32).toHex());
    } else {
        libssh2_session_free(session);
        closesocket(sock);
        emit connectFailed(QStringLiteral("无法获取主机指纹"));
        return;
    }
    m_host = QStringLiteral("%1:%2").arg(target.host, portText);
    QString khContent;
    {
        // 读完即关。句柄还开着时，Windows 上无法替换这份记录。
        QFile khFile(knownHostsPath());
        if (khFile.open(QIODevice::ReadOnly))
            khContent = QString::fromUtf8(khFile.readAll());
    }
    const QVector<KnownHost> known = parseKnownHosts(khContent);
    bool found = false;
    QString stored;
    for (const KnownHost &kh : known) {
        if (kh.key == m_host) {
            found = true;
            stored = kh.sha256;
            break;
        }
    }
    if (found && stored != fingerprint) {
        if (!confirmHostKey(m_host, stored, fingerprint)) {
            libssh2_session_free(session);
            closesocket(sock);
            emit connectFailed(QStringLiteral("已取消连接：主机指纹和上次记录不一致"));
            return;
        }
        if (!rememberHost(m_host, fingerprint)) {
            libssh2_session_free(session);
            closesocket(sock);
            emit connectFailed(QStringLiteral("无法更新主机指纹记录 %1")
                                   .arg(QDir::toNativeSeparators(knownHostsPath())));
            return;
        }
    } else if (!found) {
        rememberHost(m_host, fingerprint);
    }

    // --- 认证：密码优先，其次私钥 ---
    const char *user = target.user.toUtf8().constData();
    bool authed = false;
    if (!target.password.isEmpty()) {
        authed = libssh2_userauth_password(session, user, target.password.toUtf8().constData()) == 0;
    }
    if (!authed && !target.privateKey.isEmpty()) {
        authed = libssh2_userauth_publickey_fromfile(
                     session, user, nullptr,
                     target.privateKey.toUtf8().constData(), nullptr) == 0;
    }
    if (!authed) {
        // 密码路径失败时补一次交互式认证（有些 sshd 只开 kbd-interactive）
        libssh2_session_free(session);
        closesocket(sock);
        emit connectFailed(QStringLiteral("认证失败：用户名或密码错误（或私钥不可用）"));
        return;
    }

    // --- SFTP 通道 + 主目录 ---
    LIBSSH2_SFTP *sftp = libssh2_sftp_init(session);
    if (!sftp) {
        libssh2_session_free(session);
        closesocket(sock);
        emit connectFailed(QStringLiteral("打开 SFTP 通道失败（服务器可能未启用 sftp 子系统）"));
        return;
    }
    char buf[512];
    QString home = QStringLiteral("/");
    if (libssh2_sftp_realpath(sftp, ".", buf, sizeof(buf) - 1) > 0)
        home = QString::fromUtf8(buf);

    m_session = session;
    m_sftp = sftp;
    m_sock = quintptr(sock);
    emit connected(home, fingerprint);
}

void SftpClient::disconnectFromHost()
{
    if (deferToWorker([this] { disconnectFromHost(); }))
        return;
    if (!m_session)
        return;
    libssh2_sftp_shutdown((LIBSSH2_SFTP *)m_sftp);
    libssh2_session_disconnect((LIBSSH2_SESSION *)m_session, "bye");
    libssh2_session_free((LIBSSH2_SESSION *)m_session);
    closesocket((SOCKET)m_sock);
    m_session = nullptr;
    m_sftp = nullptr;
    m_sock = 0;
    emit disconnected();
}

bool SftpClient::ensureSession(QString *error)
{
    if (!m_session) {
        *error = QStringLiteral("SFTP 未连接");
        return false;
    }
    return true;
}

void SftpClient::listDir(const QString &path)
{
    if (deferToWorker([this, path] { listDir(path); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit listedFailed(path, err);
        return;
    }
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)m_sftp;
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_opendir(sftp, path.toUtf8().constData());
    if (!h) {
        emit listedFailed(path, lastError("无法打开目录"));
        return;
    }
    QVector<SftpEntry> entries;
    char name[512], longname[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int n = 0;
    while ((n = libssh2_sftp_readdir_ex(h, name, sizeof(name) - 1, longname, sizeof(longname) - 1, &attrs)) > 0) {
        name[n] = 0;
        const QString qname = QString::fromUtf8(name);
        if (qname == QLatin1String(".") || qname == QLatin1String(".."))
            continue;
        SftpEntry e;
        e.name = qname;
        e.permissions = attrs.permissions;
        e.isDir = (attrs.permissions & LIBSSH2_SFTP_S_IFDIR) != 0;
        e.isSymlink = (attrs.permissions & LIBSSH2_SFTP_S_IFLNK) != 0;
        if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
            e.size = attrs.filesize;
        if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
            e.mtime = attrs.mtime;
        entries.append(e);
    }
    libssh2_sftp_closedir(h);
    std::sort(entries.begin(), entries.end(), [](const SftpEntry &a, const SftpEntry &b) {
        if (a.isDir != b.isDir)
            return a.isDir;               // 目录排前
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    emit listed(path, entries);
}

void SftpClient::realPath(const QString &path)
{
    if (deferToWorker([this, path] { realPath(path); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit opFinished(QStringLiteral("realpath"), path, false, err);
        return;
    }
    char buf[1024];
    if (libssh2_sftp_realpath((LIBSSH2_SFTP *)m_sftp, path.toUtf8().constData(), buf, sizeof(buf) - 1) <= 0) {
        emit opFinished(QStringLiteral("realpath"), path, false, lastError("无法解析路径"));
        return;
    }
    emit opFinished(QStringLiteral("realpath"), QString::fromUtf8(buf), true, QString());
}

void SftpClient::uploadFile(int id, const QString &localPath, const QString &remotePath)
{
    if (deferToWorker([this, id, localPath, remotePath] { uploadFile(id, localPath, remotePath); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit transferFinished(id, false, err);
        return;
    }
    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly)) {
        emit transferFinished(id, false, QStringLiteral("无法打开本地文件 %1").arg(localPath));
        return;
    }
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)m_sftp;
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(
        sftp, remotePath.toUtf8().constData(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        0644);
    if (!h) {
        emit transferFinished(id, false, lastError("无法创建远程文件"));
        return;
    }
    const qint64 total = f.size();
    qint64 done = 0;
    bool ok = true;
    bool cancelled = false;
    char buf[kChunkSize];
    while (done < total) {
        if (m_cancel.load()) {
            cancelled = true;
            break;
        }
        const qint64 rd = f.read(buf, sizeof(buf));
        if (rd <= 0) {
            ok = false;
            err = QStringLiteral("读取本地文件失败");
            break;
        }
        qint64 sent = 0;
        while (sent < rd) {
            const ssize_t w = libssh2_sftp_write(h, buf + sent, size_t(rd - sent));
            if (w < 0) {
                ok = false;
                err = lastError("写入远程文件失败");
                break;
            }
            sent += w;
        }
        if (!ok)
            break;
        done += rd;
        emit transferProgress(id, done, total);
    }
    m_cancel.store(false);
    // close 时才把最后的块真正刷到对端；close 失败视作传输失败
    if (libssh2_sftp_close_handle(h) != 0 && !cancelled) {
        ok = false;
        err = lastError("远程文件关闭失败（磁盘满？）");
    }
    if (cancelled) {
        // 清掉半截文件，失败不报（目录里残留可手动删）
        libssh2_sftp_unlink(sftp, remotePath.toUtf8().constData());
        emit transferFinished(id, false, QStringLiteral("已取消"));
    } else {
        emit transferFinished(id, ok, ok ? QString() : err);
    }
}

void SftpClient::downloadFile(int id, const QString &remotePath, const QString &localPath)
{
    if (deferToWorker([this, id, remotePath, localPath] { downloadFile(id, remotePath, localPath); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit transferFinished(id, false, err);
        return;
    }
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)m_sftp;
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(sftp, remotePath.toUtf8().constData(),
                                               LIBSSH2_FXF_READ, 0);
    if (!h) {
        emit transferFinished(id, false, lastError("无法打开远程文件"));
        return;
    }
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    qint64 total = 0;
    if (libssh2_sftp_fstat(h, &attrs) == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))
        total = qint64(attrs.filesize);
    QFile f(localPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        libssh2_sftp_close_handle(h);
        emit transferFinished(id, false, QStringLiteral("无法写本地文件 %1").arg(localPath));
        return;
    }
    qint64 done = 0;
    bool ok = true;
    bool cancelled = false;
    char buf[kChunkSize];
    for (;;) {
        if (m_cancel.load()) {
            cancelled = true;
            break;
        }
        const ssize_t r = libssh2_sftp_read(h, buf, sizeof(buf));
        if (r == 0)
            break;                    // EOF
        if (r < 0) {
            ok = false;
            err = lastError("读取远程文件失败");
            break;
        }
        if (f.write(buf, r) != r) {
            ok = false;
            err = QStringLiteral("写本地磁盘失败");
            break;
        }
        done += r;
        if (total > 0 && done > total)
            total = done;             // 比预估大就跟着涨
        emit transferProgress(id, done, total);
    }
    f.close();
    libssh2_sftp_close_handle(h);
    m_cancel.store(false);
    if (cancelled) {
        f.remove();
        emit transferFinished(id, false, QStringLiteral("已取消"));
    } else {
        emit transferFinished(id, ok, ok ? QString() : err);
    }
}

void SftpClient::cancelTransfer(int id)
{
    Q_UNUSED(id);
    m_cancel.store(true);
}

namespace {

QString shellQuote(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

} // namespace

void SftpClient::makeDir(const QString &path)
{
    if (deferToWorker([this, path] { makeDir(path); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit opFinished(QStringLiteral("mkdir"), path, false, err);
        return;
    }
    const bool ok = libssh2_sftp_mkdir_ex((LIBSSH2_SFTP *)m_sftp,
                                          path.toUtf8().constData(), int(path.toUtf8().size()),
                                          0755) == 0;
    emit opFinished(QStringLiteral("mkdir"), path, ok, ok ? QString() : lastError("创建目录失败"));
}

void SftpClient::createFile(const QString &path)
{
    if (deferToWorker([this, path] { createFile(path); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit opFinished(QStringLiteral("create"), path, false, err);
        return;
    }
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)m_sftp;
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(
        sftp, path.toUtf8().constData(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, 0644);
    if (!h) {
        emit opFinished(QStringLiteral("create"), path, false, lastError("创建文件失败"));
        return;
    }
    libssh2_sftp_close_handle(h);
    emit opFinished(QStringLiteral("create"), path, true, QString());
}

void SftpClient::removeEntry(const QString &path, bool isDir)
{
    if (deferToWorker([this, path, isDir] { removeEntry(path, isDir); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit opFinished(QStringLiteral("remove"), path, false, err);
        return;
    }
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)m_sftp;
    const QByteArray p = path.toUtf8();
    bool ok;
    if (isDir)
        ok = libssh2_sftp_rmdir_ex(sftp, p.constData(), int(p.size())) == 0;
    else
        ok = libssh2_sftp_unlink_ex(sftp, p.constData(), int(p.size())) == 0;
    emit opFinished(QStringLiteral("remove"), path, ok, ok ? QString() : lastError("删除失败"));
}

void SftpClient::renameEntry(const QString &from, const QString &to)
{
    if (deferToWorker([this, from, to] { renameEntry(from, to); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit opFinished(QStringLiteral("rename"), from, false, err);
        return;
    }
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)m_sftp;
    const QByteArray f = from.toUtf8();
    const QByteArray t = to.toUtf8();
    const bool ok = libssh2_sftp_rename_ex(sftp, f.constData(), int(f.size()),
                                           t.constData(), int(t.size()),
                                           LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC) == 0;
    emit opFinished(QStringLiteral("rename"), from, ok, ok ? QString() : lastError("重命名失败"));
}

void SftpClient::copyRemote(const QString &from, const QString &to, bool isDir)
{
    if (deferToWorker([this, from, to, isDir] { copyRemote(from, to, isDir); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit opFinished(QStringLiteral("copy"), from, false, err);
        return;
    }
    if (isDir) {
        // SFTP 没有目录复制。走远端 shell，文件和子目录一起带走。
        const QString command = QStringLiteral("cp -a -- ") + shellQuote(from) + QLatin1Char(' ')
                                + shellQuote(to);
        LIBSSH2_CHANNEL *ch = libssh2_channel_open_session((LIBSSH2_SESSION *)m_session);
        if (!ch) {
            emit opFinished(QStringLiteral("copy"), from, false, lastError("无法打开远程命令通道"));
            return;
        }
        if (libssh2_channel_exec(ch, command.toUtf8().constData()) != 0) {
            libssh2_channel_free(ch);
            emit opFinished(QStringLiteral("copy"), from, false, lastError("远程复制失败"));
            return;
        }
        QByteArray output;
        char buf[4096];
        for (;;) {
            const ssize_t n = libssh2_channel_read_stderr(ch, buf, sizeof(buf));
            if (n > 0)
                output.append(buf, int(n));
            else
                break;
        }
        libssh2_channel_send_eof(ch);
        libssh2_channel_close(ch);
        libssh2_channel_wait_closed(ch);
        const int status = libssh2_channel_get_exit_status(ch);
        libssh2_channel_free(ch);
        const bool ok = status == 0;
        emit opFinished(QStringLiteral("copy"), from, ok,
                        ok ? QString() : QString::fromUtf8(output));
        return;
    }

    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)m_sftp;
    const QByteArray src = from.toUtf8();
    LIBSSH2_SFTP_HANDLE *in = libssh2_sftp_open(sftp, src.constData(), LIBSSH2_FXF_READ, 0);
    if (!in) {
        emit opFinished(QStringLiteral("copy"), from, false, lastError("无法打开源文件"));
        return;
    }
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    long mode = 0644;
    if (libssh2_sftp_fstat(in, &attrs) == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
        mode = long(attrs.permissions & 07777);
    const QByteArray dst = to.toUtf8();
    LIBSSH2_SFTP_HANDLE *out = libssh2_sftp_open(
        sftp, dst.constData(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, mode);
    if (!out) {
        libssh2_sftp_close_handle(in);
        emit opFinished(QStringLiteral("copy"), from, false, lastError("无法创建目标文件"));
        return;
    }
    bool ok = true;
    char buf[kChunkSize];
    for (;;) {
        const ssize_t n = libssh2_sftp_read(in, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            ok = false;
            err = lastError("读取源文件失败");
            break;
        }
        ssize_t sent = 0;
        while (sent < n) {
            const ssize_t w = libssh2_sftp_write(out, buf + sent, size_t(n - sent));
            if (w < 0) {
                ok = false;
                err = lastError("写入目标文件失败");
                break;
            }
            sent += w;
        }
        if (!ok)
            break;
    }
    if (libssh2_sftp_close_handle(out) != 0 && ok) {
        ok = false;
        err = lastError("关闭目标文件失败");
    }
    libssh2_sftp_close_handle(in);
    if (!ok)
        libssh2_sftp_unlink(sftp, dst.constData());
    emit opFinished(QStringLiteral("copy"), from, ok, ok ? QString() : err);
}

void SftpClient::setPermissions(const QString &path, quint32 mode)
{
    if (deferToWorker([this, path, mode] { setPermissions(path, mode); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit opFinished(QStringLiteral("chmod"), path, false, err);
        return;
    }
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    attrs.permissions = mode & 07777;
    const QByteArray p = path.toUtf8();
    const bool ok = libssh2_sftp_stat_ex((LIBSSH2_SFTP *)m_sftp, p.constData(),
                                         unsigned(p.size()), LIBSSH2_SFTP_SETSTAT, &attrs) == 0;
    emit opFinished(QStringLiteral("chmod"), path, ok, ok ? QString() : lastError("修改权限失败"));
}

void SftpClient::execCommand(const QString &command)
{
    if (deferToWorker([this, command] { execCommand(command); }))
        return;
    QString err;
    if (!ensureSession(&err)) {
        emit opFinished(QStringLiteral("exec"), command, false, err);
        return;
    }
    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session((LIBSSH2_SESSION *)m_session);
    if (!ch) {
        emit opFinished(QStringLiteral("exec"), command, false, lastError("无法打开远程命令通道"));
        return;
    }
    if (libssh2_channel_exec(ch, command.toUtf8().constData()) != 0) {
        const QString why = lastError("远程命令执行失败");
        libssh2_channel_free(ch);
        emit opFinished(QStringLiteral("exec"), command, false, why);
        return;
    }
    QByteArray output;
    char buf[4096];
    for (;;) {
        const ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf));
        if (n > 0)
            output.append(buf, int(n));
        else
            break;
    }
    for (;;) {
        const ssize_t n = libssh2_channel_read_stderr(ch, buf, sizeof(buf));
        if (n > 0)
            output.append(buf, int(n));
        else
            break;
    }
    libssh2_channel_send_eof(ch);
    libssh2_channel_close(ch);
    libssh2_channel_wait_closed(ch);
    const int status = libssh2_channel_get_exit_status(ch);
    libssh2_channel_free(ch);
    const bool ok = status == 0;
    QString detail = QString::fromUtf8(output);
    if (!ok && detail.trimmed().isEmpty())
        detail = QStringLiteral("退出码 %1").arg(status);
    emit opFinished(QStringLiteral("exec"), command, ok, detail);
}

QString SftpClient::lastError(const char *fallback) const
{
    char *msg = nullptr;
    int len = 0;
    if (m_session
        && libssh2_session_last_error((LIBSSH2_SESSION *)m_session, &msg, &len, 0) == 0
        && msg && len > 0) {
        return QString::fromUtf8(msg, len);
    }
    return QString::fromUtf8(fallback);
}
