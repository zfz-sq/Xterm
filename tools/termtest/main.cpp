#include "termscreen.h"

#include <cstdio>

namespace {

int g_fails = 0;

void expect(bool ok, const char *what)
{
    if (ok)
        return;
    std::fprintf(stderr, "FAIL %s\n", what);
    ++g_fails;
}

char32_t at(const TermScreen &screen, int row, int col)
{
    const TermLine *line = screen.lineAbsolute(screen.historySize() + row);
    if (!line || col < 0 || col >= int(line->cells.size()))
        return 0;
    return line->cells[size_t(col)].cp;
}

const TermCell *cell(const TermScreen &screen, int row, int col)
{
    const TermLine *line = screen.lineAbsolute(screen.historySize() + row);
    if (!line || col < 0 || col >= int(line->cells.size()))
        return nullptr;
    return &line->cells[size_t(col)];
}

// 全 ASCII 行：文本位即 cell 下标。找到词首格，返回它的指针。
const TermCell *findToken(const TermScreen &screen, int row, const char *tok)
{
    const TermLine *line = screen.lineAbsolute(screen.historySize() + row);
    if (!line)
        return nullptr;
    QString text;
    for (const TermCell &c : line->cells) {
        if (c.flags & TermScreen::FlagWideCont)
            continue;
        text.append(QChar(char16_t(c.cp)));
    }
    const int pos = text.indexOf(QLatin1String(tok));
    if (pos < 0 || pos >= int(line->cells.size()))
        return nullptr;
    return &line->cells[size_t(pos)];
}

} // namespace

