// 离屏测试: CommandBar 快捷按钮点击 → commandRequested 信号链路
#include <QApplication>
#include <QMetaObject>
#include <QPushButton>
#include <cstdio>

#include "../../commandbar.h"
#include "../../commandstore.h"

static int g_failures = 0;
static void expect(bool cond, const char *what)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    CommandBar bar;
    QList<CommandItem> cmds = { CommandItem(QStringLiteral("ls -l")),
                                CommandItem(QStringLiteral("help")) };
    bar.setCommands(cmds);

    // 收集信号
    QString got;
    QObject::connect(&bar, &CommandBar::commandRequested, &app,
                     [&](const QString &t) { got = t; });

    // 找到按钮列里的快捷按钮（按钮列在滚动区内）
    const auto buttons = bar.findChildren<QPushButton *>();
    printf("buttons found: %d\n", int(buttons.size()));
    expect(buttons.size() == 4, "2 个快捷按钮 + 2 个行内发送按钮");

    if (!buttons.isEmpty()) {
        printf("button[0] text=%s enabled=%d\n",
               buttons.at(0)->text().toUtf8().constData(),
               buttons.at(0)->isEnabled());

        // 未连接(默认 setSendEnabled(false))时按钮仍可点击 —— 点击由上层提示
        expect(buttons.at(0)->isEnabled(), "未连接时快捷按钮仍可点击(由上层提示)");

        // 打开发送开关后点击
        bar.setSendEnabled(true);
        got.clear();
        QMetaObject::invokeMethod(const_cast<QPushButton *>(buttons.at(0)), "clicked");
        app.processEvents();
        expect(got == QStringLiteral("ls -l"), "点击快捷按钮发出 commandRequested(\"ls -l\")");
    }

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
