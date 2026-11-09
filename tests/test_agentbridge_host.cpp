#include <QtTest>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>

#include <functional>

#include "agentbridge.h"
#include "hostbridge.h"

/*
 * HostBridge + xterm.host.* 工具端到端测试（M2.c）。
 *
 * 后端纯内存 fake：snapshot 返回手造缓存帧，exec 按命令回放预置的
 * df / /proc / ps 样例文本；hooksForStats 的真实桥接由 test_sftpclient
 * 覆盖。独立管道名 + 临时 agent.json，不碰真实配置与网络。
 */

struct FakeHost
{
    HostSnapshot snap;
    QString dfText;
    QString uptimeText;
    QString psText;
    QString failError;  // 非空时 exec 一律失败（错误冒泡用）
    QStringList commands; // 收到的命令记录

    HostBridge::HostHooks hooks()
    {
        HostBridge::HostHooks h;
        h.snapshot = [this] { return snap; };
        h.exec = [this](const QString &command,
                        const std::function<void(const QString &, const QString &)> &done) {
            commands << command;
            if (!failError.isEmpty()) {
                done({}, failError);
                return;
            }
            if (command.contains(QLatin1String("df "))) {
                done(dfText, {});
                return;
            }
            if (command.contains(QLatin1String("/proc/uptime"))) {
                done(uptimeText, {});
                return;
            }
            if (command.contains(QLatin1String("ps "))) {
                done(psText, {});
                return;
            }
            done({}, QStringLiteral("未知命令: %1").arg(command));
        };
        return h;
    }
};

class AgentBridgeHostTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void toolsListIncludesHostTools();
    void cpuAndMemFromSnapshot();
    void notReadyReturnsNote();
    void diskParsesDf();
    void uptimeParsesProc();
    void topParsesPsAndHonorsCount();
    void execFailureBubbles();
    void parseHelpersAreRobust();

private:
    QJsonObject call(const QString &method, const QJsonObject &params = {});

    AgentBridge *m_bridge = nullptr;
    QTemporaryDir m_dir;
    QString m_pipe;
    QString m_token;
    HostBridge m_host;
    FakeHost m_fake;
};

