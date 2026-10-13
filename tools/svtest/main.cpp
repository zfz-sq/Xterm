// 离屏端到端验证: ScreenView + 真实 SshSession / SerialSession。
// 键盘走真实 QKeyEvent，断言走 ScreenView::screenText() 文本，不依赖像素。
//
// 用法:
//   svtest <config.json> ssh        # SSH 模式(取 lastProfile): 登录→top→刷新→q
//   svtest <config.json> serial COM4 [115200]
//
// 构建: cmake -S tools/svtest -B build/svtest && cmake --build build/svtest
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QThread>
#include <cstdio>
#include <functional>

#include "../../screenview.h"
#include "../../serialsession.h"
#include "../../sshsession.h"
#include "../../sshtarget.h"
#include "../../completionengine.h"

static int g_failures = 0;

static void expect(bool cond, const char *what)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!cond)
        ++g_failures;
}

// 跑事件循环直到条件满足或超时（毫秒）
static bool runUntil(const std::function<bool()> &cond, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return true;
}

static void key(ScreenView &view, int key, const QString &text = QString(),
                Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent ev(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(&view, &ev);
}

static void typeLine(ScreenView &view, const char *s)
{
    const QString text = QString::fromUtf8(s);
    for (const QChar &c : text)
        key(view, Qt::Key_unknown, QString(c));
    key(view, Qt::Key_Return, QStringLiteral("\r"));
}

static void dumpScreen(ScreenView &view, const char *tag)
{
    printf("---- %s ----\n%s\n------------\n", tag, qPrintable(view.screenText()));
    fflush(stdout);
}

struct ProfileData
{
    SshTarget ssh;
    QString serialPort;
    int serialBaud = 115200;
};

static ProfileData loadProfile(const QString &configPath)
{
    ProfileData d;
    QFile f(configPath);
    if (!f.open(QIODevice::ReadOnly)) {
        printf("open config failed: %s\n", qPrintable(f.fileName()));
        return d;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QString lastName = doc.object().value(QStringLiteral("lastProfile")).toString();
    const QJsonArray profiles = doc.object().value(QStringLiteral("profiles")).toArray();
    QJsonObject prof;
    for (const QJsonValue &v : profiles) {
        if (v.toObject().value(QStringLiteral("name")).toString() == lastName) {
            prof = v.toObject();
            break;
        }
    }
    if (prof.isEmpty() && !profiles.isEmpty())
        prof = profiles.first().toObject();

    const QJsonObject ssh = prof.value(QStringLiteral("ssh")).toObject();
    d.ssh.host = ssh.value(QStringLiteral("host")).toString();
    d.ssh.port = ssh.value(QStringLiteral("port")).toInt(22);
    d.ssh.user = ssh.value(QStringLiteral("user")).toString();
    d.ssh.password = ssh.value(QStringLiteral("password")).toString();
    const QJsonObject serial = prof.value(QStringLiteral("serial")).toObject();
    d.serialPort = serial.value(QStringLiteral("port")).toString();
    d.serialBaud = serial.value(QStringLiteral("baudRate")).toInt(115200);
    return d;
}

static int runTopFlow(ScreenView &view, const QString &banner)
{
    printf("=== %s: top 流程 ===\n", qPrintable(banner));
    dumpScreen(view, "before top");
    typeLine(view, "top");
    const bool topOk = runUntil([&view] {
        const QString s = view.screenText();
        return s.contains(QStringLiteral("Mem:")) && s.contains(QStringLiteral("PID"));
    }, 12000);
    if (!topOk) {
        dumpScreen(view, "top never came");
        expect(false, "top 启动(Mem: 行)");
        return 0;
    }
    expect(view.screenText().contains(QStringLiteral("CPU:")), "top 启动(CPU: 行)");
    expect(view.screenText().contains(QStringLiteral("PID")), "top 启动(进程表)");
    // resize 链路: pty 尺寸跟随视口(100 列)，长命令行不应还在 80 列截断
    if (banner == QStringLiteral("SSH")) {
        bool wide = false;
        const QStringList rows = view.screenText().split(QLatin1Char('\n'));
        for (const QString &r : rows) {
            if (r.contains(QStringLiteral("wpa_supplicant"))) {
                printf("  wpa row len=%d: %s\n", r.size(), qPrintable(r));
                if (r.size() > 80)
                    wide = true;
            }
        }
        expect(wide, "pty 列宽跟随视口(resize 链路)");
    }

    // 等下一轮刷新(BusyBox top 5s 一轮),确认持续刷新没有把屏幕刷坏
    const QString round1 = view.screenText();
    runUntil([&view, round1] { return view.screenText() != round1; }, 9000);
    expect(view.screenText().contains(QStringLiteral("Mem:")), "第二轮刷新仍完整");

    key(view, Qt::Key_Q, QStringLiteral("q"));
    runUntil([&view] { return view.screenText().contains(QLatin1Char('#')); }, 5000);
    expect(view.screenText().contains(QLatin1Char('#')), "q 退出回到 shell 提示符");
    dumpScreen(view, "after quit");
    return 0;
}

static int runSsh(const QString &configPath)
{
    const ProfileData d = loadProfile(configPath);
    printf("ssh target: %s@%s:%d\n", qPrintable(d.ssh.user), qPrintable(d.ssh.host), d.ssh.port);

    ScreenView view;
    SshSession session;
    // 先接线、后 show：尺寸信号发出时连接必须在，否则信号丢失后
    // m_sentCols 缓存会让它永远不再发（隐藏 widget 的 resize 也不发事件）
    QObject::connect(&view, &ScreenView::sendData, &session, [&session](const QByteArray &bytes) {
        session.send(bytes);
    });
    QObject::connect(&session, &SshSession::received, &view, &ScreenView::receiveData);
    QObject::connect(&view, &ScreenView::terminalSizeChanged, &session,
                     [&session](int cols, int rows) {
                         printf("  terminalSizeChanged(%d, %d)\n", cols, rows);
                         fflush(stdout);
                         session.setPtySize(cols, rows);
                     });
    view.resize(160 * 15, 40 * 18); // offscreen 默认字体较宽，窗口给足列数
    view.show();
    QApplication::processEvents(); // 让 resize 事件与字体度量先落地
    QThread::msleep(60);           // 跳过 40ms 尺寸去抖窗
    QApplication::processEvents();
    printf("  view grid rows=%d\n", view.screenText().split(QLatin1Char('\n')).size());

    const QString openErr = session.open(d.ssh);
    expect(openErr.isEmpty(), "SSH 会话开始建立");
    const bool promptOk = runUntil(
        [&view] { return view.screenText().contains(QLatin1Char('#'))
                         || view.screenText().contains(QLatin1Char('$')); }, 25000);
    dumpScreen(view, "after login");
    expect(promptOk, "SSH 登录到 shell 提示符");

    int rc = 0;
    if (promptOk) {
        // 补全烟囱测试: 输入 echo 前缀由键盘真实路径发出
        typeLine(view, "echo SVTEST_OK");
        const bool echoOk = runUntil(
            [&view] { return view.screenText().contains(QStringLiteral("SVTEST_OK")); }, 8000);
        expect(echoOk, "命令执行 + 输出回显");
        rc = runTopFlow(view, QStringLiteral("SSH"));
    }
    session.close();
    return rc;
}

static int runSerial(const QString &configPath, const QString &port, int baud)
{
    ProfileData d = loadProfile(configPath);
    if (!port.isEmpty())
        d.serialPort = port;
    if (baud > 0)
        d.serialBaud = baud;
    printf("serial: %s @ %d\n", qPrintable(d.serialPort), d.serialBaud);
    if (d.serialPort.isEmpty()) {
        printf("no serial port configured\n");
        return 2;
    }

    ScreenView view;
    view.resize(160 * 15, 40 * 18);
    view.show();
    QApplication::processEvents();
    SerialSession serial;
    QObject::connect(&view, &ScreenView::sendData, &serial, [&serial](const QByteArray &bytes) {
        serial.send(bytes);
    });
    QObject::connect(&serial, &SerialSession::received, &view, &ScreenView::receiveData);

    const QString err = serial.open(d.serialPort, d.serialBaud,
                                    QSerialPort::Data8, QSerialPort::NoParity,
                                    QSerialPort::OneStop);
    expect(err.isEmpty(), "串口打开成功");
    if (!err.isEmpty()) {
        printf("open error: %s\n", qPrintable(err));
        return 2;
    }

    // 唤醒: 串口另一端可能是 getty(login:) 也可能是现成 shell
    key(view, Qt::Key_Return, QStringLiteral("\r"));
    runUntil([] { return false; }, 1500);
    dumpScreen(view, "serial after enter");

    // 自适应: login 提示时输入 root，Password 提示时输入配置里的密码，
    // 已是 shell(#/$)就直接跑
    QString s = view.screenText();
    if (!s.contains(QLatin1Char('#')) && !s.contains(QLatin1Char('$'))) {
        if (s.contains(QStringLiteral("login:"), Qt::CaseInsensitive)) {
            typeLine(view, "root");
            runUntil([] { return false; }, 1500);
            dumpScreen(view, "serial after user");
            s = view.screenText();
        }
        if (s.contains(QStringLiteral("Password"), Qt::CaseInsensitive)
            && !d.ssh.password.isEmpty()) {
            typeLine(view, d.ssh.password.toUtf8().constData());
            runUntil([] { return false; }, 2000);
            dumpScreen(view, "serial after password");
        }
    }

    const bool promptOk = runUntil(
        [&view] { return view.screenText().contains(QLatin1Char('#'))
                         || view.screenText().contains(QLatin1Char('$')); }, 6000);
    expect(promptOk, "串口到达 shell 提示符");

    int rc = 0;
    if (promptOk) {
        // Tab 客户端补全端到端: 'ec' + Tab → 'echo'（引擎补全差量 'ho'，
        // 由 ash 回显落地；板子若自己也有补全殊途同归）
        CompletionEngine engine;
        view.setCompletionEngine(&engine);
        key(view, Qt::Key_unknown, QStringLiteral("e"));
        key(view, Qt::Key_unknown, QStringLiteral("c"));
        runUntil([] { return false; }, 600);
        key(view, Qt::Key_Tab, QStringLiteral("\t"));
        const bool tabOk = runUntil(
            [&view] { return view.screenText().contains(QStringLiteral("echo")); }, 6000);
        expect(tabOk, "串口 Tab 补全 'ec'→'echo'");
        // 收尾清行: DEL×4 回到空提示行
        for (int i = 0; i < 4; ++i)
            key(view, Qt::Key_Backspace, QStringLiteral("\x7f"));
        runUntil([] { return false; }, 800);

        // Backspace 端到端: 打 'xy' 再退一格，屏上应只剩 'x'
        key(view, Qt::Key_unknown, QStringLiteral("x"));
        key(view, Qt::Key_unknown, QStringLiteral("y"));
        const bool xyOk = runUntil(
            [&view] { return view.screenText().contains(QStringLiteral("xy")); }, 5000);
        expect(xyOk, "串口输入回显 'xy'");
        key(view, Qt::Key_Backspace, QStringLiteral("\x7f"));
        runUntil([] { return false; }, 800);
        expect(view.screenText().contains(QStringLiteral("x"))
                   && !view.screenText().contains(QStringLiteral("xy")),
               "Backspace 删除末字符");
        key(view, Qt::Key_Backspace, QStringLiteral("\x7f"));
        runUntil([] { return false; }, 500);
        rc = runTopFlow(view, QStringLiteral("Serial"));
    }
    serial.close();
    return rc;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const QString configPath = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                        : app.applicationDirPath() + "/config.json";
    const QString mode = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("ssh");

    int rc = 0;
    if (mode == QStringLiteral("serial"))
        rc = runSerial(configPath,
                       argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString(),
                       argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : 0);
    else
        rc = runSsh(configPath);

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : rc;
}
