// 串口本地行编辑 与 全屏程序共存的状态机回归：
//   行编辑默认代画代编辑 → 远端画屏（BusyBox top 式 ESC[H/J，不切备用屏）
//   自动让路直通 → 提示符样纯文本输出 → 自动恢复行编辑。
//   vi 走备用屏（?1049h/l），期间按键直通且本地不代画。
// 全程离屏 + 模拟字节流，不需要真实设备。
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QObject>
#include <QPixmap>
#include <QScrollBar>
#include <QSignalSpy>
#include <QUrl>
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

// 反解已发送序列里最后一组 SGR 鼠标上报（\e[<code;x;yM/m）：坐标断言
// 不依赖字体度量，直接从行为输出取真实值。
struct SgrReport
{
    bool valid = false;
    int code = 0;
    int x = 0;
    int y = 0;
    bool release = false;
};

SgrReport lastSgr(const QByteArray &sent)
{
    SgrReport r;
    const int at = sent.lastIndexOf("\x1b[<");
    if (at < 0)
        return r;
    const int semi1 = sent.indexOf(';', at + 3);
    const int semi2 = semi1 >= 0 ? sent.indexOf(';', semi1 + 1) : -1;
    if (semi1 < 0 || semi2 < 0)
        return r;
    int end = semi2 + 1;
    while (end < sent.size() && sent.at(end) >= '0' && sent.at(end) <= '9')
        ++end;
    if (end >= sent.size() || (sent.at(end) != 'M' && sent.at(end) != 'm'))
        return r;
    r.valid = true;
    r.code = sent.mid(at + 3, semi1 - at - 3).toInt();
    r.x = sent.mid(semi1 + 1, semi2 - semi1 - 1).toInt();
    r.y = sent.mid(semi2 + 1, end - semi2 - 1).toInt();
    r.release = sent.at(end) == 'm';
    return r;
}

} // namespace

class ScreenViewTest : public QObject
{
    Q_OBJECT

private slots:
    void topSuspendsLineEditUntilPrompt();
    void viAltScreenPassthrough();
    void busyBoxViInsertTypingKeepsPassthrough();
    void viInsertEnterKeepsPassthrough();
    void ctrlCInterruptsFullScreenProgram();
    void xoffHintTogglesWithKey();
    void pureTerminalModeSendsKeysRaw();
    void localRedrawDoesNotSelfTrigger();
    void wheelBecomesArrowsWhileRemotePaints();
    void mouseTrackingReportsSgr();
    void mouseMotionModesGateReporting();
    void shiftKeepsLocalSelectionWhileTracking();
    void mouseOffKeepsLocalClicks();
    void wheelReportsWhileMouseTracking();
    void ctrlFOpensFindAndFullScreenPassesThrough();
    void keywordHighlightPaintsRuleColor();
    void ctrlClickEmitsLinkActivated();
    void osc52PolicyWritesOrDenies();
    void findRegexCyclesAndHighlightsAll();
    void tripleClickSelectsLine();
    void selectToCopyDefaultsOffOptIn();
    void waitAnchorUsesAbsoluteLines();

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

// vi 插入模式「打字 + 回车」的回显并成一块到达（串口缓冲合并）：可见字符、
// 换行、行中光标（autoindent 缩进）全齐，形状像极了提示符，但字全在换行
// 之前、换行之后只有空格。少了 feedPrintableAfterLf 判据时，这一块会被
// 当成提示符退回本地行编辑，之后的按键全被扣住不发：vi「无法输入、ESC
// 无效、也无法退出」。
void ScreenViewTest::viInsertEnterKeepsPassthrough()
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

    // BusyBox vi 开场：只发光标定位 + 整屏擦除，没有备用屏 → 直通
    feed(view, QByteArray("\x1b[H\x1b[2J"));
    feed(view, QByteArray("int main(void)\r\n{\r\n"));

    // 插入模式打 abc 再回车：按键逐个直通，vi 的回显一次性到达
    cap.reset();
    typePlain(view, "abc");
    key(view, Qt::Key_Return, QStringLiteral("\r"));
    QCOMPARE(cap.sent, QByteArray("abc\r")); // 没有被行编辑扣到回车为止
    feed(view, QByteArray("abc\r\n    "));

