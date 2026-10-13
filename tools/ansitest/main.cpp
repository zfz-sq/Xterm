// 离屏回归测试: TerminalView 的 ANSI 转义序列解析
// 构建运行: cmake -S tools/ansitest -B build/ansitest && cmake --build build/ansitest && 运行
#include <QApplication>
#include <QFont>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <cstdio>

#include "../../terminalview.h"

static int g_failures = 0;

static void expect(bool cond, const char *what)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

static QColor fgOfFirstChar(const TerminalView &v)
{
    QTextBlock b = v.document()->firstBlock();
    for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment f = it.fragment();
        if (f.isValid() && f.text().size() > 0 && f.text().at(0).isPrint())
            return f.charFormat().foreground().color();
    }
    return QColor();
}

static const QTextFragment *findFragment(const TerminalView &v, const QString &text,
                                         QTextFragment &out)
{
    for (QTextBlock b = v.document()->begin(); b.isValid(); b = b.next()) {
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (f.isValid() && f.text() == text) {
                out = f;
                return &out;
            }
        }
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    { // SGR 颜色 + reset
        TerminalView v;
        v.receiveData("\x1b[31mR\x1b[0mn\x1b[32mG");
        expect(v.toPlainText() == QStringLiteral("RnG"), "SGR 颜色不残留字符");
        expect(fgOfFirstChar(v) == QColor(0xCD, 0x31, 0x31), "SGR 31 → 红色");
    }

    { // CRLF + EL(0) + 追加
        TerminalView v;
        v.receiveData("second\x1b[K|tail\r\nnext");
        expect(v.toPlainText() == QStringLiteral("second|tail\nnext"),
               "CR/LF + EL(0) 行内擦除");
    }

    { // ED(2) 清屏 + CUP
        TerminalView v;
        v.receiveData("hello world\r\nfoo");
        v.receiveData("\x1b[2J\x1b[Hcleared");
        expect(v.toPlainText() == QStringLiteral("cleared"), "ED(2)+CUP 清屏后重绘");
    }

    { // 退格后覆盖写入: "abc\b \bXY" → "abXY"（设备行编辑场景）
        TerminalView v;
        v.receiveData("abc\b \bXY");
        expect(v.toPlainText() == QStringLiteral("abXY"), "退格+覆盖写入");
    }

    { // CRLF 不产生空行
        TerminalView v;
        v.receiveData("hello\r\nworld");
        expect(v.toPlainText() == QStringLiteral("hello\nworld"), "CRLF 无空行");
    }

    { // \r 进度条覆盖重绘
        TerminalView v;
        v.receiveData("10%\r50%");
        expect(v.toPlainText() == QStringLiteral("50%"), "\\r 覆盖重绘");
    }

    { // OSC 与私有模式被吞掉
        TerminalView v;
        v.receiveData("\x1b]0;title\x07ok\x1b[?25h!");
        expect(v.toPlainText() == QStringLiteral("ok!"), "OSC/私有模式不显示");
    }

    { // 粗体与 256 色之外的 bright 前景
        TerminalView v;
        v.receiveData("\x1b[1;34mB");
        expect(fgOfFirstChar(v) == QColor(0x24, 0x72, 0xC8), "SGR 34 → 蓝色");
    }

    { // 已有文本在主题切换后重映射颜色，粗体保留
        TerminalView v;
        v.appendSystemMessage(QStringLiteral("sys"));
        v.receiveData("\x1b[1;31mR\x1b[0mdef\x1b[92mB\x1b[41mZ");
        v.setLocalEcho(true);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QApplication::sendEvent(&v, &press);

        v.applyTheme(false);
        QTextFragment frag;
        expect(findFragment(v, QStringLiteral("sys"), frag)
                   && frag.charFormat().foreground().color() == QColor(0x0E, 0x63, 0x9C),
               "亮色主题下系统消息跟随切换");
        expect(findFragment(v, QStringLiteral("R"), frag)
                   && frag.charFormat().foreground().color() == QColor(0xC4, 0x2F, 0x2F)
                   && frag.charFormat().fontWeight() == QFont::Bold,
               "亮色主题下旧红色与粗体跟随切换");
        expect(findFragment(v, QStringLiteral("def"), frag)
                   && frag.charFormat().foreground().color() == QColor(0x24, 0x29, 0x2E),
               "亮色主题下旧默认色跟随切换");
        expect(findFragment(v, QStringLiteral("B"), frag)
                   && frag.charFormat().foreground().color() == QColor(0x1F, 0xA8, 0x55),
               "亮色主题下旧亮绿色跟随切换");
        expect(findFragment(v, QStringLiteral("Z"), frag)
                   && frag.charFormat().background().color() == QColor(0xC4, 0x2F, 0x2F),
               "亮色主题下旧背景色跟随切换");
        expect(findFragment(v, QStringLiteral("a"), frag)
                   && frag.charFormat().foreground().color() == QColor(0x0E, 0x7A, 0x6E),
               "亮色主题下旧回显色跟随切换");

        v.applyTheme(true);
        expect(findFragment(v, QStringLiteral("R"), frag)
                   && frag.charFormat().foreground().color() == QColor(0xCD, 0x31, 0x31),
               "切回深色后红色恢复");
        expect(findFragment(v, QStringLiteral("def"), frag)
                   && frag.charFormat().foreground().color() == QColor(0xD4, 0xD4, 0xD4),
               "切回深色后默认色恢复");
        expect(findFragment(v, QStringLiteral("a"), frag)
                   && frag.charFormat().foreground().color() == QColor(0x4E, 0xC9, 0xB0),
               "切回深色后回显色恢复");
        expect(v.toPlainText() == QStringLiteral("sys\nRdefBZa"), "重映射不改文本内容");
    }

    { // 未着色的 ip 输出按 Ubuntu ip -c 补色；已有 ANSI 的不改
        TerminalView v;
        v.receiveData("2: eth0: <UP,LOWER_UP> state UP\r\n"
                      "    link/ether aa:bb:cc:dd:ee:ff brd ff:ff:ff:ff:ff:ff\r\n"
                      "    inet 192.168.1.10/24 brd 192.168.1.255\r\n"
                      "    inet6 fe80::1/64 scope link\r\n"
                      "3: wlan0: state DOWN\r\n"
                      "time 12:30:00 ver 1.2.3\r\n");
        QTextFragment frag;
        auto colored = [&](const char *text, const QColor &c, bool bold) {
            const bool found = findFragment(v, QString::fromUtf8(text), frag);
            return found && frag.charFormat().foreground().color() == c
                   && (frag.charFormat().fontWeight() == QFont::Bold) == bold;
        };
        expect(colored("192.168.1.10/24", QColor(0xBC, 0x3F, 0xBC), true),
               "IPv4 为粗体洋红");
        expect(colored("fe80::1/64", QColor(0x3B, 0x8E, 0xEA), true),
               "IPv6 为粗体亮蓝");
        expect(colored("aa:bb:cc:dd:ee:ff", QColor(0xE5, 0xE5, 0x11), true),
               "MAC 为粗体黄");
        expect(colored("eth0", QColor(0x11, 0xA8, 0xCD), true), "网卡名 eth0 为粗体青");
        expect(colored("wlan0", QColor(0x11, 0xA8, 0xCD), true), "网卡名 wlan0 为粗体青");
        expect(colored("UP", QColor(0x0D, 0xBC, 0x79), true), "UP 为粗体绿");
        expect(colored("LOWER_UP", QColor(0x0D, 0xBC, 0x79), true), "LOWER_UP 为粗体绿");
        expect(colored("DOWN", QColor(0xCD, 0x31, 0x31), true), "DOWN 为粗体红");
        bool plainTime = false;
        for (QTextBlock b = v.document()->begin(); b.isValid(); b = b.next()) {
            if (!b.text().contains(QStringLiteral("12:30:00")))
                continue;
            plainTime = b.begin() != b.end()
                        && b.begin().fragment().charFormat().foreground().color()
                               == QColor(0xD4, 0xD4, 0xD4);
        }
        expect(plainTime, "时刻和点分版本号不上色");

        v.receiveData("\x1b[32m10.9.8.7\x1b[0m");
        expect(findFragment(v, QStringLiteral("10.9.8.7"), frag)
                   && frag.charFormat().foreground().color() == QColor(0x0D, 0xBC, 0x79),
               "设备已上色的地址保持原色");
    }

    { // 地址被串口拆包后仍能补色
        TerminalView v;
        v.receiveData("addr 172.16.");
        v.receiveData("5.4/16;");
        QTextFragment frag;
        expect(findFragment(v, QStringLiteral("172.16.5.4/16"), frag)
                   && frag.charFormat().foreground().color() == QColor(0xBC, 0x3F, 0xBC),
               "跨包 IPv4 补色");
    }

    { // 亮色主题用 iproute2 的浅底配色
        TerminalView v;
        v.applyTheme(false);
        v.receiveData("inet 10.0.0.1 eth0");
        QTextFragment frag;
        expect(findFragment(v, QStringLiteral("10.0.0.1"), frag)
                   && frag.charFormat().foreground().color() == QColor(0xB0, 0x3A, 0xB0)
                   && frag.charFormat().fontWeight() == QFont::Normal,
               "亮色主题 IPv4 为洋红且非粗体");
    }

    { // 256 色与真彩色，参数里的 0 不能把格式清掉
        TerminalView v;
        v.receiveData("\x1b[38;5;196mX\x1b[0m\x1b[38;2;0;255;0mG");
        QTextFragment frag;
        expect(findFragment(v, QStringLiteral("X"), frag)
                   && frag.charFormat().foreground().color() == QColor(255, 0, 0),
               "SGR 38;5;196 → 红色");
        expect(findFragment(v, QStringLiteral("G"), frag)
                   && frag.charFormat().foreground().color() == QColor(0, 255, 0),
               "SGR 38;2;0;255;0 不被 0 重置");
    }

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
