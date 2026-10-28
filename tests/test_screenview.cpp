// 串口本地行编辑 与 全屏程序共存的状态机回归：
//   行编辑默认代画代编辑 → 远端画屏（BusyBox top 式 ESC[H/J，不切备用屏）
//   自动让路直通 → 提示符样纯文本输出 → 自动恢复行编辑。
//   vi 走备用屏（?1049h/l），期间按键直通且本地不代画。
// 全程离屏 + 模拟字节流，不需要真实设备。
#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QObject>
#include <QPixmap>
#include <QScrollBar>
#include <QtTest>

#include "screenview.h"

namespace {

struct Capture
{
    QByteArray sent;

    void reset() { sent.clear(); }
};

void key(ScreenView &view, int key, const QString &text = QString(),
         Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent ev(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(&view, &ev);
}

void typePlain(ScreenView &view, const char *s)
{
    for (const char *p = s; *p; ++p)
        key(view, Qt::Key_unknown, QString(QChar::fromLatin1(*p)));
}

QString lastLine(ScreenView &view)
{
    // 屏幕行常以留白收尾（单空格行不算空），取最后一个有字的行
    const QStringList lines = view.screenText().split(QLatin1Char('\n'));
    for (int i = lines.size() - 1; i >= 0; --i) {
        if (!lines.at(i).trimmed().isEmpty())
            return lines.at(i);
    }
    return QString();
}

} // namespace

class ScreenViewTest : public QObject
{
    Q_OBJECT

private slots:
    void topSuspendsLineEditUntilPrompt();
    void viAltScreenPassthrough();
    void busyBoxViInsertTypingKeepsPassthrough();
    void pureTerminalModeSendsKeysRaw();
    void localRedrawDoesNotSelfTrigger();
    void wheelBecomesArrowsWhileRemotePaints();
    void ctrlFOpensFindAndFullScreenPassesThrough();
    void keywordHighlightPaintsRuleColor();

private:
    void feed(ScreenView &view, const QByteArray &bytes)
    {
        view.receiveData(bytes);
        // 小块走同步解析；万一进了延迟泵，等它跑完再断言
        QTest::qWait(20);
    }
};

// BusyBox top 的一帧：定位回首页 + 擦到结尾 + 反显表头 + 逐行内容
static QByteArray topFrame()
{
    QByteArray f = "\x1b[H\x1b[J";
    f += "\x1b[7m  PID USER    %CPU %MEM COMMAND\x1b[m\r\n";
    f += "  123 root     1.2  0.5 top\r\n";
    f += "\x1b[7mMem: 100M used\r\n\x1b[m";
    return f;
}

void ScreenViewTest::topSuspendsLineEditUntilPrompt()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(true);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    // 提示符起步：行编辑生效，键入不上路，回车才发整行
    feed(view, QByteArray("root@board:~# "));
    typePlain(view, "top");
    QCOMPARE(cap.sent, QByteArray());
    key(view, Qt::Key_Return, QStringLiteral("\r"));
    QCOMPARE(cap.sent, QByteArray("top\n"));
    cap.reset();

    // top 开跑：每帧画屏。'q' 必须立刻直通，不能被行编辑扣住
    feed(view, topFrame());
    QVERIFY(view.screenText().contains(QStringLiteral("PID")));
    key(view, Qt::Key_Q, QStringLiteral("q"));
    QCOMPARE(cap.sent, QByteArray("q"));
    cap.reset();

    // 刷新几轮仍然直通
    feed(view, topFrame());
    key(view, Qt::Key_Up);
    QCOMPARE(cap.sent, QByteArray("\x1b[A"));
    cap.reset();

    // 退出：top 不发还原序列，shell 打印换行 + 提示符（新行、停在行中）
    feed(view, QByteArray("\r\nroot@board:~# "));
    typePlain(view, "ls");
    QCOMPARE(cap.sent, QByteArray());          // 行编辑已恢复
    QVERIFY(view.screenText().contains(QStringLiteral("ls"))); // 且本地代画
    key(view, Qt::Key_Return, QStringLiteral("\r"));
    QCOMPARE(cap.sent, QByteArray("ls\n"));
}

