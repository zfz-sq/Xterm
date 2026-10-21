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

void HostStats::testApplySample(const QString &sample)
{
    applySample(sample);
}

void HostStats::onSample(const QString &sample)
{
    applySample(sample);
}

void HostStats::onDegraded()
{
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
}