    // 仍是直通：方向键是 vi 的光标键（不是本地翻历史），普通键立刻上路
    cap.reset();
    key(view, Qt::Key_Up);
    QCOMPARE(cap.sent, QByteArray("\x1b[A"));
    typePlain(view, "d");
    QCOMPARE(cap.sent, QByteArray("\x1b[Ad"));

    // 真提示符（换行之后有字）仍要恢复行编辑
    feed(view, QByteArray("\r\nroot@board:~# "));
    cap.reset();
    typePlain(view, "ls");
    QVERIFY(cap.sent.isEmpty()); // 回到行编辑，键入不上路
}

// Ctrl+S（0x13/XOFF）可能挂起设备 tty 的输出，Ctrl+Q（0x11）恢复：
// 状态栏提示纯按按键时刻点亮/熄灭，不做无输出检测。
void ScreenViewTest::xoffHintTogglesWithKey()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(true);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });
    QSignalSpy xoff(&view, &ScreenView::xoffChanged);

    feed(view, QByteArray("root@board:~# "));
    feed(view, QByteArray("\x1b[H\x1b[2J")); // vi 式画屏，进入直通

    // vi 里随手 Ctrl+S：0x13 透传，提示亮起
    key(view, Qt::Key_S, QStringLiteral("s"), Qt::ControlModifier);
    QCOMPARE(cap.sent, QByteArray("\x13"));
    QCOMPARE(xoff.count(), 1);
    QVERIFY(xoff.at(0).at(0).toBool());

    // 普通按键不影响提示状态
    typePlain(view, "x");
    QCOMPARE(xoff.count(), 1);

    // Ctrl+Q：0x11 透传，提示熄灭
    key(view, Qt::Key_Q, QStringLiteral("q"), Qt::ControlModifier);
    QCOMPARE(cap.sent.right(1), QByteArray("\x11"));
    QCOMPARE(xoff.count(), 2);
    QVERIFY(!xoff.at(1).at(0).toBool());
}

