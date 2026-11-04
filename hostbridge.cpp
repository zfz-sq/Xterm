#include "hostbridge.h"

namespace {

// top 的默认/上限条数：head 在远端先截断，输出量恒小
constexpr int kDefaultTopCount = 10;
constexpr int kMaxTopCount = 50;

} // namespace

QJsonArray HostBridge::parseDf(const QString &text)
{
    QJsonArray mounts;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (trimmed.isEmpty())
            continue;
        if (i == 0 && trimmed.startsWith(QLatin1String("Filesystem")))
            continue; // 表头（命令带 LC_ALL=C，列名固定）
        const QStringList cols = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (cols.size() < 6)
            continue; // 不足 6 列的杂音行直接跳过
        QJsonObject o;
        o.insert(QStringLiteral("filesystem"), cols.at(0));
        o.insert(QStringLiteral("total_kib"), double(cols.at(1).toLongLong()));
        o.insert(QStringLiteral("used_kib"), double(cols.at(2).toLongLong()));
        o.insert(QStringLiteral("available_kib"), double(cols.at(3).toLongLong()));
        // at() 返回 const 引用，去掉容量列的 % 需要一份拷贝
        o.insert(QStringLiteral("used_percent"),
                 QString(cols.at(4)).remove(QLatin1Char('%')).toInt());
        // 挂载点含空格时第 6 列起原样拼回（df -P 保证不换行）
        o.insert(QStringLiteral("mounted_on"),
                 QStringList(cols.mid(5)).join(QLatin1Char(' ')));
        mounts.append(o);
    }
    return mounts;
}

QJsonObject HostBridge::parseUptime(const QString &text)
{
    QJsonObject out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty())
        return out;
    // /proc/uptime 第一行："<开机秒> <各核 idle 汇总>"（idle 可超开机时长）
    const QStringList up = lines.at(0).simplified().split(QLatin1Char(' '));
    bool ok = false;
    const double seconds = up.isEmpty() ? -1.0 : up.at(0).toDouble(&ok);
    if (!ok || seconds < 0)
        return out;
    out.insert(QStringLiteral("uptime_seconds"), qint64(seconds));
    const qint64 total = qint64(seconds);
    const int days = int(total / 86400);
    const int hours = int((total % 86400) / 3600);
    const int minutes = int((total % 3600) / 60);
    // 与 uptime 命令同风格：小时不补零、分钟补零，跨天加「N 天 」前缀
    const QString human = days > 0
        ? QStringLiteral("%1 天 %2:%3")
              .arg(days)
              .arg(hours)
              .arg(minutes, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2")
              .arg(hours)
              .arg(minutes, 2, 10, QLatin1Char('0'));
    out.insert(QStringLiteral("human"), human);
    // /proc/loadavg 第二行（可能缺失）："1min 5min 15min 运行/总数 最近PID"
    if (lines.size() > 1) {
        const QStringList load = lines.at(1).simplified().split(QLatin1Char(' '));
        if (load.size() >= 3) {
            QJsonObject l;
            l.insert(QStringLiteral("one"), load.at(0).toDouble());
            l.insert(QStringLiteral("five"), load.at(1).toDouble());
            l.insert(QStringLiteral("fifteen"), load.at(2).toDouble());
            out.insert(QStringLiteral("load"), l);
        }
    }
    return out;
}

QJsonArray HostBridge::parsePs(const QString &text, int maxRows)
{
    QJsonArray rows;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (rows.size() >= maxRows)
            break;
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1String("USER ")))
            continue; // 表头
        // USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND
        const QStringList cols = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (cols.size() < 11)
            continue; // COMMAND 被截断的畸形行跳过
        QJsonObject o;
        o.insert(QStringLiteral("user"), cols.at(0));
        o.insert(QStringLiteral("pid"), cols.at(1).toInt());
        o.insert(QStringLiteral("cpu_percent"), cols.at(2).toDouble());
        o.insert(QStringLiteral("mem_percent"), cols.at(3).toDouble());
        // COMMAND（第 11 列起）含空格，拼回；多余空白被压缩，可接受
        o.insert(QStringLiteral("command"),
                 QStringList(cols.mid(10)).join(QLatin1Char(' ')));
        rows.append(o);
    }
    return rows;
}

