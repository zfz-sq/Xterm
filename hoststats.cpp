#include "hoststats.h"

#include "sshsession.h"

namespace {

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

} // namespace

HostStats::HostStats(SshSession *session, QObject *parent)
    : QObject(parent), m_session(session)
{
    if (m_session != nullptr) {
        connect(m_session, &SshSession::statsSample, this,
                &HostStats::onSample);
        connect(m_session, &SshSession::statsDegraded, this,
                &HostStats::onDegraded);
    }
}

HostCpuSample HostStats::parseCpuSample(const QString &text)
{
    HostCpuSample snap;
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

HostMemSample HostStats::parseMemSample(const QString &text)
{
    HostMemSample snap;
    const qint64 total = memInfoKiB(text, "MemTotal");
    if (total <= 0)
        return snap;
    qint64 available = memInfoKiB(text, "MemAvailable");
    if (available < 0) {
        // 没有 MemAvailable 的老内核：用 free+buffers+cached 近似。
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

void HostStats::resetDelta()
{
    m_haveCpu = false;
    m_prevTotal = 0;
    m_prevIdle = 0;
}

void HostStats::clearCache()
{
    m_cpuOk = false;
    m_cpuPercent = 0;
    m_memOk = false;
    m_memPercentOk = false;
    m_memPercent = 0;
    m_usedKiB = 0;
    m_totalKiB = 0;
}

QString HostStats::statusText() const
{
    QString id;
    if (m_session) {
        const SshTarget t = m_session->target();
        if (!t.user.isEmpty())
            id = t.user + QLatin1Char('@');
        id += t.host;
        id += QStringLiteral(":%1").arg(t.port > 0 ? t.port : 22);
    }

    const QString cpu = m_cpuOk ? QString::number(m_cpuPercent) : QStringLiteral("--");
    const QString memPct = m_memPercentOk ? QString::number(m_memPercent)
                                          : QStringLiteral("--");
    QString mem = QStringLiteral("--/--");
    if (m_memOk && m_totalKiB > 0) {
        if (m_totalKiB >= 1024 * 1024) {
            const auto gib = [](qint64 kib) {
                return QString::number(double(kib) / (1024.0 * 1024.0), 'f', 1);
            };
            mem = QStringLiteral("%1/%2G").arg(gib(m_usedKiB), gib(m_totalKiB));
        } else {
            const auto mib = [](qint64 kib) {
                return QString::number(qRound(double(kib) / 1024.0));
            };
            mem = QStringLiteral("%1/%2M").arg(mib(m_usedKiB), mib(m_totalKiB));
        }
    }
    return id + QStringLiteral("      ·     CPU ") + cpu
           + QStringLiteral("% · 内存 ") + memPct
           + QStringLiteral("% · ") + mem;
}

void HostStats::testApplySample(const QString &sample)
{
    applySample(sample);
}

HostSnapshot HostStats::snapshot() const
{
    HostSnapshot snap;
    snap.cpuOk = m_cpuOk;
    snap.cpuPercent = m_cpuPercent;
    snap.memOk = m_memOk;
    snap.memPercentOk = m_memPercentOk;
    snap.memPercent = m_memPercent;
    snap.usedKiB = m_usedKiB;
    snap.totalKiB = m_totalKiB;
    return snap;
}

void HostStats::onSample(const QString &sample)
{
    // 断开后仍在途的帧直接丢弃，不污染展示缓存（原 MainWindow 逻辑下沉）
    if (m_session && m_session->state() != Transport::Connected)
        return;
    applySample(sample);
}

void HostStats::onDegraded()
{
    updateCache(false, 0, false, false, 0, 0, 0);
    emit statsUpdated(false, 0, false, false, 0, 0, 0);
}

void HostStats::applySample(const QString &sample)
{
    const HostCpuSample cpu = parseCpuSample(sample);
    const HostMemSample mem = parseMemSample(sample);

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
    // 计数回退（远端重启）只是丢差值：重新基线，下一帧恢复。
    if (cpu.ok) {
        m_haveCpu = true;
        m_prevTotal = cpu.total;
        m_prevIdle = cpu.idle;
    }

    emit statsUpdated(cpuOk, cpuPercent, mem.ok, mem.percentOk, mem.percent,
                      mem.usedKiB, mem.totalKiB);
    updateCache(cpuOk, cpuPercent, mem.ok, mem.percentOk, mem.percent,
                mem.usedKiB, mem.totalKiB);
}

void HostStats::updateCache(bool cpuOk, int cpuPercent, bool memOk,
                            bool memPercentOk, int memPercent,
                            qint64 usedKiB, qint64 totalKiB)
{
    m_cpuOk = cpuOk;
    m_cpuPercent = cpuPercent;
    m_memOk = memOk;
    m_memPercentOk = memPercentOk;
    m_memPercent = memPercent;
    m_usedKiB = usedKiB;
    m_totalKiB = totalKiB;
}