// 全屏程序（备用屏 / 远端画屏）期间 Ctrl+C 是 ETX 中断，是 vi 卡住时最后
// 的逃生门；即使画面上有选区残留（在 vi 里双击选过词，vi 不滚动、选区
// 一直留着）也不能被「有选区时 Ctrl+C=复制」截留。行编辑场景的
// 「有选区 Ctrl+C=复制」惯例保持不变。
void ScreenViewTest::ctrlCInterruptsFullScreenProgram()
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

    // BusyBox vi 画屏（远端画屏直通），第 0 行有词可选
    feed(view, QByteArray("\x1b[H\x1b[2J"));
    feed(view, QByteArray("int main(void)\r\n{\r\n"));

    // 双击第 0 行选词 → 选区成立（选区文本为证），此时 Ctrl+C 仍必须发出 ETX
    QClipboard *clip = QGuiApplication::clipboard();
    clip->clear();
    const QPointF pos(30, 12);
    QMouseEvent press(QEvent::MouseButtonPress, pos, pos, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent dblClick(QEvent::MouseButtonDblClick, pos, pos, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &press);
    QApplication::sendEvent(&view, &dblClick);
    // 选区即复制默认关（M3.3）：选区成立不由剪贴板证明，直接看选区文本
    QCOMPARE(view.selectedText(), QStringLiteral("int"));
    key(view, Qt::Key_C, QStringLiteral("c"), Qt::ControlModifier);
    QCOMPARE(cap.sent, QByteArray("\x03"));

    // 备用屏（vim 式）同样直通
    feed(view, QByteArray("\x1b[?1049h\x1b[H\x1b[2J"));
    cap.reset();
    key(view, Qt::Key_C, QStringLiteral("c"), Qt::ControlModifier);
    QCOMPARE(cap.sent, QByteArray("\x03"));
    feed(view, QByteArray("\x1b[?1049l"));

    // 对照：回到提示符（行编辑）后，有选区时 Ctrl+C 仍是复制，不发给设备
    feed(view, QByteArray("\r\nroot@board:~# "));
    clip->clear();
    QApplication::sendEvent(&view, &press);
    QApplication::sendEvent(&view, &dblClick);
    QVERIFY(!view.selectedText().isEmpty()); // 选区在（自动复制默认关）
    cap.reset();
    key(view, Qt::Key_C, QStringLiteral("c"), Qt::ControlModifier);
    QVERIFY(cap.sent.isEmpty()); // 只复制，不发字节
    QVERIFY(!clip->text().isEmpty()); // 复制走了显式 Ctrl+C 路径
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

// 鼠标上报开启（vim 式 ?1049h + 1000 + 1006）：点击/双击/释放编码成 SGR
// 序列发给当前链路；关掉 1006 后回落 X10 老编码（BusyBox htop 只认这种），
// X10 无 release 事件。
void ScreenViewTest::mouseTrackingReportsSgr()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[H\x1b[2J"));

    const QPointF pos(30, 12);
    QMouseEvent press(QEvent::MouseButtonPress, pos, pos, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &press);
    const SgrReport p = lastSgr(cap.sent);
    QVERIFY(p.valid);
    QCOMPARE(p.code, 0);
    QVERIFY(!p.release);
    QVERIFY(p.x >= 1);
    QVERIFY(p.y >= 1);

    QApplication::sendEvent(&view, &release);
    const SgrReport r = lastSgr(cap.sent);
    QVERIFY(r.valid);
    QCOMPARE(r.code, 0);
    QCOMPARE(r.x, p.x); // 同点按下/释放：坐标一致
    QCOMPARE(r.y, p.y);
    QVERIFY(r.release);
    QCOMPARE(cap.sent.count("\x1b[<"), 2);

    // 双击是第二次按下的上报（Qt 不再补 press）
    QMouseEvent dblClick(QEvent::MouseButtonDblClick, pos, pos, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &dblClick);
    QCOMPARE(cap.sent.count("\x1b[<"), 3);
    QApplication::sendEvent(&view, &release);
    QCOMPARE(cap.sent.count("\x1b[<"), 4);

    // 回落 X10：\e[M + 三个 +32 单字节（共 6 字节）；release 无从表达，不发
    feed(view, QByteArray("\x1b[?1006l"));
    cap.reset();
    QApplication::sendEvent(&view, &press);
    QVERIFY(cap.sent.startsWith("\x1b[M"));
    QCOMPARE(cap.sent.size(), 6);
    QApplication::sendEvent(&view, &release);
    QCOMPARE(cap.sent.size(), 6); // X10：release 不上报
}

// motion 档位门控：1000 只报按下/释放；1002 加按住拖动（按钮+32）；1003
// 任何移动都报（无按钮 35）；同一格不重发。
void ScreenViewTest::mouseMotionModesGateReporting()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("\x1b[?1000h\x1b[?1006h"));
    QMouseEvent hover(QEvent::MouseMove, QPointF(30, 12), QPointF(30, 12),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &hover);
    QVERIFY(cap.sent.isEmpty()); // 1000：移动不上报

    feed(view, QByteArray("\x1b[?1002h"));
    QApplication::sendEvent(&view, &hover);
    QVERIFY(cap.sent.isEmpty()); // 1002：未按住按钮不报

    const QPointF pressPos(30, 12);
    const QPointF dragPos(90, 80);
    QMouseEvent press(QEvent::MouseButtonPress, pressPos, pressPos,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &press);
    cap.reset();
    QMouseEvent drag(QEvent::MouseMove, dragPos, dragPos, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &drag);
    const SgrReport d = lastSgr(cap.sent);
    QVERIFY(d.valid);
    QCOMPARE(d.code, 32); // 按住左键拖动：按钮 0 + motion 32
    QVERIFY(!d.release);
    QApplication::sendEvent(&view, &drag);
    QCOMPARE(cap.sent.count("\x1b[<"), 1); // 同格不重发

    QMouseEvent release(QEvent::MouseButtonRelease, dragPos, dragPos,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &release);
    cap.reset();
    // 1003：松开后移动也报（编码 35）；同格去重仍生效，换个格再动
    feed(view, QByteArray("\x1b[?1003h"));
    QApplication::sendEvent(&view, &drag);
    QVERIFY(cap.sent.isEmpty()); // 与拖动末格相同：去重
    QMouseEvent wander(QEvent::MouseMove, QPointF(150, 120), QPointF(150, 120),
                       Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &wander);
    const SgrReport w = lastSgr(cap.sent);
    QVERIFY(w.valid);
    QCOMPARE(w.code, 35); // 无按钮移动：3 + 32
}