void HostBridge::registerTools(AgentBridge::Hooks *hooks)
{
    auto meta = [hooks](const char *name, const QString &desc, QJsonObject params) {
        const QString full = QString::fromLatin1(name);
        QJsonObject m;
        m.insert(QStringLiteral("name"), full);
        m.insert(QStringLiteral("description"), desc);
        m.insert(QStringLiteral("params"), params);
        hooks->toolMeta.insert(full, m);
    };

    // —— xterm.host.cpu / xterm.host.mem：stats 通道缓存帧，不跑命令 ——
    hooks->tools.insert(QStringLiteral("xterm.host.cpu"),
                        [this](const AgentBridge::ToolCall &) -> AgentBridge::ToolResult {
                            if (!m_hooks.snapshot)
                                return {false, false, {}, QStringLiteral("Host 后端未挂载")};
                            const HostSnapshot snap = m_hooks.snapshot();
                            QJsonObject data;
                            data.insert(QStringLiteral("ok"), snap.cpuOk);
                            if (snap.cpuOk) {
                                data.insert(QStringLiteral("percent"), snap.cpuPercent);
                            } else {
                                data.insert(QStringLiteral("note"),
                                            QStringLiteral("CPU 采样未就绪：需 SSH 已连接且 "
                                                           "stats 通道送出第二帧（连接后约 6 秒），"
                                                           "可稍后重试"));
                            }
                            return {true, false, data, QString()};
                        });
    meta("xterm.host.cpu",
         QStringLiteral("远端主机 CPU 使用率（百分比，来自状态栏采样通道，仅 SSH）"),
         QJsonObject());

    hooks->tools.insert(QStringLiteral("xterm.host.mem"),
                        [this](const AgentBridge::ToolCall &) -> AgentBridge::ToolResult {
                            if (!m_hooks.snapshot)
                                return {false, false, {}, QStringLiteral("Host 后端未挂载")};
                            const HostSnapshot snap = m_hooks.snapshot();
                            QJsonObject data;
                            data.insert(QStringLiteral("ok"), snap.memOk);
                            if (snap.memOk) {
                                data.insert(QStringLiteral("percent"), snap.memPercent);
                                data.insert(QStringLiteral("used_kib"), double(snap.usedKiB));
                                data.insert(QStringLiteral("total_kib"), double(snap.totalKiB));
                            } else {
                                data.insert(QStringLiteral("note"),
                                            QStringLiteral("内存采样未就绪：需 SSH 已连接"
                                                           "（首帧后即有）"));
                            }
                            return {true, false, data, QString()};
                        });
    meta("xterm.host.mem",
         QStringLiteral("远端主机内存用量（百分比与 KiB，来自状态栏采样通道，仅 SSH）"),
         QJsonObject());

    // —— xterm.host.disk / xterm.host.uptime / xterm.host.top：按需远端命令 ——
    hooks->tools.insert(QStringLiteral("xterm.host.disk"),
                        [this](const AgentBridge::ToolCall &) -> AgentBridge::ToolResult {
                            if (!m_hooks.exec)
                                return {false, false, {}, QStringLiteral("Host 后端未挂载")};
                            QString err;
                            const QString out =
                                m_hooks.exec(QStringLiteral("LC_ALL=C df -P -k"), &err);
                            if (!err.isEmpty())
                                return {false, false, {}, err};
                            QJsonObject data;
                            data.insert(QStringLiteral("mounts"), parseDf(out));
                            return {true, false, data, QString()};
                        });
    meta("xterm.host.disk",
         QStringLiteral("远端磁盘各挂载点用量（df，KiB 与百分比，仅 SSH）"),
         QJsonObject());

    hooks->tools.insert(QStringLiteral("xterm.host.uptime"),
                        [this](const AgentBridge::ToolCall &) -> AgentBridge::ToolResult {
                            if (!m_hooks.exec)
                                return {false, false, {}, QStringLiteral("Host 后端未挂载")};
                            QString err;
                            const QString out = m_hooks.exec(
                                QStringLiteral("cat /proc/uptime /proc/loadavg"), &err);
                            if (!err.isEmpty())
                                return {false, false, {}, err};
                            const QJsonObject data = parseUptime(out);
                            if (data.isEmpty())
                                return {false, false, {},
                                        QStringLiteral("无法解析 /proc/uptime 输出")};
                            return {true, false, data, QString()};
                        });
    meta("xterm.host.uptime",
         QStringLiteral("远端开机时长与平均负载（/proc，仅 SSH）"),
         QJsonObject());

    hooks->tools.insert(QStringLiteral("xterm.host.top"),
                        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            if (!m_hooks.exec)
                                return {false, false, {}, QStringLiteral("Host 后端未挂载")};
                            const int count = qBound(
                                1, call.params.value(QStringLiteral("count")).toInt(kDefaultTopCount),
                                kMaxTopCount);
                            // head 多带一行表头；count 已是整数无注入面，
                            // 格式串里的 "%cpu" 不是 %N 占位符、arg 不会动它
                            QString err;
                            const QString out = m_hooks.exec(
                                QStringLiteral("LC_ALL=C ps auxww --sort=-%cpu | head -n %1")
                                    .arg(count + 1),
                                &err);
                            if (!err.isEmpty())
                                return {false, false, {}, err};
                            QJsonObject data;
                            data.insert(QStringLiteral("processes"), parsePs(out, count));
                            return {true, false, data, QString()};
                        });
    meta("xterm.host.top",
         QStringLiteral("远端 CPU 占用最高的进程列表（ps，按 CPU 降序，仅 SSH）"),
         [] {
             QJsonObject params;
             QJsonObject count;
             count.insert(QStringLiteral("type"), QStringLiteral("integer"));
             count.insert(QStringLiteral("description"),
                          QStringLiteral("返回条数，默认 10，上限 50"));
             params.insert(QStringLiteral("count"), count);
             return params;
         }());
}