void AgentBridgeHostTest::initTestCase()
{
    m_fake.snap.cpuOk = true;
    m_fake.snap.cpuPercent = 37;
    m_fake.snap.memOk = true;
    m_fake.snap.memPercentOk = true;
    m_fake.snap.memPercent = 62;
    m_fake.snap.usedKiB = 65024092;  // 62 GiB
    m_fake.snap.totalKiB = 104857600; // 100 GiB
    m_fake.dfText = QStringLiteral(
        "Filesystem     1024-blocks      Used Available Capacity Mounted on\n"
        "/dev/sda1        61662668  20323096  38166548      35% /\n"
        "tmpfs              999720         0    999720       0% /dev/shm\n");
    m_fake.uptimeText =
        QStringLiteral("8615.93 33472.85\n0.52 0.58 0.59 1/567 8901\n");
    m_fake.psText = QStringLiteral(
        "USER       PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND\n"
        "root         1  0.0  0.3 169372  6608 ?        Ss   10:00   0:05 /sbin/init splash\n"
        "dev        842 12.5  1.2 3200400 99012 ?        Sl   10:01   1:23 python3 server.py\n"
        "dev        950  2.1  0.8  90000 21000 pts/0    R+   10:05   0:01 top -d 2\n");

    m_pipe = QStringLiteral("XtermAgentHostTest-%1").arg(qApp->applicationPid());
    m_bridge = new AgentBridge(this, m_pipe);
    AgentBridge::Hooks hooks; // Host 用例不碰会话钩子，留空即可
    m_host.setBackend(m_fake.hooks());
    m_host.registerTools(&hooks);
    m_bridge->start(hooks, m_dir.filePath(QStringLiteral("agent.json")));

    QFile f(m_dir.filePath(QStringLiteral("agent.json")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject agent = QJsonDocument::fromJson(f.readAll()).object();
    m_token = agent.value(QStringLiteral("token")).toString();
    QVERIFY(!m_token.isEmpty());
}

QJsonObject AgentBridgeHostTest::call(const QString &method, const QJsonObject &params)
{
    QLocalSocket sock;
    sock.connectToServer(m_pipe);
    if (!sock.waitForConnected(2000))
        return {};
    QJsonObject req;
    req.insert(QStringLiteral("id"), 1);
    req.insert(QStringLiteral("token"), m_token);
    req.insert(QStringLiteral("method"), method);
    req.insert(QStringLiteral("params"), params);
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    sock.write("\n");
    for (int i = 0; i < 300 && !sock.canReadLine(); ++i)
        QTest::qWait(10);
    if (!sock.canReadLine())
        return {};
    return QJsonDocument::fromJson(sock.readLine()).object();
}

// tools.list：五件 Host 工具都注册进了清单（M2.a 自发现对扩展工具生效）
void AgentBridgeHostTest::toolsListIncludesHostTools()
{
    const QJsonObject resp = call(QStringLiteral("tools.list"));
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    QStringList names;
    const QJsonArray tools = resp.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    for (const QJsonValue &v : tools)
        names << v.toObject().value(QStringLiteral("name")).toString();
    for (const char *name : { "xterm.host.cpu", "xterm.host.mem", "xterm.host.disk",
                              "xterm.host.uptime", "xterm.host.top" })
        QVERIFY2(names.contains(QString::fromLatin1(name)), name);
}

// cpu/mem：直接读 stats 缓存帧，不产生远端命令
void AgentBridgeHostTest::cpuAndMemFromSnapshot()
{
    const int commandsBefore = m_fake.commands.size();
    const QJsonObject cpu =
        call(QStringLiteral("xterm.host.cpu")).value(QStringLiteral("result")).toObject();
    QVERIFY(cpu.value(QStringLiteral("ok")).toBool());
    QCOMPARE(cpu.value(QStringLiteral("percent")).toInt(), 37);

    const QJsonObject mem =
        call(QStringLiteral("xterm.host.mem")).value(QStringLiteral("result")).toObject();
    QVERIFY(mem.value(QStringLiteral("ok")).toBool());
    QCOMPARE(mem.value(QStringLiteral("percent")).toInt(), 62);
    QCOMPARE(mem.value(QStringLiteral("used_kib")).toDouble(), double(65024092));
    QCOMPARE(mem.value(QStringLiteral("total_kib")).toDouble(), double(104857600));
    QCOMPARE(m_fake.commands.size(), commandsBefore); // 纯缓存，不跑命令
}

// 未就绪：工具调用成功但 data.ok=false 附说明（CPU 差值需要第二帧）
void AgentBridgeHostTest::notReadyReturnsNote()
{
    HostBridge bridge;
    FakeHost cold; // 全默认 false
    bridge.setBackend(cold.hooks());
    AgentBridge::Hooks hooks;
    bridge.registerTools(&hooks);
    QVERIFY(hooks.tools.contains(QStringLiteral("xterm.host.cpu")));

    const AgentBridge::ToolResult cpu =
        hooks.tools.value(QStringLiteral("xterm.host.cpu"))({{}, nullptr, {}});
    QVERIFY(cpu.ok);
    QVERIFY(!cpu.data.value(QStringLiteral("ok")).toBool());
    QVERIFY(cpu.data.value(QStringLiteral("note")).toString().contains(QStringLiteral("重试")));

    const AgentBridge::ToolResult mem =
        hooks.tools.value(QStringLiteral("xterm.host.mem"))({{}, nullptr, {}});
    QVERIFY(mem.ok);
    QVERIFY(!mem.data.value(QStringLiteral("ok")).toBool());
}

// disk：df -P 输出解析成结构化挂载点（含 % 去除与数字列）
void AgentBridgeHostTest::diskParsesDf()
{
    const QJsonObject resp = call(QStringLiteral("xterm.host.disk"));
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonArray mounts = resp.value(QStringLiteral("result")).toObject()
                                  .value(QStringLiteral("mounts")).toArray();
    QCOMPARE(mounts.size(), 2);
    const QJsonObject root = mounts.at(0).toObject();
    QCOMPARE(root.value(QStringLiteral("filesystem")).toString(), QStringLiteral("/dev/sda1"));
    QCOMPARE(root.value(QStringLiteral("total_kib")).toDouble(), double(61662668));
    QCOMPARE(root.value(QStringLiteral("used_kib")).toDouble(), double(20323096));
    QCOMPARE(root.value(QStringLiteral("used_percent")).toInt(), 35);
    QCOMPARE(root.value(QStringLiteral("mounted_on")).toString(), QStringLiteral("/"));
    const QJsonObject shm = mounts.at(1).toObject();
    QCOMPARE(shm.value(QStringLiteral("used_percent")).toInt(), 0);
    QCOMPARE(shm.value(QStringLiteral("mounted_on")).toString(), QStringLiteral("/dev/shm"));
}

// uptime：/proc/uptime + loadavg → 秒数/人类可读/三档负载
void AgentBridgeHostTest::uptimeParsesProc()
{
    const QJsonObject resp = call(QStringLiteral("xterm.host.uptime"));
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonObject up = resp.value(QStringLiteral("result")).toObject();
    QCOMPARE(up.value(QStringLiteral("uptime_seconds")).toInteger(), qint64(8615));
    QCOMPARE(up.value(QStringLiteral("human")).toString(), QStringLiteral("2:23"));
    const QJsonObject load = up.value(QStringLiteral("load")).toObject();
    QCOMPARE(load.value(QStringLiteral("one")).toDouble(), 0.52);
    QCOMPARE(load.value(QStringLiteral("five")).toDouble(), 0.58);
    QCOMPARE(load.value(QStringLiteral("fifteen")).toDouble(), 0.59);
}

// top：ps 输出按 count 截断，COMMAND 含空格完整拼回
void AgentBridgeHostTest::topParsesPsAndHonorsCount()
{
    const QJsonObject resp = call(QStringLiteral("xterm.host.top"),
                                  {{QStringLiteral("count"), 2}});
    QVERIFY(resp.value(QStringLiteral("ok")).toBool());
    const QJsonArray rows = resp.value(QStringLiteral("result")).toObject()
                                .value(QStringLiteral("processes")).toArray();
    QCOMPARE(rows.size(), 2); // count=2 生效（表头不计）
    const QJsonObject init = rows.at(0).toObject();
    QCOMPARE(init.value(QStringLiteral("pid")).toInt(), 1);
    QCOMPARE(init.value(QStringLiteral("cpu_percent")).toDouble(), 0.0);
    QCOMPARE(init.value(QStringLiteral("command")).toString(),
             QStringLiteral("/sbin/init splash"));
    const QJsonObject py = rows.at(1).toObject();
    QCOMPARE(py.value(QStringLiteral("user")).toString(), QStringLiteral("dev"));
    QCOMPARE(py.value(QStringLiteral("cpu_percent")).toDouble(), 12.5);
    QCOMPARE(py.value(QStringLiteral("command")).toString(),
             QStringLiteral("python3 server.py"));
    // 命令带 count+1 的 head 截断（表头 + 2 条）
    QVERIFY(m_fake.commands.last().contains(QStringLiteral("head -n 3")));
}

// exec 失败：错误原样冒泡（未连接/退出码非零等场景）
void AgentBridgeHostTest::execFailureBubbles()
{
    m_fake.failError = QStringLiteral("SSH 会话未连接");
    const QJsonObject resp = call(QStringLiteral("xterm.host.disk"));
    QVERIFY(!resp.value(QStringLiteral("ok")).toBool());
    QVERIFY(resp.value(QStringLiteral("error")).toString()
                .contains(QStringLiteral("SSH 会话未连接")));
    m_fake.failError.clear();
}

// 解析函数对空/畸形输入不崩、不产数据
void AgentBridgeHostTest::parseHelpersAreRobust()
{
    QCOMPARE(HostBridge::parseDf(QString()).size(), 0);
    QCOMPARE(HostBridge::parseDf(QStringLiteral("Filesystem 1024-blocks")).size(), 0);
    QCOMPARE(HostBridge::parseDf(QStringLiteral("garbage line\n")).size(), 0);
    QVERIFY(HostBridge::parseUptime(QString()).isEmpty());
    QVERIFY(HostBridge::parseUptime(QStringLiteral("not a number\n")).isEmpty());
    QCOMPARE(HostBridge::parsePs(QStringLiteral("USER PID %CPU COMMAND"), 5).size(), 0);
    QCOMPARE(HostBridge::parsePs(QString(), 5).size(), 0);
    // 缺第二行（loadavg）时 uptime 仍可用，只是无 load 字段
    const QJsonObject up = HostBridge::parseUptime(QStringLiteral("120.5 240.0\n"));
    QCOMPARE(up.value(QStringLiteral("uptime_seconds")).toInteger(), qint64(120));
    QVERIFY(!up.contains(QStringLiteral("load")));
    // 跨天 human 格式
    const QJsonObject days =
        HostBridge::parseUptime(QStringLiteral("90061 0\n"));
    QCOMPARE(days.value(QStringLiteral("human")).toString(), QStringLiteral("1 天 1:01"));
}

QTEST_MAIN(AgentBridgeHostTest)
#include "test_agentbridge_host.moc"