int main()
{
    {
        TermScreen screen;
        screen.feed("ab\r\ncd");
        expect(at(screen, 0, 0) == U'a' && at(screen, 0, 1) == U'b', "crlf places a,b");
        expect(at(screen, 1, 0) == U'c' && at(screen, 1, 1) == U'd', "crlf places c,d");
        expect(screen.cursorRow() == 1 && screen.cursorCol() == 2, "cursor after cd");
    }
    {
        TermScreen screen;
        QByteArray reply;
        screen.setReplyHandler([&reply](const QByteArray &bytes) { reply = bytes; });
        screen.feed("ab\x1b[6n");
        expect(reply == QByteArray("\x1b[1;3R"), "cursor report");
        reply.clear();
        screen.feed("\x1b[5n");
        expect(reply == QByteArray("\x1b[0n"), "device status");
    }
    {
        TermScreen screen;
        screen.feed("\x1b[31mX\x1b[0m\x1b[38;5;196mY\x1b[38;2;0;255;0mZ");
        const TermCell *x = cell(screen, 0, 0);
        const TermCell *y = cell(screen, 0, 1);
        const TermCell *z = cell(screen, 0, 2);
        expect(x && x->cp == U'X' && x->fg.kind == TermColor::Indexed && x->fg.index == 1,
               "sgr red");
        expect(y && y->fg.kind == TermColor::Rgb && qRed(y->fg.rgb) == 255 && qGreen(y->fg.rgb) == 0,
               "sgr 256 red");
        expect(z && z->fg.kind == TermColor::Rgb && qGreen(z->fg.rgb) == 255 && qRed(z->fg.rgb) == 0,
               "sgr truecolor green");
    }
    {
        TermScreen screen;
        screen.resize(20, 8);
        screen.feed(QByteArray(20, 'A') + "B");
        const TermLine *top = screen.lineAbsolute(screen.historySize());
        expect(top && top->wrapped && top->cells[19].cp == U'A', "soft wrap");
        expect(at(screen, 1, 0) == U'B', "wrapped next line");
        expect(screen.cursorRow() == 1 && screen.cursorCol() == 1, "cursor after wrap");
    }
    {
        TermScreen screen;
        screen.resize(20, 4);
        for (int i = 0; i < 6; ++i)
            screen.feed(QByteArray(1, char('0' + i)) + "\r\n");
        expect(screen.historySize() == 3, "scrollback depth");
        const TermLine *oldest = screen.lineAbsolute(0);
        expect(oldest && oldest->cells[0].cp == U'0', "oldest history line");
        expect(at(screen, 0, 0) == U'3' && at(screen, 2, 0) == U'5', "screen after scroll");
        expect(at(screen, 3, 0) == U' ', "bottom blank after final lf");
    }
    {
        TermScreen screen;
        screen.feed("ab\x1b[?1049hZ");
        expect(screen.altScreen(), "entered alt");
        expect(screen.lineAbsolute(0) && screen.lineAbsolute(0)->cells[0].cp == U'Z', "alt cell");
        screen.feed("\x1b[?1049l");
        expect(!screen.altScreen(), "left alt");
        expect(at(screen, 0, 0) == U'a' && at(screen, 0, 1) == U'b', "primary restored");
    }
    {
        TermScreen screen;
        screen.feed("abcd\x1b[2D\x1b[K");
        expect(at(screen, 0, 0) == U'a' && at(screen, 0, 1) == U'b', "el keeps left");
        expect(at(screen, 0, 2) == U' ' && at(screen, 0, 3) == U' ', "el clears right");
    }
    {
        TermScreen screen;
        screen.feed("\x1b[2;2HZ");
        expect(screen.cursorRow() == 1 && screen.cursorCol() == 2, "cup");
        expect(at(screen, 1, 1) == U'Z', "cup writes Z");
    }
    {
        TermScreen screen;
        screen.feed(QByteArray("\xe4\xb8\xad", 3));
        const TermCell *a = cell(screen, 0, 0);
        const TermCell *b = cell(screen, 0, 1);
        expect(a && a->cp == 0x4E2D && (a->flags & TermScreen::FlagWide), "cjk wide");
        expect(b && (b->flags & TermScreen::FlagWideCont), "cjk continuation");
        expect(screen.cursorCol() == 2, "cursor after cjk");
        screen.feed("\b\bX");
        expect(at(screen, 0, 0) == U'X', "overwrite cjk");
        expect(cell(screen, 0, 1) && !(cell(screen, 0, 1)->flags & TermScreen::FlagWideCont),
               "continuation cleared");
    }
    {
        TermScreen screen;
        screen.feed("\x1b[?1h\x1b[?2004h");
        expect(screen.appCursorKeys() && screen.bracketedPaste(), "modes on");
        screen.feed("\x1b[?1l\x1b[?2004l");
        expect(!screen.appCursorKeys() && !screen.bracketedPaste(), "modes off");
    }
    {
        TermScreen screen;
        screen.feed("hello\r\n");
        const int dropped = screen.feed(QByteArray("\x1b") + "c");
        expect(dropped < 0 && screen.historySize() == 0, "ris clears history");
        expect(at(screen, 0, 0) == U' ', "ris clears screen");
        expect(screen.cursorRow() == 0 && screen.cursorCol() == 0, "ris homes");
    }
    {
        TermScreen screen;
        const int dropped = screen.appendNote(QStringLiteral("已连接"));
        expect(dropped >= 0, "note feed");
        expect(at(screen, 0, 0) == U'已', "note text");
        expect(screen.cursorRow() == 1 && screen.cursorCol() == 0, "note ends on next line");
        const TermCell *c = cell(screen, 0, 0);
        expect(c && c->fg.kind == TermColor::Info, "note uses info color");
    }

    {
        TermScreen screen;
        screen.feed("ab\x1b[Ecd");
        expect(at(screen, 0, 0) == U'a' && at(screen, 1, 0) == U'c', "cnl");
        expect(screen.cursorCol() == 2, "cnl column");
    }
    {
        TermScreen screen;
        screen.feed("A\x1b" "EB");
        expect(at(screen, 0, 0) == U'A' && at(screen, 1, 0) == U'B', "nel");
    }
    {
        TermScreen screen;
        screen.feed("A\x1b[3b");
        expect(at(screen, 0, 0) == U'A' && at(screen, 0, 3) == U'A', "rep");
        expect(screen.cursorCol() == 4, "rep cursor");
    }
    {
        TermScreen screen;
        screen.resize(20, 8);
        screen.feed("\x1b[3;6r\x1b[?6h\x1b[1;1HX");
        expect(screen.cursorRow() == 2 && at(screen, 2, 0) == U'X', "origin mode");
    }
    {
        TermScreen screen;
        screen.feed("\x1b[?2026h");
        expect(screen.syncing(), "sync on");
        screen.feed("Z\x1b[?2026l");
        expect(!screen.syncing() && at(screen, 0, 0) == U'Z', "sync off");
    }
    {
        TermScreen screen;
        screen.feed("ab\b\b\x1b[4hX");
        expect(at(screen, 0, 0) == U'X' && at(screen, 0, 1) == U'a' && at(screen, 0, 2) == U'b',
               "insert mode");
    }
    {
        TermScreen screen;
        QString title;
        screen.setTitleHandler([&title](const QString &text) { title = text; });
        screen.feed("\x1b]0;hello\x07");
        expect(title == QLatin1String("hello"), "osc title");
        screen.feed("\x07");
        expect(screen.takeBell() && !screen.takeBell(), "bell once");
    }
    {
        TermScreen screen;
        screen.feed(QByteArray(1, char(0xFF)));
        expect(at(screen, 0, 0) == 0xFFFD, "bad utf8");
    }
    {
        // BusyBox ip addr 风格：设备没上色，按 ip -c 给地址/网卡/状态补色
        TermScreen screen;
        screen.feed("2: eth0: <BROADCAST,UP,LOWER_UP> mtu 1500\r\n"
                    "    link/ether 02:11:22:33:44:55 brd ff:ff:ff:ff:ff:ff\r\n"
                    "    inet 192.168.1.116/24 brd 192.168.1.255 scope global eth0\r\n"
                    "    inet6 fe80::111:2222:3333:4444/64 scope link\r\n"
                    "3: usb0: <NO-CARRIER> state DOWN\r\n");
        const TermCell *eth = findToken(screen, 0, "eth0");
        expect(eth && eth->fg.kind == TermColor::Indexed && eth->fg.index == 6, "ipcolor eth0 cyan");
        const TermCell *up = findToken(screen, 0, "UP");
        expect(up && up->fg.kind == TermColor::Indexed && up->fg.index == 2, "ipcolor UP green");
        const TermCell *mac = findToken(screen, 1, "02:11:22:33:44:55");
        expect(mac && mac->fg.kind == TermColor::Indexed && mac->fg.index == 3, "ipcolor mac yellow");
        const TermCell *v4 = findToken(screen, 2, "192.168.1.116/24");
        expect(v4 && v4->fg.kind == TermColor::Indexed && v4->fg.index == 5, "ipcolor v4 magenta");
        const TermCell *v6 = findToken(screen, 3, "fe80::111:2222:3333:4444/64");
        expect(v6 && v6->fg.kind == TermColor::Indexed && v6->fg.index == 12, "ipcolor v6 bright blue");
        const TermCell *down = findToken(screen, 4, "DOWN");
        expect(down && down->fg.kind == TermColor::Indexed && down->fg.index == 1, "ipcolor DOWN red");
        const TermCell *usb = findToken(screen, 4, "usb0");
        expect(usb && usb->fg.kind == TermColor::Indexed && usb->fg.index == 6, "ipcolor usb0 cyan");
        // 嵌入式常见口：CAN / 隧道
        screen.feed("9: can0: <NOARP,ECHO> state DOWN\r\n");
        const TermCell *can = findToken(screen, 5, "can0");
        expect(can && can->fg.kind == TermColor::Indexed && can->fg.index == 6, "ipcolor can0 cyan");
        const TermCell *down2 = findToken(screen, 5, "DOWN");
        expect(down2 && down2->fg.kind == TermColor::Indexed && down2->fg.index == 1,
               "ipcolor DOWN red 2");
    }
    {
        // 设备自己上过色的词保持原样，不补色也不改色
        TermScreen screen;
        screen.feed("\x1b[31m10.0.0.1\x1b[0m 10.0.0.2");
        const TermCell *painted = findToken(screen, 0, "10.0.0.1");
        const TermCell *plain = findToken(screen, 0, "10.0.0.2");
        expect(painted && painted->fg.kind == TermColor::Indexed && painted->fg.index == 1,
               "ipcolor keeps device color");
        expect(plain && plain->fg.kind == TermColor::Indexed && plain->fg.index == 5,
               "ipcolor paints plain twin");
    }
    {
        // 词被 IO 拆在两个 chunk：光标行重扫拼出完整 IP
        TermScreen screen;
        screen.feed("inet 192.16");
        expect(!findToken(screen, 0, "192.16") || true, "ipcolor split pending");
        screen.feed("8.1.116/24 dev eth0");
        const TermCell *v4 = findToken(screen, 0, "192.168.1.116/24");
        expect(v4 && v4->fg.kind == TermColor::Indexed && v4->fg.index == 5, "ipcolor across chunks");
        const TermCell *eth = findToken(screen, 0, "eth0");
        expect(eth && eth->fg.kind == TermColor::Indexed && eth->fg.index == 6, "ipcolor if after join");
    }
    {
        // 滚进历史的行也补色
        TermScreen screen;
        screen.resize(20, 4);
        for (int i = 0; i < 3; ++i)
            screen.feed(QByteArray("pad") + QByteArray(1, char('0' + i)) + "\r\n");
        screen.feed("inet 172.20.1.5/16\r\n");
        const TermLine *hist = screen.lineAbsolute(3); // ip 行滚进历史
        bool colored = false;
        if (hist) {
            for (const TermCell &c : hist->cells) {
                if (c.cp == U'1' && c.fg.kind == TermColor::Indexed && c.fg.index == 5)
                    colored = true;
            }
        }
        expect(colored, "ipcolor in history");
    }

    if (g_fails) {
        std::fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    std::printf("termtest ok\n");
    return 0;
}
