// 现场诊断: 直接驱动 ConPty 跑系统 ssh.exe，原始字节双向落盘。
// 绕开 SshSession，用环境变量做参数级二分，定位「整屏空白、无法输入」的成因。
//   XTEST_NOTT=1          不加 -tt
//   XTEST_NO_PW=1         不代填密码（只等，看是否仍然刷屏）
//   XTEST_NO_DA_REPLY=1   不自动应答设备查询（conpty.cpp 侧读取）
//   XTEST_SEND=0          完全不发送任何输入
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

#include "../../conpty.h"
#include "../../sshsession.h"
#include "../../sshtarget.h"

static QJsonObject readSshBlock(const QString &cfgPath)
{
    QFile f(cfgPath);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QJsonArray profiles = root.value(QStringLiteral("profiles")).toArray();
    const QString last = root.value(QStringLiteral("lastProfile")).toString();
    for (const QJsonValue &v : profiles) {
        const QJsonObject p = v.toObject();
        if (p.value(QStringLiteral("name")).toString() == last)
            return p.value(QStringLiteral("ssh")).toObject();
    }
    return profiles.isEmpty() ? QJsonObject()
                              : profiles.first().toObject()
                                        .value(QStringLiteral("ssh")).toObject();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // E1: 纯输入通路自检 —— 跑一个会回显的程序，看我们写进去的字节能不能回来
    if (qEnvironmentVariableIsSet("XTEST_PROG")) {
        const QString prog = qEnvironmentVariable("XTEST_PROG");
        QStringList a;
        for (const QByteArray &t : qEnvironmentVariable("XTEST_ARGV").toUtf8().split(' '))
            if (!t.isEmpty())
                a << QString::fromUtf8(t);
        QFile o1(QStringLiteral("D:/Qt/QtProject/Xterm/build/sshdump.bin"));
        o1.open(QIODevice::WriteOnly | QIODevice::Truncate);
        ConPty p;
        long long n = 0;
        QObject::connect(&p, &ConPty::outputReceived, [&](const QByteArray &c) {
            n += c.size();
            o1.write(c);
        });
        QObject::connect(&p, &ConPty::launched, [&](const QString &err) {
            printf("launched err='%s'\n", qPrintable(err));
            fflush(stdout);
        });
        QObject::connect(&p, &ConPty::finished, [](int code, bool byUs) {
            printf("finished code=%d byUs=%d\n", code, int(byUs));
            fflush(stdout);
            QCoreApplication::quit();
        });
        p.start(prog, a, 80, 24);
        if (qEnvironmentVariableIsSet("XTEST_RESIZE")) {
            // 尺寸真的变了必须通知伪终端：mode CON 会打印当前行列，据此核对
            const QStringList wh = qEnvironmentVariable("XTEST_RESIZE")
                                       .split(QLatin1Char('x'));
            // 先取成整数再按值捕获：singleShot 的 lambda 在 1200ms 后才跑，
            // 那时 if 块早已退出，按引用捕 wh 就是悬垂引用（实测崩成 bad_alloc）。
            const int w = wh.value(0).toInt();
            const int h = wh.value(1).toInt();
            QTimer::singleShot(1200, [&p, w, h] { p.resize(w, h); });
        }
        QTimer::singleShot(1500, [&] {
            printf("[send] %s\n", qPrintable(a.isEmpty() ? QString() : QStringLiteral("input probe")));
            p.write(QByteArray("mode CON\r"));
            p.write(QByteArray("echo XTERM_INPUT_OK\r"));
        });
        QTimer::singleShot(4000, [&] { printf("total=%lld\n", n); p.stop(); });
        return app.exec();
    }

    const QString cfgPath = argc > 1
        ? QString::fromLocal8Bit(argv[1])
        : QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
              .filePath(QStringLiteral("config.json"));
    const QJsonObject o = readSshBlock(cfgPath);
    if (o.isEmpty()) {
        printf("open config failed: %s\n", qPrintable(cfgPath));
        return 1;
    }

    SshTarget t;
    t.host = o.value(QStringLiteral("host")).toString();
    t.port = o.value(QStringLiteral("port")).toInt(22);
    t.user = o.value(QStringLiteral("user")).toString();
    t.password = o.value(QStringLiteral("password")).toString();
    t.connectTimeoutSec = o.value(QStringLiteral("connectTimeoutSec")).toInt(10);
    if (qEnvironmentVariableIsSet("XTEST_NO_PW"))
        t.password.clear();

    QStringList args;
    if (!qEnvironmentVariableIsSet("XTEST_NOTT"))
        args << QStringLiteral("-tt");
    args << QStringLiteral("-p") << QString::number(t.port);
    args << QStringLiteral("-o")
         << QStringLiteral("ConnectTimeout=%1").arg(t.connectTimeoutSec);
    if (qEnvironmentVariableIsSet("XTEST_FULL_ARGS")) {
        // 与 SshSession::buildArguments() 对齐，用于对比选项差异对首屏时延的影响
        args << QStringLiteral("-o") << QStringLiteral("ServerAliveInterval=30");
        args << QStringLiteral("-o") << QStringLiteral("ServerAliveCountMax=3");
        args << QStringLiteral("-o") << QStringLiteral("TCPKeepAlive=yes");
    }
    QString dest = t.host;
    if (!t.user.isEmpty())
        dest = t.user + QLatin1Char('@') + t.host;
    args << dest;

    printf("program=%s\nargs=%s\npw_len=%d\n",
           qPrintable(SshSession::sshExecutable()),
           qPrintable(args.join(QLatin1Char(' '))), int(t.password.size()));
    fflush(stdout);

    QFile out(QStringLiteral("D:/Qt/QtProject/Xterm/build/sshdump.bin"));
    out.open(QIODevice::WriteOnly | QIODevice::Truncate);
    QFile tx(QStringLiteral("D:/Qt/QtProject/Xterm/build/sshtx.bin"));
    tx.open(QIODevice::WriteOnly | QIODevice::Truncate);

    QByteArray tail;         // 最近输出，用于找提示
    QElapsedTimer clock;
    clock.start();
    long long total = 0;
    int pwSent = 0;
    int cmdSent = 0;

    ConPty pty;
    QObject::connect(&pty, &ConPty::launched, [](const QString &err) {
        printf("launched err='%s'\n", qPrintable(err));
        fflush(stdout);
    });
    QObject::connect(&pty, &ConPty::finished, [](int code, bool byUs) {
        printf("finished code=%d byUs=%d\n", code, int(byUs));
        fflush(stdout);
        QCoreApplication::quit();
    });
    QObject::connect(&pty, &ConPty::outputReceived, [&](const QByteArray &chunk) {
        out.write(chunk);
        total += chunk.size();
        tail += chunk;
        if (tail.size() > 400)
            tail.remove(0, tail.size() - 400);
        // 只打印可读文本，转义序列压掉，方便看刷屏节奏
        QString plain;
        for (const QByteArray &line : chunk.split('\n'))
            for (char c : line)
                if (c >= 32 && c < 127) plain += QChar(c);
        if (!plain.isEmpty())
            printf("[%6.2fs] %s\n", clock.elapsed() / 1000.0,
                   plain.left(100).toUtf8().constData());
    });

    if (!pty.start(SshSession::sshExecutable(), args, 80, 24)) {
        printf("start failed\n");
        return 1;
    }

    // 每 200ms 看一次提示：出现 password 就代填密码；出现 shell 提示符就发命令
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        const QString low = QString::fromUtf8(tail).toLower();
        if (!qEnvironmentVariableIsSet("XTEST_NO_PW") && low.contains(QStringLiteral("password:"))) {
            if (pwSent < 3) {
                const QByteArray p = t.password.toUtf8() + "\r";
                tx.write(p);
                pty.write(p);
                tail.clear();
                printf("[send] password #%d\n", ++pwSent);
                fflush(stdout);
            }
            return;
        }
        if (low.contains(QStringLiteral("(yes/no"))) {
            tx.write("yes\r");
            pty.write("yes\r");
            tail.clear();
            printf("[send] yes\n");
            fflush(stdout);
            return;
        }
        if (low.contains(QLatin1Char('#')) || low.contains(QLatin1Char('$'))) {
            if (cmdSent < 1) {
                ++cmdSent;
                const QByteArray c = "id; echo XTERM_MARKER_DONE\r";
                tx.write(c);
                pty.write(c);
                tail.clear();
                printf("[send] command\n");
                fflush(stdout);
            }
        }
    });
    poll.start(200);

    QTimer::singleShot(5000, [&] {
        const QByteArray c = "id; echo XTERM_MARKER_DONE\r";
        tx.write(c);
        pty.write(c);
        printf("[send@5s] id + marker\n");
        fflush(stdout);
    });
    QTimer::singleShot(20000, [&] {
        printf("收尾: 总输出 %lld 字节, 密码发送 %d 次, 命令发送 %d 次\n",
               total, pwSent, cmdSent);
        fflush(stdout);
        pty.stop();
    });
    return app.exec();
}
