#include "hoststats.h"

#include "sftpclient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <cstdlib>
#include <cstring>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh2.h>

namespace {
constexpr int kSessionTimeoutMs = 8000;
constexpr int kConnectTimeoutMs = 8000;
constexpr int kSampleIntervalMs = 3000;
constexpr int kRetryIntervalMs = 5000;

struct CpuSnap
{
    bool ok = false;
    quint64 total = 0;
    quint64 idle = 0;
};

struct MemSnap
{
    bool ok = false;
    bool percentOk = false;
    int percent = 0;
    qint64 usedKiB = 0;
    qint64 totalKiB = 0;
};

struct KnownHost
{
    QString key;
    QString sha256;
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

qint64 memInfoKiB(const QString &text, const char *key)
{
    const QString prefix = QString::fromLatin1(key) + QLatin1Char(':');
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(prefix))
            continue;
        const QStringList parts = trimmed.mid(prefix.size()).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.isEmpty())
            return -1;
        bool ok = false;
        const qint64 v = parts.first().toLongLong(&ok);
        return ok ? v : -1;
    }
    return -1;
}

CpuSnap parseCpu(const QString &text)
{
    CpuSnap snap;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QLatin1String("cpu ")))
            continue;
        const QStringList parts = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        // cpu user nice system idle iowait irq softirq steal
        // guest / guest_nice 已经含在 user / nice 里，不能再加进总量。
        if (parts.size() < 5)
            return snap;
        const int n = qMin(parts.size() - 1, 8);
        quint64 vals[8] = {};
        for (int i = 0; i < n; ++i) {
            bool ok = false;
            vals[i] = parts.at(i + 1).toULongLong(&ok);
            if (!ok)
                return snap;
            snap.total += vals[i];
        }
        snap.idle = vals[3];
        if (n > 4)
            snap.idle += vals[4];
        snap.ok = snap.total > 0;
        return snap;
    }
    return snap;
}

MemSnap parseMem(const QString &text)
{
    MemSnap snap;
    const qint64 total = memInfoKiB(text, "MemTotal");
    if (total <= 0)
        return snap;
    qint64 available = memInfoKiB(text, "MemAvailable");
    if (available < 0) {
        const qint64 freeKiB = memInfoKiB(text, "MemFree");
        const qint64 buffers = memInfoKiB(text, "Buffers");
        const qint64 cached = memInfoKiB(text, "Cached");
        if (freeKiB < 0)
            return snap;
        available = freeKiB;
        if (buffers > 0)
            available += buffers;
        if (cached > 0)
            available += cached;
    }
    if (available < 0)
        available = 0;
    if (available > total)
        available = total;
    snap.ok = true;
    snap.totalKiB = total;
    snap.usedKiB = total - available;
    snap.percentOk = true;
    snap.percent = int(snap.usedKiB * 100 / total);
    if (snap.percent < 0)
        snap.percent = 0;
    if (snap.percent > 100)
        snap.percent = 100;
    return snap;
}

void kbdCallback(const char *, int, const char *, int, int numPrompts,
                 const LIBSSH2_USERAUTH_KBDINT_PROMPT *,
                 LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
{
    const auto *password = static_cast<const QByteArray *>(*abstract);
    if (!password)
        return;
    for (int i = 0; i < numPrompts; ++i) {
        auto *text = static_cast<char *>(std::malloc(size_t(password->size()) + 1));
        if (!text)
            continue;
        if (!password->isEmpty())
            std::memcpy(text, password->constData(), size_t(password->size()));
        text[password->size()] = '\0';
        responses[i].text = text;
        responses[i].length = unsigned(password->size());
    }
}

bool authWithKey(LIBSSH2_SESSION *session, const QByteArray &user, const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    const QByteArray key = path.toUtf8();
    return libssh2_userauth_publickey_fromfile(session, user.constData(), nullptr,
                                                key.constData(), nullptr) == 0;
}
} // namespace

HostStats::HostStats()
{
    Q_ASSERT(!parent());
    qRegisterMetaType<SshTarget>("SshTarget");

    m_thread = new QThread;
    moveToThread(m_thread);
    m_thread->start();
}

