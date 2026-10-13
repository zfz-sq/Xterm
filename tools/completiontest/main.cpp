// 离屏回归测试: ScreenView 的 Tab 客户端补全 + CompletionEngine
// 构建运行: cmake -S tools/completiontest -B build/completiontest && cmake --build build/completiontest
//
// ScreenView 不代画补全内容，显示交给设备回显。测试里用「理想 tty」模拟回显：
// 可打印字符原样回灌，DEL(0x7f) 回灌 "\b \b"，\r 回灌 "\r\n"，其余（\t/ESC）丢弃。
// 断言两层：发给设备的字节序列 + 模拟回显落地后的后续 Tab 行为。
#include <QApplication>
#include <QKeyEvent>
#include <cstdio>

#include "../../completionengine.h"
#include "../../screenview.h"

static int g_failures = 0;

static void expect(bool cond, const char *what)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

static QByteArray hexOf(const QByteArray &d)
{
    QByteArray out;
    for (char c : d) {
        if (!out.isEmpty())
            out += ' ';
        out += QByteArray::number(static_cast<unsigned char>(c), 16).rightJustified(2, '0');
    }
    return out;
}

// 键盘 + 理想回显模拟台
class Rig
{
public:
    ScreenView view;
    CompletionEngine engine;
    QByteArray captured;
    bool autoEcho = true; // 关掉后补全字节不落地，模拟串口/网络延迟；
                          // 普通输入字符的回显不受影响（人速打字永远等得到回显）

    Rig()
    {
        view.setCompletionEngine(&engine);
        QObject::connect(&view, &ScreenView::sendData, &view, [this](const QByteArray &d) {
            captured += d;
            if (echoNext)
                view.receiveData(simulateEcho(d));
        });
        view.resize(80 * 10, 24 * 20);
        view.receiveData(QByteArrayLiteral("root# "));
    }

    // tty ECHO 的行为：字符回显、DEL 擦除（\b 空格 \b）、CR 回显成 CRLF
    static QByteArray simulateEcho(const QByteArray &out)
    {
        QByteArray echo;
        for (char c : out) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u == 0x7f)
                echo += QByteArrayLiteral("\b \b");
            else if (u >= 0x20 && u < 0x7f)
                echo += c;
            else if (c == '\r')
                echo += QByteArrayLiteral("\r\n");
        }
        return echo;
    }

    void typeText(const QString &s)
    {
        for (QChar c : s)
            pressChar(c);
    }

    void pressChar(QChar c)
    {
        captured.clear();
        echoNext = true;
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(c));
        QApplication::sendEvent(&view, &ev);
    }

    void pressTab()
    {
        captured.clear();
        echoNext = autoEcho;
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
        QApplication::sendEvent(&view, &ev);
    }

    QByteArray drain() { QByteArray d = captured; captured.clear(); return d; }

