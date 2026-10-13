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

    if (g_fails) {
        std::fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    std::printf("termtest ok\n");
    return 0;
}
