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
    void feedStatsSeparatePromptFromViEcho();
    void boldMapsIndexedToBright();
    void mouseModesParseAndReset();
    void osc8AnnotatesCells();
    void osc52DecodesBase64();
    void historyRingBufferCapsAndCounts();
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

// feed 统计把「提示符」与「vi 插入模式打字+回车的回显」分开：前者的字
// 写在换行之后，后者的字全在换行之前（换行后只有 autoindent 缩进空格，
// 空格不计可见字符）。ScreenView 依此决定何时从全屏直通恢复本地行编辑。
void TermScreenTest::feedStatsSeparatePromptFromViEcho()
{
    TermScreen screen;
    screen.resize(100, 34);

    // vi 插入模式：打字 + 回车的回显并成一块（串口缓冲合并）
    screen.feed("abc\r\n    ");
    QCOMPARE(screen.feedPrintable(), 3);
    QCOMPARE(screen.feedNewlines(), 1);
    QCOMPARE(screen.feedPrintableAfterLf(), 0);

    // shell 提示符：换行之后跟着写出的字
    screen.feed("\r\nroot@board:~# ");
    QCOMPARE(screen.feedNewlines(), 1);
    QCOMPARE(screen.feedPrintableAfterLf(), 13);

    // 每次 feed 重新起算
    screen.feed("\r\n");
    QCOMPARE(screen.feedPrintableAfterLf(), 0);
}

// 粗体亮色（bold-is-bright）：SGR 1;31 与 SGR 91 渲染成同一个颜色。
// ls / dmesg / gcc 都用「1;3x」表达亮色，不映射会比 MobaXterm 显暗。
void TermScreenTest::boldMapsIndexedToBright()
{
    TermScreen screen;
    screen.resize(80, 2);
    screen.feed("\x1b[1;31mA\x1b[m\x1b[91mB");

    const TermLine *line = screen.lineAbsolute(0);
    QVERIFY(line != nullptr && line->cells.size() >= 2);
    const TermCell &bold = line->cells.at(0);
    const TermCell &bright = line->cells.at(1);
    QVERIFY(bold.attr & TermScreen::AttrBold);
    QCOMPARE(bold.fg.index, quint8(1)); // 存储仍是普通红
    QCOMPARE(bright.fg.index, quint8(9));
    QCOMPARE(screen.foreground(bold), screen.foreground(bright)); // 画出来同色
}

// 鼠标上报模式解析（DECSET/DECRST 1000/1002/1003/1006）：档位互斥、
// 后设覆盖前设、DECRST 只关自己这档、RIS 全复位清零。
// ScreenView 据此决定鼠标事件走远端上报还是本地选区。
void TermScreenTest::mouseModesParseAndReset()
{
    TermScreen screen;
    QCOMPARE(screen.mouseTrackingMode(), 0);
    QVERIFY(!screen.mouseSgrEncoding());

    screen.feed("\x1b[?1000h\x1b[?1006h"); // vim/htop 标准开场
    QCOMPARE(screen.mouseTrackingMode(), 1000);
    QVERIFY(screen.mouseSgrEncoding());

    screen.feed("\x1b[?1002h"); // 互斥：新档覆盖旧档
    QCOMPARE(screen.mouseTrackingMode(), 1002);
    screen.feed("\x1b[?1003h");
    QCOMPARE(screen.mouseTrackingMode(), 1003);
    screen.feed("\x1b[?1000l"); // 关闭只关自己这档
    QCOMPARE(screen.mouseTrackingMode(), 1003);
    screen.feed("\x1b[?1006l");
    QVERIFY(!screen.mouseSgrEncoding());
    screen.feed("\x1b[?1003l");
    QCOMPARE(screen.mouseTrackingMode(), 0);

    screen.feed("\x1b[?1002h\x1b[?1006h");
    screen.feed("\x1b" "c"); // RIS 全复位：鼠标模式一并清零
    QCOMPARE(screen.mouseTrackingMode(), 0);
    QVERIFY(!screen.mouseSgrEncoding());
}

