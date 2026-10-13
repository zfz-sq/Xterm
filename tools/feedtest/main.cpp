// 离屏回放: 把 ConPTY 抓到的真实字节流(toptest.py 落盘)喂给 TermScreen
// (SSH/串口两页共用的屏幕模型), 打印渲染出的可见文本。
// 构建: cmake -S tools/feedtest -B build/feedtest && cmake --build build/feedtest
// 运行: build/feedtest/feedtest.exe <dump.bin> [cols] [rows]
#include <QApplication>
#include <QFile>
#include <cstdio>

#include "../../termscreen.h"

namespace {

QString trimRight(const QString &s)
{
    int n = s.size();
    while (n > 0 && s.at(n - 1) == QLatin1Char(' '))
        --n;
    return s.left(n);
}

// TermScreen → 可见文本(只看当前屏; 备用屏时 lineAbsolute 是屏内行)
QStringList screenText(TermScreen &screen)
{
    QStringList out;
    const int rows = screen.rows();
    for (int r = 0; r < rows; ++r) {
        const TermLine *line = screen.lineAbsolute(r);
        QString s;
        if (line) {
            for (int c = 0; c < int(line->cells.size());) {
                const TermCell &cell = line->cells[size_t(c)];
                if (cell.flags & TermScreen::FlagWideCont) {
                    ++c;
                    continue;
                }
                s += QString::fromUcs4(&cell.cp, 1);
                c += (cell.flags & TermScreen::FlagWide) ? 2 : 1;
            }
        }
        out.append(trimRight(s));
    }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (argc < 2) {
        printf("usage: feedtest <dump.bin> [cols] [rows]\n");
        return 2;
    }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) {
        printf("cannot open %s\n", argv[1]);
        return 2;
    }
    const QByteArray raw = f.readAll();
    const int cols = argc > 2 ? atoi(argv[2]) : 100;
    const int rows = argc > 3 ? atoi(argv[3]) : 30;

    // ---- 1) SSH 页路径: TermScreen ----
    {
        TermScreen screen;
        screen.resize(cols, rows);
        QByteArray replies;
        screen.setReplyHandler([&replies](const QByteArray &b) { replies += b; });
        // 模拟真实到达节奏: 任意位置切断
        for (int i = 0; i < raw.size(); i += 977)
            screen.feed(raw.mid(i, 977));
        printf("=== TermScreen(两页共用屏幕模型) alt=%d cursor=(%d,%d) replies=%d字节 ===\n",
               int(screen.altScreen()), screen.cursorRow(), screen.cursorCol(),
               replies.size());
        const QStringList lines = screenText(screen);
        int shown = 0;
        for (int i = 0; i < lines.size(); ++i) {
            if (!lines.at(i).isEmpty()) {
                printf("%2d|%s\n", i + 1, lines.at(i).toUtf8().constData());
                ++shown;
            }
        }
        printf("非空行 %d/%d\n", shown, rows);
    }
    return 0;
}