HostStats::~HostStats()
{
    QMetaObject::invokeMethod(this, &HostStats::stop, Qt::QueuedConnection);
    m_thread->quit();
    m_thread->wait(20000);
    delete m_thread;
}

void HostStats::setHostKeyPrompter(HostKeyPrompter prompter)
{
    m_prompter = std::move(prompter);
}

bool HostStats::deferToWorker(const std::function<void()> &fn)
{
    if (QThread::currentThread() == thread())
        return false;
    QMetaObject::invokeMethod(this, fn, Qt::QueuedConnection);
    return true;
}

void HostStats::start(const SshTarget &target)
{
    if (deferToWorker([this, target] { start(target); }))
        return;
    closeSession();
    m_target = target;
    m_stopped = false;
    m_giveUp = false;
    m_haveCpu = false;
    m_prevTotal = 0;
    m_prevIdle = 0;
    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &HostStats::onTick);
    }
    m_timer->start(0);
}

void HostStats::stop()
{
    if (deferToWorker([this] { stop(); }))
        return;
    m_stopped = true;
    if (m_timer) {
        m_timer->stop();
        delete m_timer;
        m_timer = nullptr;
    }
    closeSession();
}

void HostStats::onTick()
{
    if (m_stopped || m_giveUp)
        return;
    if (!m_session && !openSession()) {
        if (!m_stopped && !m_giveUp && m_timer)
            m_timer->start(kRetryIntervalMs);
        return;
    }
    QByteArray output;
    const bool got = readProc(&output);
    const QString text = QString::fromUtf8(output);
    const CpuSnap cpu = got ? parseCpu(text) : CpuSnap{};
    const MemSnap mem = got ? parseMem(text) : MemSnap{};
    bool cpuOk = false;
    int cpuPercent = 0;
    if (cpu.ok && m_haveCpu && cpu.total > m_prevTotal) {
        const quint64 dt = cpu.total - m_prevTotal;
        quint64 di = cpu.idle >= m_prevIdle ? cpu.idle - m_prevIdle : 0;
        if (di > dt)
            di = dt;
        cpuPercent = int((dt - di) * 100 / dt);
        cpuOk = true;
    }
    if (cpu.ok) {
        m_haveCpu = true;
        m_prevTotal = cpu.total;
        m_prevIdle = cpu.idle;
    }
    if (!got) {
        closeSession();
        m_haveCpu = false;
    }
    if (!m_stopped) {
        emit statsUpdated(cpuOk, cpuPercent, mem.ok, mem.percentOk, mem.percent,
                          mem.usedKiB, mem.totalKiB);
        if (m_timer)
            m_timer->start(got ? kSampleIntervalMs : kRetryIntervalMs);
    }
}

bool HostStats::confirmHostKey(const QString &host, const QString &stored, const QString &current)
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

bool HostStats::rememberHost(const QString &key, const QString &fingerprint)
{
    const QString path = SftpClient::knownHostsPath();
    QStringList lines;
    QFile in(path);
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
    QDir().mkpath(QFileInfo(path).absolutePath());
    {
        QSaveFile out(path);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(payload);
            if (out.commit())
                return true;
        }
    }
    QFile plain(path);
    if (!plain.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return plain.write(payload) == payload.size();
}

