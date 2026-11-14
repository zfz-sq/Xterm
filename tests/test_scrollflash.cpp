#include <QtTest>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>

#include "scrollflash.h"

using namespace Qt::StringLiterals;

// ScrollFlash 黑盒用例：只能通过 dynamic property "flash" 与鼠标穿透属性观察
// （Flasher 是实现细节，不暴露接口）。属性翻转是 QSS 显隐的唯一数据源，
// 回归「滚动不浮现/待机不隐藏」都在这层断言。
class TestScrollFlash : public QObject
{
    Q_OBJECT

private slots:
    void attachStartsIdleAndClickThrough();
    void programmaticScrollDoesNotShow();
    void flashingOnUserScroll();
    void mouseLeftHidesImmediatelyAndClosesGate();
    void sliderDragSurvivesMouseLeft();
    void idleAgainAfterQuietPeriod();
    void continuousScrollingStaysFlashing();
    void rangeCollapseHidesBarImmediately();
    void noOverflowStaysHidden();
    void attachTreeCoversChildrenAndIsIdempotent();
    void idleBarRendersTransparentUnderThemeQss();
    void flashingTrackRendersThemeTrackColor();
};

void TestScrollFlash::attachStartsIdleAndClickThrough()
{
    QScrollBar bar(Qt::Vertical);
    ScrollFlash::attach(&bar);

    QCOMPARE(bar.property("flash").toString(), u"false"_s);
    // 待机即鼠标穿透：透明滚动条不拦截点击（终端选词落到底层）
    QVERIFY(bar.testAttribute(Qt::WA_TransparentForMouseEvents));
}

void TestScrollFlash::programmaticScrollDoesNotShow()
{
    // 用户定稿（2026-11-14）：日志信息流等程序化滚动（syncBar 的 setValue）不得
    // 点亮滚动条——只有滚轮/鼠标移入（noteUserActivity）才浮现
    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 100);
    ScrollFlash::attach(&bar);

    bar.setValue(30); // 程序滚动：门未开，不浮现
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
    QVERIFY(bar.testAttribute(Qt::WA_TransparentForMouseEvents));

    ScrollFlash::noteUserActivity(&bar); // 滚轮/鼠标移入：开门浮现
    QCOMPARE(bar.property("flash").toString(), u"true"_s);

    QTest::qWait(2300); // 2s 无操作：隐去并关门
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
    bar.setValue(60); // 门已关：后续程序滚动仍不浮现
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
}

void TestScrollFlash::flashingOnUserScroll()
{
    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 100);
    ScrollFlash::attach(&bar);

    // 用户滚动 = 门开 + valueChanged 汇聚（滚轮滚动即此路径）
    ScrollFlash::noteUserActivity(&bar);
    bar.setValue(30);

    QCOMPARE(bar.property("flash").toString(), u"true"_s);
    QVERIFY(!bar.testAttribute(Qt::WA_TransparentForMouseEvents));
}

void TestScrollFlash::mouseLeftHidesImmediatelyAndClosesGate()
{
    // 用户定稿（2026-11-14）：鼠标移出应用窗口立即隐去，不等 2s；隐去即关门
    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 100);
    ScrollFlash::attach(&bar);

    ScrollFlash::noteUserActivity(&bar);
    bar.setValue(30);
    QCOMPARE(bar.property("flash").toString(), u"true"_s);

    ScrollFlash::noteMouseLeft(&bar);
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
    QVERIFY(bar.testAttribute(Qt::WA_TransparentForMouseEvents));
    bar.setValue(60); // 门已关：后续程序滚动不得再点亮
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
}

void TestScrollFlash::sliderDragSurvivesMouseLeft()
{
    // 拖拽滑块时鼠标被滑块 grab，物理移出窗口也会触发宿主 leave：
    // 不得打断拖拽，松手后移出才照常隐去
    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 100);
    ScrollFlash::attach(&bar);

    ScrollFlash::noteUserActivity(&bar);
    bar.setValue(30);
    QCOMPARE(bar.property("flash").toString(), u"true"_s);

    bar.setSliderDown(true); // 模拟按住滑块拖拽
    ScrollFlash::noteMouseLeft(&bar);
    QCOMPARE(bar.property("flash").toString(), u"true"_s); // 拖拽中不隐去

    bar.setSliderDown(false);
    ScrollFlash::noteMouseLeft(&bar);
    QCOMPARE(bar.property("flash").toString(), u"false"_s); // 松手后照常隐去
}

void TestScrollFlash::idleAgainAfterQuietPeriod()
{
    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 100);
    ScrollFlash::attach(&bar);

    ScrollFlash::noteUserActivity(&bar);
    bar.setValue(40);
    QCOMPARE(bar.property("flash").toString(), u"true"_s);

    QTest::qWait(2300); // kIdleMs=2000 + 余量
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
    QVERIFY(bar.testAttribute(Qt::WA_TransparentForMouseEvents));
}

void TestScrollFlash::continuousScrollingStaysFlashing()
{
    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 100);
    ScrollFlash::attach(&bar);

    ScrollFlash::noteUserActivity(&bar);
    bar.setValue(10);
    QTest::qWait(800); // 未到 2000ms
    bar.setValue(30);  // 续期：闪烁计时重启
    QTest::qWait(800); // 距首次滚动已超 1600ms，但距上次滚动未到
    QCOMPARE(bar.property("flash").toString(), u"true"_s);

    QTest::qWait(2200);
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
}

void TestScrollFlash::rangeCollapseHidesBarImmediately()
{
    // 滚动中内容收缩到不足一屏：必须立即隐去，否则滑块占满全高成一条满色长条
    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 100);
    ScrollFlash::attach(&bar);

    ScrollFlash::noteUserActivity(&bar);
    bar.setValue(30);
    QCOMPARE(bar.property("flash").toString(), u"true"_s);

    bar.setRange(0, 0); // rangeChanged → maximum<=0 → 立即隐去（不等 2s）
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
    QVERIFY(bar.testAttribute(Qt::WA_TransparentForMouseEvents));
}

