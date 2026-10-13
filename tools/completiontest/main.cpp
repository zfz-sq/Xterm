// 离屏回归测试: TerminalView 的 Tab 客户端补全 + CompletionEngine
// 构建运行: cmake -S tools/completiontest -B build/completiontest && cmake --build build/completiontest
#include <QApplication>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <cstdio>

#include "../../completionengine.h"
#include "../../terminalview.h"

static int g_failures = 0;

static void expect(bool cond, const char *what)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

// 用 sendEvent 把真实 QKeyEvent 投递给 TerminalView（模拟键盘）
static void typeChar(TerminalView &v, QChar c)
{
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, QString(c));
    QApplication::sendEvent(&v, &ev);
}

// 把 Tab 投递给 TerminalView 的真 keyPressEvent
static void pressTabKey(TerminalView &v)
{
    QKeyEvent ev(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t"));
    QApplication::sendEvent(&v, &ev);
}

static void typeChars(TerminalView &v, const QString &s)
{
    for (QChar c : s)
        typeChar(v, c);
}

static QString lineAtCursor(TerminalView &v)
{
    // 客户端补全按当前 m_cursor.block() 的最后一行进行，
    // 因此以行内容末尾作为比较。
    QTextCursor c = v.textCursor();
    c.movePosition(QTextCursor::End);
    return c.block().text();
}

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
        // 应包含 ls / less / locate / ln / login / lsblk ...
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

    // -------- TerminalView Tab 流程: 无匹配
    {
        TerminalView v;
        v.setLocalEcho(true);
        CompletionEngine e;
        e.setUserCommands({QStringLiteral("ls"), QStringLiteral("reboot")});
        v.setCompletionEngine(&e);

        // 模拟输入 "xxx"
        typeChar(v, QChar('x'));
        typeChar(v, QChar('x'));
        typeChar(v, QChar('x'));

        pressTabKey(v);
        expect(lineAtCursor(v) == QStringLiteral("xxx"), "无匹配 Tab 不改动输入");
    }

    // -------- TerminalView Tab 流程: 唯一匹配
    {
        TerminalView v;
        v.setLocalEcho(true);
        CompletionEngine e;
        // 构造一个只有 reboot 能匹配 "reb" 的场景
        QStringList user = {QStringLiteral("reboot")};
        e.setUserCommands(user);
        v.setCompletionEngine(&e);

        typeChar(v, QChar('r'));
        typeChar(v, QChar('e'));
        typeChar(v, QChar('b'));

        pressTabKey(v);
        expect(lineAtCursor(v) == QStringLiteral("reboot"),
               "唯一匹配: 'reb' → 'reboot'");
    }

    // -------- TerminalView Tab 流程: 多匹配 LCP 扩展
    {
        TerminalView v;
        v.setLocalEcho(true);
        CompletionEngine e;
        // 只放一个会冲突的用户命令 + 让内置来填剩余
        e.setUserCommands({QStringLiteral("localcmd")});
        v.setCompletionEngine(&e);

        // 输入 l  -> 内置里以 l 开头的候选 (less / ln / locate / login / ls / lsblk ...)
        // LCP = "l", 等于 prefix, 推进到 cycle[0] = "less"
        typeChar(v, QChar('l'));
        pressTabKey(v);
        QString line = lineAtCursor(v);
        expect(line == QStringLiteral("less"),
               "多匹配首次 Tab 后到 cycle[0] (sorted builtin)");

        // 再按 Tab：cycle 到下一个，应该是 ln / locate / login / ls / lsblk 之一
        pressTabKey(v);
        QString after2 = lineAtCursor(v);
        expect(after2 == QStringLiteral("ln") || after2 == QStringLiteral("locate")
                   || after2 == QStringLiteral("login") || after2 == QStringLiteral("logout")
                   || after2 == QStringLiteral("ls") || after2 == QStringLiteral("lsblk"),
               "再次 Tab 推到 cycle[1]");

        // 再 Tab 多次，行内容应当一直在 cycle 内（不会变回 "l"，会经过所有候选再 wrap 回 "l"）
        for (int i = 0; i < 20; ++i)
            pressTabKey(v);
        QString afterMany = lineAtCursor(v);
        expect(afterMany == QStringLiteral("l")
                   || afterMany == QStringLiteral("less")
                   || afterMany == QStringLiteral("ln") || afterMany == QStringLiteral("locate")
                   || afterMany == QStringLiteral("login") || afterMany == QStringLiteral("logout")
                   || afterMany == QStringLiteral("ls") || afterMany == QStringLiteral("lsblk"),
               "多次 Tab 后行内容在 cycle 内");
    }

    // -------- 退格 / 回车打断 Cycle
    {
        TerminalView v;
        v.setLocalEcho(true);
        CompletionEngine e;
        e.setUserCommands({QStringLiteral("ls"), QStringLiteral("ln")});
        v.setCompletionEngine(&e);

        typeChar(v, QChar('l'));
        pressTabKey(v); // 进入 cycle[0]
        typeChar(v, QChar(' ')); // 空格应触发 resetCompletionCycle

        // 接下来输入 "h" 应当不参与上一轮 cycle
        typeChar(v, QChar('h'));
        expect(lineAtCursor(v).endsWith(QStringLiteral("h")), "非 Tab 键打断 cycle");
    }

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}