bool HostStats::openSession()
{
    if (m_target.host.isEmpty() || m_target.user.isEmpty()) {
        m_giveUp = true;
        return false;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    const QString portText = QString::number(m_target.port > 0 ? m_target.port : 22);
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(m_target.host.toUtf8().constData(), portText.toUtf8().constData(),
                    &hints, &res) != 0 || !res) {
        return false;
    }
    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        return false;
    }
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
            return false;
        }
        int soerr = 0;
        int solen = int(sizeof(soerr));
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&soerr, &solen);
        if (soerr != 0) {
            closesocket(sock);
            return false;
        }
    } else if (rc != 0) {
        closesocket(sock);
        return false;
    }
    nb = 0;
    ioctlsocket(sock, FIONBIO, &nb);
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        closesocket(sock);
        return false;
    }
    libssh2_session_set_timeout(session, kSessionTimeoutMs);
    if (libssh2_session_handshake(session, (libssh2_socket_t)sock) != 0) {
        libssh2_session_free(session);
        closesocket(sock);
        return false;
    }

    const char *hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!hash) {
        libssh2_session_free(session);
        closesocket(sock);
        return false;
    }
    const QString fingerprint = QString::fromLatin1(QByteArray(hash, 32).toHex());
    const QString hostKey = QStringLiteral("%1:%2").arg(m_target.host, portText);
    QString khContent;
    {
        QFile khFile(SftpClient::knownHostsPath());
        if (khFile.open(QIODevice::ReadOnly))
            khContent = QString::fromUtf8(khFile.readAll());
    }
    bool found = false;
    QString stored;
    for (const KnownHost &kh : parseKnownHosts(khContent)) {
        if (kh.key == hostKey) {
            found = true;
            stored = kh.sha256;
            break;
        }
    }
    if (found && stored != fingerprint) {
        if (m_stopped || !confirmHostKey(hostKey, stored, fingerprint) || m_stopped
            || !rememberHost(hostKey, fingerprint)) {
            libssh2_session_free(session);
            closesocket(sock);
            if (!m_stopped)
                m_giveUp = true;
            return false;
        }
    } else if (!found) {
        rememberHost(hostKey, fingerprint);
    }

    const QByteArray user = m_target.user.toUtf8();
    const QByteArray password = m_target.password.toUtf8();
    bool authed = false;
    if (!password.isEmpty())
        authed = libssh2_userauth_password(session, user.constData(), password.constData()) == 0;
    if (!authed && !password.isEmpty()) {
        *libssh2_session_abstract(session) = const_cast<QByteArray *>(&password);
        authed = libssh2_userauth_keyboard_interactive(session, user.constData(), &kbdCallback) == 0;
        *libssh2_session_abstract(session) = nullptr;
    }
    if (!authed)
        authed = authWithKey(session, user, m_target.privateKey);
    if (!authed && m_target.privateKey.isEmpty()) {
        const QStringList defaults = {
            QDir::home().filePath(QStringLiteral(".ssh/id_ed25519")),
            QDir::home().filePath(QStringLiteral(".ssh/id_ecdsa")),
            QDir::home().filePath(QStringLiteral(".ssh/id_rsa")),
        };
        for (const QString &path : defaults) {
            if (authWithKey(session, user, path)) {
                authed = true;
                break;
            }
        }
    }
    if (!authed) {
        libssh2_session_free(session);
        closesocket(sock);
        m_giveUp = true;
        return false;
    }

    m_session = session;
    m_sock = quintptr(sock);
    return true;
}

void HostStats::closeSession()
{
    if (m_session) {
        libssh2_session_disconnect((LIBSSH2_SESSION *)m_session, "bye");
        libssh2_session_free((LIBSSH2_SESSION *)m_session);
        m_session = nullptr;
    }
    if (m_sock) {
        closesocket((SOCKET)m_sock);
        m_sock = 0;
    }
}

bool HostStats::execRead(const char *command, QByteArray *output)
{
    if (!m_session)
        return false;
    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session((LIBSSH2_SESSION *)m_session);
    if (!channel)
        return false;
    if (libssh2_channel_exec(channel, command) != 0) {
        libssh2_channel_free(channel);
        return false;
    }
    char buf[4096];
    for (;;) {
        const ssize_t n = libssh2_channel_read(channel, buf, sizeof(buf));
        if (n > 0) {
            output->append(buf, int(n));
            if (output->size() > 256 * 1024)
                break;
        } else {
            break;
        }
    }
    for (;;) {
        const ssize_t n = libssh2_channel_read_stderr(channel, buf, sizeof(buf));
        if (n <= 0)
            break;
    }
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    return true;
}

bool HostStats::readProc(QByteArray *output)
{
    if (execRead("/bin/cat /proc/stat /proc/meminfo", output) && output->contains("MemTotal"))
        return true;
    output->clear();
    return execRead("cat /proc/stat /proc/meminfo", output) && output->contains("MemTotal");
}
