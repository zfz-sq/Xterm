// TermScreen 缩放行为回归：放大时从滚动历史回填（xterm 语义），
// 缩小时顶部行推入历史，任何缩放序列都不丢内容。
#include <QtTest>

#include "termscreen.h"

namespace {

QString lineText(const TermScreen &screen, int abs)
{
    const TermLine *line = screen.lineAbsolute(abs);
    if (line == nullptr)
        return QString();
    QString text;
    for (const TermCell &cell : line->cells) {
        if (cell.flags & TermScreen::FlagWideCont)
            continue;
        if (cell.cp >= 0x20)
            text += QString::fromUcs4(&cell.cp, 1);
    }
    return text.trimmed();
}

QByteArray numberedLines(int from, int to)
{
    QByteArray block;
    for (int i = from; i < to; ++i)
        block += "LINE_" + QByteArray::number(i) + "\r\n";
    return block;
}

} // namespace

class TermScreenTest : public QObject
{
    Q_OBJECT

private slots:
    void growBackfillsFromHistory();
    void growContinuesWritingAfterBackfill();
    void growExhaustedHistoryLeavesNoHole();
    void shrinkKeepsEverythingReachable();
    void resizeStormLosesNothing();
    void screenCtlFlagTracksPainterBlocks();
};

void TermScreenTest::growBackfillsFromHistory()
{
    TermScreen screen;
    screen.resize(80, 10);
    screen.feed(numberedLines(0, 25)); // 屏幕 16..24，历史 0..15

    QCOMPARE(screen.historySize(), 16);
    QCOMPARE(lineText(screen, 16), QStringLiteral("LINE_16"));

    screen.resize(80, 16); // 放大 6 行
    QCOMPARE(screen.historySize(), 10);
    QCOMPARE(lineText(screen, 10), QStringLiteral("LINE_10"));
    QCOMPARE(lineText(screen, 15), QStringLiteral("LINE_15"));
    QCOMPARE(lineText(screen, 16), QStringLiteral("LINE_16"));
    QCOMPARE(lineText(screen, 24), QStringLiteral("LINE_24"));
}

void TermScreenTest::growContinuesWritingAfterBackfill()
{
    TermScreen screen;
    screen.resize(80, 10);
    screen.feed(numberedLines(0, 25));
    screen.resize(80, 16);

    screen.feed(numberedLines(25, 26));
    QCOMPARE(lineText(screen, 25), QStringLiteral("LINE_25"));
    // 光标行（26）依然为空：回填把光标随内容一起挪了，没有跳位
    QCOMPARE(lineText(screen, 26), QString());
}

void TermScreenTest::growExhaustedHistoryLeavesNoHole()
{
    TermScreen screen;
    screen.resize(80, 10);
    screen.feed(numberedLines(0, 25));
    screen.resize(80, 40); // 一次要 30 行，历史只有 16 行
    QCOMPARE(screen.historySize(), 0);
    QCOMPARE(lineText(screen, 0), QStringLiteral("LINE_0"));
    QCOMPARE(lineText(screen, 24), QStringLiteral("LINE_24"));
}

void TermScreenTest::shrinkKeepsEverythingReachable()
{
    TermScreen screen;
    screen.resize(80, 20);
    screen.feed(numberedLines(0, 30)); // 历史 0..10，屏幕 11..29 + 空光标行

    screen.resize(80, 8); // 屏幕顶部 12 行进历史
    QCOMPARE(screen.historySize(), 23);
    for (int i = 0; i < 23; ++i)
        QCOMPARE(lineText(screen, i), QStringLiteral("LINE_%1").arg(i));
    QCOMPARE(lineText(screen, 29), QStringLiteral("LINE_29"));
}

void TermScreenTest::resizeStormLosesNothing()
{
    TermScreen screen;
    screen.resize(80, 30);
    int next = 0;
    auto feedMore = [&screen, &next](int n) {
        screen.feed(numberedLines(next, next + n));
        next += n;
    };

    feedMore(200);
    screen.resize(80, 18);
    feedMore(50);
    screen.resize(80, 45);
    feedMore(60);
    screen.resize(80, 20);
    feedMore(30);
    screen.resize(80, 40);
    feedMore(30);

    // 全序列无缺失、无重复：绝对行拼起来正好是 LINE_0..next-1
    const int total = screen.historySize() + screen.rows();
    int expect = 0;
    for (int abs = 0; abs < total; ++abs) {
        const QString text = lineText(screen, abs);
        if (text.isEmpty())
            continue;
        if (text == QStringLiteral("LINE_%1").arg(expect)) {
            ++expect;
        } else {
            expect = -1;
            break;
        }
    }
    QCOMPARE(expect, next);
}

// 画屏标志：BusyBox top 式帧（ESC[H + 重画）置位，提示符样纯文本块清零，
// 而且光标停在行中、写了可见字符——ScreenView 依此判定全屏程序进出。
void TermScreenTest::screenCtlFlagTracksPainterBlocks()
{
    TermScreen screen;
    screen.resize(100, 34);

    // shell 提示符：纯文本，不是画屏
    screen.feed("root@board:~# ");
    QVERIFY(!screen.screenCtlSeen());
    QVERIFY(screen.feedPrintable() > 0);
    QVERIFY(screen.cursorCol() > 0);

    // BusyBox top 一帧：ESC[H ESC[J + 反显表头 + 行内容
    QByteArray frame = "\x1b[H\x1b[J";
    frame += "\x1b[7m  PID USER %CPU COMMAND\x1b[m\r\n";
    frame += "  123 root  1.2 top\r\n";
    screen.feed(frame);
    QVERIFY(screen.screenCtlSeen());

    // 退出后 shell 直接打提示符（纯文本、停在行中）
    screen.feed("root@board:~# ");
    QVERIFY(!screen.screenCtlSeen());
    QVERIFY(screen.feedPrintable() > 0);
    QVERIFY(screen.cursorCol() > 0);

    // 整屏擦除（clear / vi 起手）也算画屏
    screen.feed("\x1b[H\x1b[2J");
    QVERIFY(screen.screenCtlSeen());
}

QTEST_MAIN(TermScreenTest)

#include "test_termscreen.moc"