// Shift+拖动强制本地选区（上报开着也不发一字节），复制行为不回归——
// vim 里想复制屏幕内容时唯一的逃生口。
void ScreenViewTest::shiftKeepsLocalSelectionWhileTracking()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("root@board:~# hello world\r\n"));
    feed(view, QByteArray("\x1b[?1002h\x1b[?1006h"));

    view.setSelectToCopy(true); // Shift 逃生口的自动复制是断言点，显式开启
    QClipboard *clip = QGuiApplication::clipboard();
    clip->clear();
    const QPointF from(20, 12);
    const QPointF to(110, 12);
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton,
                      Qt::LeftButton, Qt::ShiftModifier);
    QMouseEvent drag(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton,
                     Qt::ShiftModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton,
                        Qt::NoButton, Qt::ShiftModifier);
    QApplication::sendEvent(&view, &press);
    QApplication::sendEvent(&view, &drag);
    QApplication::sendEvent(&view, &release);

    QVERIFY(cap.sent.isEmpty()); // 全程零上报：选区优先
    QVERIFY(!clip->text().isEmpty()); // 本地选区成立并复制
}

// 上报未开启（默认）：点击/拖动/双击仍是本地选区行为，一个字节都不发。
void ScreenViewTest::mouseOffKeepsLocalClicks()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setLocalEcho(true);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("root@board:~# hello world\r\n"));
    view.setSelectToCopy(true); // 双击自动复制是断言点，显式开启
    QClipboard *clip = QGuiApplication::clipboard();
    clip->clear();
    const QPointF pos(30, 12);
    QMouseEvent press(QEvent::MouseButtonPress, pos, pos, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent dblClick(QEvent::MouseButtonDblClick, pos, pos, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &press);
    QApplication::sendEvent(&view, &dblClick);
    QVERIFY(!clip->text().isEmpty()); // 双击选词照常
    QVERIFY(cap.sent.isEmpty());      // 且不发任何字节
}

// 上报开启时滚轮交给程序（64/65），优先于备用屏方向键兼容分支——
// htop 滚列表靠它；Shift+滚轮仍留给本地历史。
void ScreenViewTest::wheelReportsWhileMouseTracking()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    Capture cap;
    QObject::connect(&view, &ScreenView::sendData,
                     [&cap](const QByteArray &bytes) { cap.sent += bytes; });

    feed(view, QByteArray("\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[H\x1b[2J"));
    QWheelEvent wheelDown(QPointF(10, 10), QPointF(10, 10), QPoint(0, -120),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollUpdate, false);
    QApplication::sendEvent(&view, &wheelDown);
    QVERIFY(cap.sent.contains("\x1b[<65;")); // 滚轮下 = 按钮 65
    QVERIFY(!cap.sent.contains(QByteArray("\x1b[B"))); // 不再是方向键兑底

    cap.reset();
    QWheelEvent wheelUp(QPointF(10, 10), QPointF(10, 10), QPoint(0, 120),
                        QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                        Qt::ScrollUpdate, false);
    QApplication::sendEvent(&view, &wheelUp);
    QVERIFY(cap.sent.contains("\x1b[<64;")); // 滚轮上 = 按钮 64

    cap.reset();
    QWheelEvent shiftDown(QPointF(10, 10), QPointF(10, 10), QPoint(0, -120),
                          QPoint(0, -120), Qt::NoButton, Qt::ShiftModifier,
                          Qt::ScrollUpdate, false);
    QApplication::sendEvent(&view, &shiftDown);
    QVERIFY(cap.sent.isEmpty()); // Shift+滚轮：本地滚动历史，不上报
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

