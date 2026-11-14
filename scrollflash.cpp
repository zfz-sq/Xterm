#include "scrollflash.h"

#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QWidget>

namespace {

// 停止滚动后滑块停留多久隐去（用户定稿：不滚动 2s 后自动隐藏）
constexpr int kIdleMs = 2000;

class Flasher : public QObject
{
public:
    explicit Flasher(QScrollBar *bar)
        : QObject(bar), m_bar(bar)
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(kIdleMs);
        connect(&m_timer, &QTimer::timeout, this, [this] { apply(false); });
        // 滚轮/拖动等用户滚动与日志流等程序滚动都经 valueChanged 汇聚，但只有
        // 「用户滚动门」开着才浮现（noteUserActivity）：输出刷屏不点亮滚动条
        connect(m_bar, &QScrollBar::valueChanged, this, [this](int) {
            if (m_armed)
                apply(true);
        });
        // 内容收缩到不足一屏（range 变不可滚）时立即隐去：否则滑块会占满全高成一条满色长条
        connect(m_bar, &QScrollBar::rangeChanged, this, [this](int, int) {
            if (m_bar->maximum() <= 0)
                apply(false);
        });
        apply(false); // 初始待机
    }

    // 打开用户滚动门并浮现（滚轮/鼠标进入时由宿主调用）
    void arm() { m_armed = true; apply(true); }

    // 立即隐去并关门（鼠标移出窗口时由宿主调用）：停掉待机计时，不等 2s
    void disarm()
    {
        m_timer.stop();
        apply(false);
    }

private:
    void apply(bool shown)
    {
        if (shown && m_bar->maximum() <= 0)
            shown = false; // 内容不足一屏：无滑块可显示，连轨道也不浮现
        if (shown) {
            m_timer.start(); // 续期先行：属性已是 true 时 early-return 也要重启计时，
                             // 否则连续滚动会在 2s 内中途隐去
        } else {
            m_armed = false; // 隐去即关门：之后的程序滚动不得再点亮
        }
        const QVariant cur = m_bar->property("flash");
        const QString want = shown ? QStringLiteral("true") : QStringLiteral("false");
        if (cur.isValid() && cur.toString() == want)
            return; // 属性已就位，无需重求样式
        m_bar->setProperty("flash", want);
        // 属性选择器变化需重新求值样式，仅 update() 不会重解析 QSS
        m_bar->style()->unpolish(m_bar);
        m_bar->style()->polish(m_bar);
        m_bar->update();
        // 待机时鼠标穿透：透明滚动条不应拦截点击（终端选词/列表点击落到底层控件）
        m_bar->setAttribute(Qt::WA_TransparentForMouseEvents, !shown);
    }

    QScrollBar *m_bar;
    QTimer m_timer;
    bool m_armed = false; // 用户滚动门：滚轮/鼠标进入开，隐去即关
};

} // namespace

namespace ScrollFlash {

void attach(QScrollBar *bar)
{
    if (!bar || bar->property("_scrollFlashAttached").toBool())
        return;
    bar->setProperty("_scrollFlashAttached", true);
    auto *flasher = new Flasher(bar); // parent = bar，随 bar 析构
    // Flasher 在匿名 namespace（无 Q_OBJECT），不能 findChildren/qobject_cast 定位，
    // 用属性存裸 QObject* 供 noteUserActivity 取回（写入点仅此处，类型受控）
    bar->setProperty("_scrollFlasher", QVariant::fromValue<QObject *>(flasher));
}

void attachTree(QWidget *root)
{
    if (!root)
        return;
    const auto bars = root->findChildren<QScrollBar *>();
    for (QScrollBar *bar : bars)
        attach(bar);
}

void noteUserActivity(QScrollBar *bar)
{
    if (!bar)
        return;
    const QVariant v = bar->property("_scrollFlasher");
    if (!v.isValid())
        return; // 未 attach：无门可开（渐进增强下该条保持常显）
    if (auto *flasher = static_cast<Flasher *>(v.value<QObject *>()))
        flasher->arm();
}

void noteMouseLeft(QScrollBar *bar)
{
    if (!bar)
        return;
    // 拖拽滑块时鼠标被滑块 grab，物理移出窗口也会触发宿主 leave：
    // 不打断拖拽，松手后由 2s 待机接管
    if (bar->isSliderDown())
        return;
    const QVariant v = bar->property("_scrollFlasher");
    if (!v.isValid())
        return;
    if (auto *flasher = static_cast<Flasher *>(v.value<QObject *>()))
        flasher->disarm();
}

} // namespace ScrollFlash
