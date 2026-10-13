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

    // clear 在各家 terminfo/busybox 里写法不同，逐一验证（现场只见到"按了没反应"，
    // 得先确定是哪一种形式没落地）。
    { // linux/vt100 terminfo: clear = ESC[H ESC[J（ED mode 0，从光标擦到末尾）
        TerminalView v;
        v.receiveData("line1\r\nline2\r\nline3");
        v.receiveData("\x1b[H\x1b[Jnew");
        expect(v.toPlainText() == QStringLiteral("new"), "clear 变体 ED(0) 从光标全屏擦除");
    }

    { // ncurses E3 前缀: ESC[3J ESC[H ESC[2J（3J 是清回滚区，不能把它当非法序列丢掉）
        TerminalView v;
        v.receiveData("old\r\nprompt # ");
        v.receiveData("\x1b[3J\x1b[H\x1b[2Jprompt # ");
        expect(v.toPlainText() == QStringLiteral("prompt # "), "clear 变体 E3+ED(2)");
    }

    { // ED(1) 擦除文档头到光标（补全 ED 家族，别静默丢掉）
        TerminalView v;
        v.receiveData("junk\r\nkeep me");
        v.receiveData("\x1b[2;1H\x1b[1J");
        expect(v.toPlainText() == QStringLiteral("keep me"), "ED(1) 擦除光标以上");
    }

    { // RIS（ESC c）：BusyBox clear 认定 xterm 时、以及 reset 都用它，语义是整屏复位
        TerminalView v;
        v.receiveData("old text\r\nmore");
        v.receiveData("\x1b" "cfresh");
        expect(v.toPlainText() == QStringLiteral("fresh"), "RIS ESC c 清屏并复位");
    }

    { // 备用屏切换序列本身必须被吞掉，不能漏成可见乱码（备用屏语义暂不实现）
        TerminalView v;
        v.receiveData("before");
        v.receiveData("\x1b[?1049hafter");
        expect(!v.toPlainText().contains(QStringLiteral("1049")),
               "CSI ?1049h 不吃成可见乱码");
    }

    { // conhost 对 cls 的真实重绘字节（build/sshdump.bin 抓的，逐行 EL+CR+LF，不擦屏）。
      // LF 若按「插入一行」实现，就会把 24 个空行插进文档中间，旧内容被顶到下面。
      TerminalView v;
      v.receiveData("\x1b[?9001h\x1b[?1004h\x1b[?25l\x1b[2J\x1b[m\x1b[H"
                    "MARK_OLD_LINE\r\n\x1b]0;C:\\WINDOWS\\SYSTEM32\\cmd.exe\x07"
                    "\x1b[?25h\x1b[?25l"
                    "\x1b[H"
                    "\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n"
                    "\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n"
                    "\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n"
                    "\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n"
                    "\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n"
                    "\x1b[4;1H\x1b[3J\x1b[?25l\x1b[HAFTER_CLS\r\n\x1b[?25h");
      expect(v.toPlainText() == QStringLiteral("AFTER_CLS\n"),
             "conhost 整屏重绘后只剩新内容，不留空行墙");
    }

    { // 单独盯 LF 的语义：下面还有行就是「下移」，不能把行拆开
        TerminalView v;
        v.receiveData("AAA\r\nBBB\r\nCCC");
        v.receiveData("\x1b[H\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n"); // 三行原地擦除
        v.receiveData("\x1b[HNEW");
        // 第三组 EL+LF 时已在文档末行，只能新起一行（那是屏幕的空行，不是插入的墙）。
        // 修复前每组 LF 都往文档里多插一个空块，旧内容被整体顶下去。
        expect(v.toPlainText() == QStringLiteral("NEW\n\n\n"),
               "LF 在已有行之间下移而非插入空行");
    }

    { // 同一串字节按 7 字节一片投递：真实到达会被任意位置切断，
      // 转义序列跨片也必须得到同样的结果
        static const char kStream[] =
            "\x1b[2J\x1b[HAAA\r\nBBB\r\nCCC"
            "\x1b[H\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n\x1b[K\r\n"
            "\x1b[4;1H\x1b[3J\x1b[HNEW\r\n";
        TerminalView v;
        const QByteArray raw(kStream);
        for (int i = 0; i < raw.size(); i += 7)
            v.receiveData(raw.mid(i, 7));
        expect(v.toPlainText() == QStringLiteral("NEW\n"),
               "跨包投递的同一段重绘结果与整包一致");
    }

    printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