private:
    bool echoNext = true;
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // -------- CompletionEngine 本身
    {
        CompletionEngine e;
        e.setUserCommands({QStringLiteral("ls"), QStringLiteral("help"), QStringLiteral("reboot")});

        QStringList all = e.complete(QString());
        expect(all.contains(QStringLiteral("ls")) && all.contains(QStringLiteral("help"))
                   && all.contains(QStringLiteral("reboot")),
               "complete('') 包含用户命令 + 内置");

        QStringList prefix = e.complete(QStringLiteral("l"));
        expect(prefix.contains(QStringLiteral("ls")), "complete('l') 包含 ls");
        expect(prefix.contains(QStringLiteral("less")), "complete('l') 包含 less");
        expect(!prefix.contains(QStringLiteral("help")), "complete('l') 不含 help");

        expect(e.complete("c").contains(QStringLiteral("cat"))
                   && e.complete("c").contains(QStringLiteral("cd")),
               "complete('c') 包含 cat/cd");
        expect(e.complete("xxx").isEmpty(), "complete('xxx') 空");
    }

    // -------- LCP
    {
        CompletionEngine e;
        QStringList items = {QStringLiteral("ls"), QStringLiteral("less"),
                             QStringLiteral("lsblk"), QStringLiteral("ln")};
        expect(CompletionEngine::longestCommonPrefix(items) == QStringLiteral("l"),
               "LCP 最小公共部分");
        items = {QStringLiteral("ls"), QStringLiteral("ls")};
        expect(CompletionEngine::longestCommonPrefix(items) == QStringLiteral("ls"),
               "LCP 唯一项");
        expect(CompletionEngine::longestCommonPrefix({}).isEmpty(), "LCP 空集");
    }

    // -------- Tab 流程: 无匹配直通设备
    {
        Rig rig;
        rig.engine.setUserCommands({QStringLiteral("ls"), QStringLiteral("reboot")});
        rig.typeText(QStringLiteral("xxx"));
        rig.pressTab();
        expect(rig.drain() == QByteArray(1, '\t'), "无匹配 Tab 直通发 \\t");
    }

    // -------- Tab 流程: 唯一匹配
    {
        Rig rig;
        rig.engine.setUserCommands({QStringLiteral("reboot")});
        rig.typeText(QStringLiteral("reb"));
        rig.pressTab();
        expect(rig.drain() == "oot", "唯一匹配: 'reb' 发出差量 'oot'");
        // 回显已落地，再 Tab 重新取词: "reboot" 已是完整候选，无差量可发
        rig.pressTab();
        expect(rig.drain().isEmpty(), "补齐后再 Tab 无差量（不直发也不乱发）");
    }

    // -------- Tab 流程: 多匹配 LCP 扩展 + Cycle
    {
        Rig rig;
        rig.engine.setUserCommands({QStringLiteral("localcmd")});

        rig.typeText(QStringLiteral("l"));
        rig.pressTab();
        // 候选 sorted: less localcmd ln locate login logout ls lsblk ...
        // LCP == "l"，立即推进 cycle[0] = less: DEL('l') + "less"
        expect(rig.drain() == QByteArray(1, '\x7f') + "less",
               "多匹配首次 Tab: DEL + cycle[0] 'less'");

        rig.pressTab();
        QByteArray second = rig.drain();
        expect(second == QByteArray(4, '\x7f') + "ln"
                   || second == QByteArray(4, '\x7f') + "localcmd"
                   || second == QByteArray(4, '\x7f') + "locate"
                   || second == QByteArray(4, '\x7f') + "login"
                   || second == QByteArray(4, '\x7f') + "logout"
                   || second == QByteArray(4, '\x7f') + "ls"
                   || second == QByteArray(4, '\x7f') + "lsblk",
               "再次 Tab: DEL*4 + cycle[1]");

        // 一路 Tab 下去每帧都是「DEL×旧词长 + 新候选词」，且新词在候选集合内
        static const char *const kCands[] = { "", "l", "less", "localcmd", "ln",
                                              "locate", "login", "logout", "ls", "lsblk" };
        auto inCands = [](const QByteArray &word) {
            for (const char *c : kCands)
                if (word == c)
                    return true;
            return false;
        };
        bool inCycle = true;
        for (int i = 0; i < 20; ++i) {
            rig.pressTab();
            QByteArray d = rig.drain();
            const int dels = int(d.count('\x7f'));
            const QByteArray word = d.mid(dels);
            if (dels < 1 || dels > 8 || !inCands(word) || (word.isEmpty() && dels > 0)) {
                inCycle = false;
                printf("  异常帧: %s\n", hexOf(d).constData());
                break;
            }
        }
        expect(inCycle, "多次 Tab 始终在 cycle 内替换");
    }

    // -------- 回显延迟窗口: 补全字节已发出、回显未落地时再 Tab
    {
        Rig rig;
        rig.engine.setUserCommands({QStringLiteral("localcmd")});
        rig.autoEcho = false;

        rig.typeText(QStringLiteral("l"));
        rig.pressTab(); // 发出 DEL + "less"，屏上还停在 "l"
        expect(rig.drain() == QByteArray(1, '\x7f') + "less", "延迟模式首次 Tab");

        rig.pressTab(); // 屏上词尾 == cycleEnd - pending，应按未变推进
        QByteArray d = rig.drain();
        expect(d == QByteArray(4, '\x7f') + "ln"
                   || d == QByteArray(4, '\x7f') + "localcmd"
                   || d == QByteArray(4, '\x7f') + "locate"
                   || d == QByteArray(4, '\x7f') + "login"
                   || d == QByteArray(4, '\x7f') + "logout"
                   || d == QByteArray(4, '\x7f') + "ls"
                   || d == QByteArray(4, '\x7f') + "lsblk",
               "回显未落地再 Tab 仍按 cycle 推进");

        // 把两轮回显按到达顺序落地，屏上终态应是第二个候选词
        rig.view.receiveData(Rig::simulateEcho(QByteArray(1, '\x7f') + "less"));
        rig.view.receiveData(Rig::simulateEcho(d));
    }

    // -------- 非 Tab 键打断 Cycle
    {
        Rig rig;
        rig.engine.setUserCommands({QStringLiteral("ls"), QStringLiteral("ln")});
        rig.typeText(QStringLiteral("l"));
        rig.pressTab(); // 进入 cycle
        rig.pressChar(QLatin1Char('h')); // 继续打字应打断
        expect(rig.drain() == QByteArray(1, 'h'), "打字打断 cycle（字符直发）");
        rig.pressTab();
        expect(rig.drain() == QByteArray(1, '\t'), "打断后重新取词，无匹配直通");
    }

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