// OSC 8 超链接解析：链接段 cell 带池化 uri（同一 URL 去重），空 URI 关闭；
// BEL 与 ST 两种终止符等价；链接段外的字符不带链接
void TermScreenTest::osc8AnnotatesCells()
{
    TermScreen screen;
    screen.resize(80, 5);
    screen.feed("\x1b]8;;https://example.com\x1b\\go\x1b]8;;\x1b\\ tail");

    const TermLine *line = screen.lineAbsolute(0);
    QVERIFY(line != nullptr && line->cells.size() >= 9);
    const quint32 uri = line->cells.at(0).uri; // 'g'
    QVERIFY(uri != TermCell::NoUri);
    QCOMPARE(screen.uriAt(uri), QStringLiteral("https://example.com"));
    QCOMPARE(line->cells.at(1).uri, uri);              // 'o' 同一链接
    QVERIFY(line->cells.at(2).uri == TermCell::NoUri); // 关闭后的空格
    QVERIFY(line->cells.at(6).uri == TermCell::NoUri); // " tail" 的 'l'

    // 同一 URL 再开：池去重，两次下标一致
    screen.feed("\x1b]8;;https://example.com\x07xx\x1b]8;;\x07");
    const TermLine *again = screen.lineAbsolute(0);
    QCOMPARE(again->cells.at(7).uri, uri); // "go tail" 后继续写的第一个 'x'
}

// OSC 52 剪贴板：base64 解码后回调；'?' 查询、非法 base64、缺 selection
// 分号都不触发
void TermScreenTest::osc52DecodesBase64()
{
    TermScreen screen;
    QByteArray got;
    screen.setClipboardHandler([&got](const QByteArray &data) { got = data; });

    screen.feed("\x1b]52;c;aGVsbG8=\x07"); // "hello"，BEL 终止
    QCOMPARE(got, QByteArrayLiteral("hello"));
    got.clear();

    screen.feed("\x1b]52;p;dGVzdA==\x1b\\"); // "test"，ST 终止 + primary
    QCOMPARE(got, QByteArrayLiteral("test"));
    got.clear();

    screen.feed("\x1b]52;c;?\x07"); // 查询不支持
    QVERIFY(got.isEmpty());
    screen.feed("\x1b]52;c;!!\x07"); // 非法 base64
    QVERIFY(got.isEmpty());
    screen.feed("\x1b]52;c\x07"); // 没有第二个分号
    QVERIFY(got.isEmpty());
}

// 环形历史（M3.4）：上限可配（默认 10000），超限丢最老；绝对基数
// droppedLines 单调累计（含清屏/收紧上限丢弃），总数不回退
void TermScreenTest::historyRingBufferCapsAndCounts()
{
    TermScreen screen;
    screen.resize(80, 10);
    QCOMPARE(screen.maxHistory(), 10000); // 新默认（旧硬编码 8000）

    screen.setMaxHistory(100); // 下限 100（qBound 钳位，太小失去回滚意义）
    // 喂 130 行：屏内第 10 个换行起就往历史滚，共 121 行（末尾多一空行），
    // 环形只留 100，最老 21 行被丢
    QByteArray block;
    for (int i = 0; i < 130; ++i)
        block += "L" + QByteArray::number(i) + "\r\n";
    screen.feed(block);
    QCOMPARE(screen.historySize(), 100);
    QCOMPARE(screen.droppedLines(), qint64(21));
    QCOMPARE(lineText(screen, 0), QStringLiteral("L21")); // 最老现存行
    // 绝对总数 = 丢弃 + 现存（含尾空行）
    QCOMPARE(screen.droppedLines() + screen.historySize() + screen.rows(), qint64(131));

    // 再喂 5 行：丢 5 留 100，总数单调涨（内存封顶、行号不封顶）
    QByteArray more;
    for (int i = 130; i < 135; ++i)
        more += "L" + QByteArray::number(i) + "\r\n";
    screen.feed(more);
    QCOMPARE(screen.historySize(), 100);
    QCOMPARE(screen.droppedLines(), qint64(26));
    QCOMPARE(screen.droppedLines() + screen.historySize() + screen.rows(), qint64(136));

    // 清滚动历史（ED 3，私有接口的外部触发链路）：丢弃计入基数，总数不回退
    screen.feed("\x1b[3J");
    QCOMPARE(screen.historySize(), 0);
    QCOMPARE(screen.droppedLines() + screen.rows(), qint64(136));
}

QTEST_MAIN(TermScreenTest)

#include "test_termscreen.moc"