// OSC 8 链接：Ctrl+点击命中链接格发 linkActivated（URL 原样）；
// 空白格 Ctrl+点击、无 Ctrl 的链接格点击都不触发（保持本地选区/双击语义）
void ScreenViewTest::ctrlClickEmitsLinkActivated()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    feed(view, QByteArrayLiteral("\x1b]8;;https://example.com/a\x07open\x1b]8;;\x07 tail"));
    QSignalSpy links(&view, &ScreenView::linkActivated);

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 12), QPointF(10, 12),
                      Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(&view, &press);
    QCOMPARE(links.count(), 1);
    QCOMPARE(links.at(0).at(0).toUrl(), QUrl(QStringLiteral("https://example.com/a")));

    // 同一行但无链接的空白格：不触发
    links.clear();
    QMouseEvent blank(QEvent::MouseButtonPress, QPointF(300, 12), QPointF(300, 12),
                      Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(&view, &blank);
    QCOMPARE(links.count(), 0);

    // 无 Ctrl 的链接格点击：不触发（选区/双击选词不受影响）
    links.clear();
    QMouseEvent plain(QEvent::MouseButtonPress, QPointF(10, 12), QPointF(10, 12),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &plain);
    QCOMPARE(links.count(), 0);
}

// OSC 52 策略：Allow 直接写系统剪贴板；Deny 拒绝并留屏上提示
// （Ask 弹窗是模态交互，留待真机验收）
void ScreenViewTest::osc52PolicyWritesOrDenies()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    QClipboard *clip = QGuiApplication::clipboard();

    view.setOsc52Policy(ScreenView::Osc52Policy::Allow);
    feed(view, QByteArrayLiteral("\x1b]52;c;aGVsbG8=\x07"));
    QCOMPARE(clip->text(), QStringLiteral("hello"));

    view.setOsc52Policy(ScreenView::Osc52Policy::Deny);
    clip->setText(QStringLiteral("keep"));
    feed(view, QByteArrayLiteral("\x1b]52;c;dGVzdA==\x07"));
    QCOMPARE(clip->text(), QStringLiteral("keep")); // 未被远端覆盖
    QVERIFY(view.screenText().contains(QStringLiteral("剪贴板"))); // 拒绝提示入屏
}

// find（M3.3）："." 切正则后 \d+ 命中全部数字串，Enter/Shift+Enter 环形
// 循环；关掉正则同串无命中；全部命中浅黄底确实上屏（像素容差计数）
void ScreenViewTest::findRegexCyclesAndHighlightsAll()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    feed(view, QByteArray("aa 11 bb\r\ncc 22 dd\r\naa 33 bb\r\n"));
    view.show(); // offscreen 下 isVisible 需要真正在屏（与 ctrlF 老用例一致）

    key(view, Qt::Key_F, QStringLiteral("f"), Qt::ControlModifier);
    QLineEdit *find = view.findChild<QLineEdit *>();
    QVERIFY(find != nullptr && find->isVisible());
    find->setText(QStringLiteral("\\d+"));
    view.setFindRegex(true); // 触发首次查找：光标在末行，环形回卷到 abs 0
    QCOMPARE(view.findHitRow(), 0);
    QCOMPARE(view.findHitCol(), 3); // "aa 11 bb" 里的 11

    auto enter = [&find](Qt::KeyboardModifiers mods) {
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Return, mods, QStringLiteral("\r"));
        QApplication::sendEvent(find, &ev);
    };
    enter(Qt::NoModifier);
    QCOMPARE(view.findHitRow(), 1); // 22
    enter(Qt::NoModifier);
    QCOMPARE(view.findHitRow(), 2); // 33
    enter(Qt::NoModifier);
    QCOMPARE(view.findHitRow(), 0); // 环回
    enter(Qt::ShiftModifier);
    QCOMPARE(view.findHitRow(), 2); // 逆向

    // 关掉正则：同串当纯文本找，无命中
    view.setFindRegex(false);
    QCOMPARE(view.findHitRow(), -1);
    view.setFindRegex(true);
    QCOMPARE(view.findHitRow(), 0); // 重开恢复命中

    // 全部命中浅黄底（暗色主题 0x4A3F0A）真画在屏上：容差计数
    QImage img = view.grab().toImage();
    int lightYellow = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (qAbs(c.red() - 0x4A) <= 25 && qAbs(c.green() - 0x3F) <= 25
                && qAbs(c.blue() - 0x0A) <= 25)
                ++lightYellow;
        }
    QVERIFY(lightYellow > 20); // 3 处数字串都有底色
}

