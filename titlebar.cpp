#include "titlebar.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLibrary>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QToolButton>
#include <QWindow>

namespace {
constexpr int kBarHeight = 30;
constexpr int kBtnSize = 40; // 系统按钮宽度（VSCode 同款横向矩形热区）
} // namespace

// ---------- CaptionButton: 矢量画 Win11 风格系统按钮字形 ----------

CaptionButton::CaptionButton(Glyph glyph, QWidget *parent)
    : QToolButton(parent)
    , m_glyph(glyph)
{
    setFixedSize(kBtnSize, kBarHeight);
    setFocusPolicy(Qt::NoFocus);
}

void CaptionButton::paintEvent(QPaintEvent *event)
{
    // QSS 负责底色（hover 灰 / pressed 深灰 / 关闭键红），这里只叠字形
    QToolButton::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal cx = rect().center().x();
    const qreal cy = rect().center().y();

    QColor c = m_color;
    if (m_glyph == Close && (underMouse() || isDown()))
        c = QColor(Qt::white); // 关闭键 hover 红底 (#C42B1C) 上用白叉
    QPen pen(c, 1);
    pen.setCapStyle(Qt::FlatCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (m_glyph) {
    case Minimize:
        // 10px 横线（+0.5 落在像素行中央，线条清晰）
        p.drawLine(QLineF(cx - 5, cy + 0.5, cx + 5, cy + 0.5));
        break;
    case Maximize:
        // 9x9 方框
        p.drawRect(QRectF(cx - 4.5, cy - 4.5, 9, 9));
        break;
    case Restore: {
        // ❐: 前方框(左下)完整；后方框(右上)被遮住左下角，只露 顶边+右边+两段短边
        QPolygonF back;
        back << QPointF(cx - 2.5, cy - 2.5)  // 左边可见段下端
             << QPointF(cx - 2.5, cy - 5.5)  // 左边上端
             << QPointF(cx + 5.5, cy - 5.5)  // 顶边
             << QPointF(cx + 5.5, cy + 2.5)  // 右边+底边右段起点
             << QPointF(cx + 2.5, cy + 2.5); // 底边右段终点
        p.drawPolyline(back);
        p.drawRect(QRectF(cx - 5.5, cy - 2.5, 8, 8)); // 前方框
        break;
    }
    case Close:
        // 10px 斜叉
        p.drawLine(QLineF(cx - 5, cy - 5, cx + 5, cy + 5));
        p.drawLine(QLineF(cx + 5, cy - 5, cx - 5, cy + 5));
        break;
    }
}

// ---------- TitleBar ----------

TitleBar::TitleBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("TitleBar"));
    setFixedHeight(kBarHeight);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(0);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setObjectName(QStringLiteral("TitleIcon"));
    m_iconLabel->setPixmap(QPixmap(QStringLiteral(":/icons/app.png"))
                               .scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    layout->addWidget(m_iconLabel);
    layout->addSpacing(6);

    // VSCode 式菜单项（设置 / 工具 / 帮助），由 MainWindow 用 addMenuButton 逐个注入
    layout->addSpacing(6);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName(QStringLiteral("TitleLabel"));
    m_titleLabel->hide(); // 默认不显示标题（VSCode 同款），需要时 setTitle() 打开
    layout->addWidget(m_titleLabel);
    // 弹性占位不挂在隐藏的标题 label 上（隐藏项不占位会把菜单挤到中间），
    // 独立 stretch 保证 [icon][菜单们] 恒靠左、右侧按钮恒靠右
    layout->addStretch(1);

    m_themeBtn = new QToolButton(this);
    m_themeBtn->setObjectName(QStringLiteral("TitleThemeBtn"));
    m_themeBtn->setFixedHeight(kBarHeight); // 热区撑满栏高，点击更好点
    connect(m_themeBtn, &QToolButton::clicked, this, &TitleBar::themeClicked);
    layout->addWidget(m_themeBtn);

    m_minBtn = new CaptionButton(CaptionButton::Minimize, this);
    m_minBtn->setObjectName(QStringLiteral("TitleMinBtn"));
    m_minBtn->setToolTip(tr("最小化"));
    connect(m_minBtn, &QToolButton::clicked, this, &TitleBar::minimizedRequested);
    layout->addWidget(m_minBtn);

    m_maxBtn = new CaptionButton(CaptionButton::Maximize, this);
    m_maxBtn->setObjectName(QStringLiteral("TitleMaxBtn"));
    m_maxBtn->setToolTip(tr("最大化"));
    connect(m_maxBtn, &QToolButton::clicked, this, &TitleBar::maximizedToggled);
    layout->addWidget(m_maxBtn);

    m_closeBtn = new CaptionButton(CaptionButton::Close, this);
    m_closeBtn->setObjectName(QStringLiteral("TitleCloseBtn"));
    m_closeBtn->setToolTip(tr("关闭"));
    connect(m_closeBtn, &QToolButton::clicked, this, &TitleBar::closedRequested);
    layout->addWidget(m_closeBtn);

    updateMaxButtonGlyph();
    // 跟随窗口状态切换 ▢/❐ 图标
    if (window())
        window()->installEventFilter(this);
}