void ScreenViewTest::viAltScreenPassthrough()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(true);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("root@board:~# "));
    typePlain(view, "vi");
    key(view, Qt::Key_Return, QStringLiteral("\r"));
    cap.reset();

    // vi 进备用屏（BusyBox vi: ?1049h），画自己的画面
    feed(view, QByteArray("\x1b[?1049h\x1b[H\x1b[2J"));
    feed(view, QByteArray("int main(void)\r\n{\r\n"));
    QVERIFY(view.usingAltScreen());

    // 普通键直通，且本地回显不往 vi 的画面上画字
    typePlain(view, "x");
    QCOMPARE(cap.sent, QByteArray("x"));
    QVERIFY(view.screenText().contains(QStringLiteral("main")));
    QVERIFY(!view.screenText().contains(QLatin1Char('x')));

    // 退出备用屏 → 提示符（换行后出现）→ 行编辑恢复
    feed(view, QByteArray("\x1b[?1049l"));
    feed(view, QByteArray("\r\nroot@board:~# "));
    typePlain(view, "echo");
    QVERIFY(cap.sent == QByteArray("x"));
    QVERIFY(view.screenText().contains(QStringLiteral("echo")));
}

// BusyBox vi 不切备用屏，画屏走 remoteRaw 直通。插入模式的逐字回显
// （纯文本、无控制序列、不带换行）绝不能触发恢复行编辑——否则打一个字
// 就退出直通，上下方向键被本地行编辑劫持成翻历史，vi 里没法编辑。
void ScreenViewTest::busyBoxViInsertTypingKeepsPassthrough()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(true);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("root@board:~# "));
    typePlain(view, "vi");
    key(view, Qt::Key_Return, QStringLiteral("\r"));
    cap.reset();

    // BusyBox vi 开场：只发光标定位 + 整屏擦除，没有 ?1049h 备用屏
    feed(view, QByteArray("\x1b[H\x1b[2J"));
    feed(view, QByteArray("int main(void)\r\n{\r\n"));

    // 进入插入模式敲字：远端逐字回显，块里没有换行 → 必须保持直通
    typePlain(view, "abc");
    QCOMPARE(cap.sent, QByteArray("abc"));
    feed(view, QByteArray("abc"));

    // 上下方向键是发给 vi 的光标移动序列，不是本地翻历史
    cap.reset();
    key(view, Qt::Key_Up);
    QCOMPARE(cap.sent, QByteArray("\x1b[A"));
    key(view, Qt::Key_Down);
    QCOMPARE(cap.sent, QByteArray("\x1b[A\x1b[B"));

    // 退出：vi 还原画面（画屏序列）→ shell 打印换行提示符 → 行编辑恢复
    feed(view, QByteArray("\x1b[H\x1b[2J"));
    feed(view, QByteArray("\r\nroot@board:~# "));
    cap.reset();
    typePlain(view, "ls");
    QVERIFY(cap.sent.isEmpty()); // 回到行编辑，键入不上路
}

// 终端档（串口设置里的开关，落到视图就是 setLocalEcho(false)）：
// 与 SSH 完全一致——按键逐个裸发不等回车、本地不代画、不进远端画屏
// 状态机。Linux 串口控制台（vi 的单键提示、vim 全屏重画）走这条路，
// 不再被 STM32 CDC 那套本地行编辑扣键。
void ScreenViewTest::pureTerminalModeSendsKeysRaw()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(false); // 终端档
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("root@board:~# "));
    typePlain(view, "e");     // 逐键上路，不等回车
    QCOMPARE(cap.sent, QByteArray("e"));
    QVERIFY(!view.screenText().contains(QLatin1Char('e'))); // 本地不代画
    key(view, Qt::Key_Return, QStringLiteral("\r"));
    QCOMPARE(cap.sent, QByteArray("e\r")); // 回车是裸 \r，不是行编辑的提交

    // BusyBox vi 式画屏照常渲染，按键依旧全直通；Tab 也不被本地补全吃掉
    cap.reset();
    feed(view, QByteArray("\x1b[H\x1b[2Jint main(void)\r\n{\r\n"));
    QVERIFY(view.screenText().contains(QStringLiteral("main")));
    typePlain(view, "x");
    key(view, Qt::Key_Tab, QStringLiteral("\t"));
    key(view, Qt::Key_Up);
    QCOMPARE(cap.sent, QByteArray("x\t\x1b[A"));
}

void ScreenViewTest::localRedrawDoesNotSelfTrigger()
{
    // 行编辑自己的重画（ESC[r;cH 定位）走的是 receiveData，
    // 不能被误判成远端画屏而把行编辑自己关掉
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(true);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("root@board:~# "));
    typePlain(view, "hello ");
    typePlain(view, "world");
    QVERIFY(cap.sent.isEmpty());            // 仍在行编辑
    QCOMPARE(lastLine(view), QStringLiteral("root@board:~# hello world"));
    key(view, Qt::Key_Backspace);           // 行内编辑继续可用
    QVERIFY(lastLine(view).endsWith(QStringLiteral("worl")));
}