// 三击选行：双击选词后第三击在 500ms 窗口内落下 → 整行选区（既有行为上锁）
void ScreenViewTest::tripleClickSelectsLine()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    feed(view, QByteArray("one two three\r\nrest\r\n"));

    const QPointF word(70, 12); // 第 0 行 col 4（two 区）
    QMouseEvent press(QEvent::MouseButtonPress, word, word, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, word, word, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &press);
    QApplication::sendEvent(&view, &dbl);
    QCOMPARE(view.selectedText(), QStringLiteral("two"));

    const QPointF third(100, 12); // 第三击（仍在 500ms 窗口内）
    QMouseEvent tri(QEvent::MouseButtonPress, third, third, Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &tri);
    QCOMPARE(view.selectedText(), QStringLiteral("one two three"));
}

// 选区即复制：默认关（拖完不进剪贴板，选区仍在）；显式开启后自动复制
void ScreenViewTest::selectToCopyDefaultsOffOptIn()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    feed(view, QByteArray("hello world\r\n"));
    QClipboard *clip = QGuiApplication::clipboard();
    clip->clear();

    const QPointF from(10, 12);  // col 0
    const QPointF to(80, 12);    // col 4（hello）
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent drag(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &press);
    QApplication::sendEvent(&view, &drag);
    QApplication::sendEvent(&view, &release);
    QVERIFY(clip->text().isEmpty());       // 默认关：不自动复制
    QCOMPARE(view.selectedText(), QStringLiteral("hello")); // 但选区成立

    view.setSelectToCopy(true);
    QApplication::sendEvent(&view, &press);
    QApplication::sendEvent(&view, &drag);
    QApplication::sendEvent(&view, &release);
    QCOMPARE(clip->text(), QStringLiteral("hello")); // 开启：松开即复制
}

// 环形历史 + 绝对行号（M3.4）：环形丢行后 recentLineCount 仍单调递增；
// wait 锚点（recentTextFrom）只含锚点之后的新增行——相对行号在环形丢行
// 后会漂移，绝对行号不会
void ScreenViewTest::waitAnchorUsesAbsoluteLines()
{
    ScreenView view;
    view.resize(100 * 15, 34 * 18);
    view.setMaxHistory(100); // 小环形（下限 100），强制丢最老行

    QByteArray block;
    for (int i = 0; i < 200; ++i)
        block += "line " + QByteArray::number(i) + "\r\n";
    feed(view, block);
    // 200 行：屏内第 34 个换行起往历史滚 167 行，环形只留 100（丢 67）；
    // 绝对总数 = 67 + 100 + 34 = 201（末尾一空行）
    QCOMPARE(view.recentLineCount(), qint64(201));

    // wait 语义：此刻记锚点，再喂 3 行（含唯一标记）。锚点后的新输出
    // 从锚点行起：MARKER_1 写在锚点行（abs 200），换行后的 201/202 才是「锚点后」
    const qint64 anchor = view.recentLineCount();
    feed(view, QByteArray("MARKER_1\r\nMARKER_2\r\nMARKER_3\r\n"));
    QCOMPARE(view.recentLineCount(), qint64(204)); // 单调涨，环形丢行不回退

    const QString fresh = view.recentTextFrom(anchor, 400);
    QVERIFY(fresh.contains(QStringLiteral("MARKER_2")));
    QVERIFY(fresh.contains(QStringLiteral("MARKER_3")));
    QVERIFY(!fresh.contains(QStringLiteral("line 199"))); // 锚点前行不进 fresh
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ScreenViewTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_screenview.moc"