void TitleBar::setTitle(const QString &title)
{
    m_titleLabel->setText(title);
    m_titleLabel->setVisible(!title.isEmpty());
}

QToolButton *TitleBar::addMenuButton(const QString &title)
{
    auto *btn = new QToolButton(this);
    btn->setObjectName(QStringLiteral("TitleMenuBtn"));
    btn->setText(title);
    btn->setFixedHeight(kBarHeight); // 热区撑满栏高
    btn->setPopupMode(QToolButton::InstantPopup);
    // 插到标题文字之前：菜单项永远紧挨应用图标一排，标题始终居右弹性区前
    if (auto *box = qobject_cast<QHBoxLayout *>(layout()))
        box->insertWidget(box->indexOf(m_titleLabel), btn);
    return btn;
}

void TitleBar::setThemeIcon(const QIcon &icon, const QString &tooltip)
{
    m_themeBtn->setIcon(icon);
    m_themeBtn->setToolTip(tooltip);
}

void TitleBar::applyCaptionTheme(bool dark)
{
    const QColor c = CaptionButton::captionForeground(dark);
    m_minBtn->setGlyphColor(c);
    m_maxBtn->setGlyphColor(c);
    m_closeBtn->setGlyphColor(c);
}

void TitleBar::updateMaxButtonGlyph()
{
    const bool max = window() && window()->isMaximized();
    m_maxBtn->setGlyph(max ? CaptionButton::Restore : CaptionButton::Maximize);
    m_maxBtn->setToolTip(max ? tr("向下还原") : tr("最大化"));
}

// ---- 拖动移动窗口（优先走系统拖动，保留手动路径兜底） ----
void TitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOrigin = event->globalPosition().toPoint() - window()->frameGeometry().topLeft();
    }
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton) && window()) {
        // 系统级移动（含 Win11 贴边分屏手势）；失败再手动搬窗口
        if (window()->windowHandle()
            && window()->windowHandle()->startSystemMove()) {
            m_dragging = false;
            return;
        }
        window()->move(event->globalPosition().toPoint() - m_dragOrigin);
    }
    QWidget::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit maximizedToggled();
    QWidget::mouseDoubleClickEvent(event);
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event)
{
    // 窗口状态变化时刷新最大化按钮图标
    if (watched == window() && event->type() == QEvent::WindowStateChange)
        updateMaxButtonGlyph();
    return QWidget::eventFilter(watched, event);
}

void applyWin11RoundedCorners(QWidget *window)
{
#ifdef Q_OS_WIN
    if (WId hwnd = window->winId()) {
        QLibrary dwm(QStringLiteral("dwmapi"));
        if (dwm.load()) {
            using PfnDwmSetWindowAttribute = long (*)(void *, unsigned long, const void *, unsigned long);
            if (auto pfn = reinterpret_cast<PfnDwmSetWindowAttribute>(
                    dwm.resolve("DwmSetWindowAttribute"))) {
                int pref = 2; // DWMWCP_ROUND
                pfn(reinterpret_cast<void *>(hwnd), 33, &pref, sizeof(pref));
            }
        }
    }
#else
    Q_UNUSED(window);
#endif
}