void ScreenViewTest::wheelBecomesArrowsWhileRemotePaints()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(true);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("root@board:~# "));
    // 行编辑期：滚轮翻本地滚动历史，不发字节
    QWheelEvent wheelDown(QPointF(10, 10), QPointF(10, 10), QPoint(0, -120),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollUpdate, false);
    QApplication::sendEvent(&view, &wheelDown);
    QVERIFY(cap.sent.isEmpty());

    feed(view, topFrame());
    QApplication::sendEvent(&view, &wheelDown);
    QVERIFY(cap.sent.contains(QByteArray("\x1b[B"))); // top 列表滚动
}

void ScreenViewTest::ctrlFOpensFindAndFullScreenPassesThrough()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(true);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    // 21 行埋一个关键词，再压 60 行把它推进滚动历史深处
    QByteArray block;
    for (int i = 0; i < 20; ++i)
        block += "line " + QByteArray::number(i) + "\r\n";
    block += "the NEEDLE row\r\n";
    for (int i = 0; i < 60; ++i)
        block += "tail " + QByteArray::number(i) + "\r\n";
    feed(view, block);

    QLineEdit *find = view.findChild<QLineEdit *>();
    QScrollBar *bar = view.findChild<QScrollBar *>();
    QVERIFY(find != nullptr);
    QVERIFY(bar != nullptr);
    view.show(); // isVisible 需要 widget 真正在屏（离屏平台也算）
    QVERIFY(find->isHidden());
    QCOMPARE(bar->value(), bar->maximum()); // 跟随底部

    // Ctrl+F 打开查找框（offscreen 下焦点链不可靠，断言可见性即可）
    key(view, Qt::Key_F, QStringLiteral("f"), Qt::ControlModifier);
    QVERIFY(find->isVisible());
    QVERIFY(cap.sent.isEmpty()); // 不吃行编辑场景下的任何输出

    // 输入关键词回车：命中在历史深处，视口应滚离底部
    for (const QChar &c : QStringLiteral("NEEDLE"))
        QApplication::sendEvent(find, new QKeyEvent(QEvent::KeyPress, Qt::Key_unknown,
                                                    Qt::NoModifier, QString(c)));
    QApplication::sendEvent(find, new QKeyEvent(QEvent::KeyPress, Qt::Key_Return,
                                                Qt::NoModifier, QStringLiteral("\r")));
    QVERIFY(bar->value() < bar->maximum());
    QVERIFY(find->isVisible());

    // Esc 关闭
    QApplication::sendEvent(find, new QKeyEvent(QEvent::KeyPress, Qt::Key_Escape,
                                                Qt::NoModifier, QString()));
    QVERIFY(find->isHidden());

    // vi 备用屏：Ctrl+F 是下翻页，必须原样直通 0x06
    feed(view, QByteArray("\x1b[?1049h\x1b[H"));
    QVERIFY(view.usingAltScreen());
    key(view, Qt::Key_F, QStringLiteral("f"), Qt::ControlModifier);
    QCOMPARE(cap.sent, QByteArray("\x06"));

    // 远端画屏（BusyBox top）期间同样直通
    feed(view, QByteArray("\x1b[?1049l"));
    feed(view, topFrame());
    key(view, Qt::Key_F, QStringLiteral("f"), Qt::ControlModifier);
    QCOMPARE(cap.sent, QByteArray("\x06\x06"));
}

void ScreenViewTest::keywordHighlightPaintsRuleColor()
{
    // 离屏下直接抓帧采像素：规则启用后 ERROR 五个字母的笔画应出现规则色，
    // 规则停用后恢复默认前景。关键词用小写，顺带验证不区分大小写。
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.show();
    feed(view, QByteArray("[  12.3] ERROR: sensor read failed\r\n"));

    // 纯绿画面对比度足够，主题里没有别的绿色元素
    const QColor rule(0x00, 0xFF, 0x00);
    auto greenPixels = [](const QImage &img) {
        int count = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                if (c.green() > 200 && c.red() < 80 && c.blue() < 80)
                    ++count;
            }
        return count;
    };

    KeywordRule rule_lower;
    rule_lower.text = QStringLiteral("error");
    rule_lower.color = rule;

    QVERIFY(greenPixels(view.grab().toImage()) == 0); // 未配置规则
    view.setKeywords({ rule_lower });
    QVERIFY(greenPixels(view.grab().toImage()) > 0);  // 命中并按规则色绘制

    rule_lower.enabled = false;
    view.setKeywords({ rule_lower });
    QVERIFY(greenPixels(view.grab().toImage()) == 0); // 停用即失效
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ScreenViewTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_screenview.moc"