void TestScrollFlash::noOverflowStaysHidden()
{
    // 内容不足一屏（maximum=0）时即使发生滚动事件也不该浮现轨道
    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 0);
    ScrollFlash::attach(&bar);
    QCOMPARE(bar.property("flash").toString(), u"false"_s);

    bar.setValue(0); // 不可滚：无 valueChanged；即便触发 apply(true) 也被钳为隐藏
    QCOMPARE(bar.property("flash").toString(), u"false"_s);
    QVERIFY(bar.testAttribute(Qt::WA_TransparentForMouseEvents));
}

void TestScrollFlash::attachTreeCoversChildrenAndIsIdempotent()
{
    QScrollArea area;
    auto *inner = new QLabel(u"content"_s);
    area.setWidget(inner);
    area.resize(200, 100);
    area.show(); // 滚动条在此后才有几何/生命周期意义
    QTest::qWait(50);

    ScrollFlash::attachTree(&area);
    auto *vbar = area.verticalScrollBar();
    auto *hbar = area.horizontalScrollBar();
    QVERIFY(vbar);
    QCOMPARE(vbar->property("flash").toString(), u"false"_s);

    // 幂等：重复 attach 不产生第二个控制器（属性仍按单一定时器节奏翻转）。
    // 门在内容可滚后打开（真实滚轮路径：range 就位后才 wheel——arm 时 max=0 会被
    // 「不足一屏」钳为隐藏并关门，这是正确行为）
    ScrollFlash::attachTree(&area);
    vbar->setRange(0, 100);
    ScrollFlash::noteUserActivity(vbar);
    vbar->setValue(20);
    QCOMPARE(vbar->property("flash").toString(), u"true"_s);
    QTest::qWait(2200);
    QCOMPARE(vbar->property("flash").toString(), u"false"_s);

    Q_UNUSED(hbar); // 宽内容不足时 horizontal 可能为空/不可滚，不做强断言
}

void TestScrollFlash::idleBarRendersTransparentUnderThemeQss()
{
    // 渲染级回归（2026-11-14 用户截图：空终端右缘仍有常态滑块色的全高竖条）：
    // 属性 flash=false 正确不代表 QSS 生效——按主题注入同款规则（基础待机透明/
    // 长度钳制在前，override handle 色在后）实渲染，待机时空 range 不应出现滑块色
    qApp->setStyleSheet(QStringLiteral(
        "QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }\n"
        "QScrollBar::handle:vertical { background: #4A4A52; min-height: 48px; max-height: 100px; }\n"
        "QScrollBar[flash=\"false\"]::handle:vertical { background: transparent; }\n"
        "QScrollBar::handle:vertical:hover { background: #5A5A62; }\n"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }\n"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }\n"
        "QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background-color: #285049; }\n"
        "QScrollBar[flash=\"true\"]:vertical { background-color: #2C2F3F; }\n"));

    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 0); // 空终端：不可滚
    bar.resize(5, 300);
    ScrollFlash::attach(&bar);
    bar.show();
    QTest::qWait(120); // polish + 属性生效

    QCOMPARE(bar.property("flash").toString(), u"false"_s);
    const QImage img = bar.grab().toImage();
    const QColor px = img.pixelColor(2, 150);
    qWarning() << "[probe] idle bar pixel:" << px << "size:" << img.size();
    // 常态滑块色 #285049 不应出现在待机态（透明/父背景色皆可，唯独不是它）
    QVERIFY(px != QColor(0x28, 0x50, 0x49));
}

void TestScrollFlash::flashingTrackRendersThemeTrackColor()
{
    // 渲染级回归（2026-11-14 用户反馈「滑轨没有显示」）：flash=true 时轨道必须真的
    // 画出来——按主题注入同款规则（termius 钦定实色轨道 #2c2f3f）实渲染取证
    qApp->setStyleSheet(QStringLiteral(
        "QScrollBar:vertical { background: transparent; width: 5px; margin: 0; }\n"
        "QScrollBar::handle:vertical { background: #4A4A52; min-height: 48px; max-height: 100px; }\n"
        "QScrollBar[flash=\"false\"]::handle:vertical { background: transparent; }\n"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }\n"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }\n"
        "QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background-color: #285049; }\n"
        "QScrollBar[flash=\"true\"]:vertical { background-color: #2C2F3F; }\n"));

    QScrollBar bar(Qt::Vertical);
    bar.setRange(0, 100);
    bar.setValue(50); // 滑块居中：上部/下部都是纯轨道
    bar.resize(5, 300);
    ScrollFlash::attach(&bar);
    bar.show();
    ScrollFlash::noteUserActivity(&bar); // 用户滚动浮现
    QTest::qWait(120); // polish + 属性生效

    QCOMPARE(bar.property("flash").toString(), u"true"_s);
    const QImage img = bar.grab().toImage();
    const QColor track = img.pixelColor(2, 270); // 滑块钳在 48~100px：y=270 必是轨道
    const QColor handle = img.pixelColor(2, 170); // pageStep=1、value=50：滑块钳制在 y≈150-198
    qWarning() << "[probe] track pixel:" << track << "handle pixel:" << handle;
    QCOMPARE(track, QColor(0x2C, 0x2F, 0x3F)); // 滑轨实色（termius 钦定）
    QCOMPARE(handle, QColor(0x28, 0x50, 0x49)); // 滑块常态色同步亮起
}

QTEST_MAIN(TestScrollFlash)
#include "test_scrollflash.moc"
