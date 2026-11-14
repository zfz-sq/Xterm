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
constexpr int kBarHeight = 30;   // 标题栏高度保持原样（用户定稿 2026-11-14 四轮：不加高）
constexpr int kBtnHeight = 28;   // 三类按钮统一 28×28 正方形（热区够大且栏内上下各留 1px）
constexpr int kCaptionWidth = 28; // 系统按钮同尺寸正方形（用户定稿：最大化等也一样大）
} // namespace

// ---------- CaptionButton: 矢量画 Win11 风格系统按钮字形 ----------

CaptionButton::CaptionButton(Glyph glyph, QWidget *parent)
    : QToolButton(parent)
    , m_glyph(glyph)
{
    setFixedSize(kCaptionWidth, kBtnHeight);
    setFocusPolicy(Qt::NoFocus);
}

void CaptionButton::paintEvent(QPaintEvent *event)
{
    // QSS 负责底色（hover 灰 / pressed 深灰 / 关闭键红），这里只叠字形
    QToolButton::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal cx = rect().center().x();
    // 与菜单文字同幅下移 2px（对应 QSS 里 #TitleBar QToolButton 的 padding-top: 4px），保持视觉基线一致
    const qreal cy = rect().center().y() + 2;

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
                               .scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
    // 正方形居中不贴栏边（用户定稿 2026-11-14 四轮：28×28 统一尺寸）：hover 圆角块才是按钮形状
    m_themeBtn->setFixedSize(kBtnHeight, kBtnHeight);
    connect(m_themeBtn, &QToolButton::clicked, this, &TitleBar::themeClicked);
    layout->addWidget(m_themeBtn);

    // 侧边栏切换（Termius 同位：主题按钮右侧）：命令栏卡片已升级为多功能侧边栏
    // （命令/主题/MCP 三页），此按钮控制其展开/收起，收起后终端铺满窗口；
    // 不用 checked 态（用户定稿：侧边栏开着时按钮常亮不协调），状态只存在 m_sidebarShown
    m_sidebarBtn = new QToolButton(this);
    m_sidebarBtn->setObjectName(QStringLiteral("TitleSidebarBtn"));
    m_sidebarBtn->setFixedSize(kBtnHeight, kBtnHeight);
    m_sidebarBtn->setToolTip(tr("展开 / 收起侧边栏"));
    connect(m_sidebarBtn, &QToolButton::clicked, this, [this] {
        m_sidebarShown = !m_sidebarShown;
        emit sidebarToggled(m_sidebarShown);
    });
    layout->addWidget(m_sidebarBtn);

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
    btn->setFixedHeight(kBtnHeight); // 与图标/系统按钮同高，栏内居中不贴边（hover 圆角块统一）
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

void TitleBar::setSidebarShown(bool shown)
{
    m_sidebarShown = shown; // 只同步状态不发信号（启动恢复）；按钮无常亮选中态
}

// 侧边栏按钮字形色随主题（QPainter 画面板图标：外框+右侧面板列，Termius 同款）
void TitleBar::applyCaptionTheme(bool dark)
{
    const QColor c = CaptionButton::captionForeground(dark);
    m_minBtn->setGlyphColor(c);
    m_maxBtn->setGlyphColor(c);
    m_closeBtn->setGlyphColor(c);

    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(c, 1.4);
    pen.setCapStyle(Qt::FlatCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(2.0, 3.0, 16.0, 14.0), 3.0, 3.0); // 外框
    p.drawLine(QLineF(12.7, 3.7, 12.7, 16.3));                  // 主区|侧列分隔线
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRect(QRectF(14.3, 6.0, 2.0, 2.4)); // 侧列内容小块
    p.drawRect(QRectF(14.3, 10.2, 2.0, 2.4));
    p.end();
    m_sidebarBtn->setIcon(QIcon(pm));
}

// 按钮字形/提示跟随最大化态：伪最大化路径（UiBuilder::toggleMaximized）不触发
// WindowStateChange，eventFilter 拿不到通知，由 UiBuilder 手动调这里同步
void TitleBar::setMaximizedShown(bool maximized)
{
    m_maxBtn->setGlyph(maximized ? CaptionButton::Restore : CaptionButton::Maximize);
    m_maxBtn->setToolTip(maximized ? tr("向下还原") : tr("最大化"));
}

void TitleBar::updateMaxButtonGlyph()
{
    setMaximizedShown(window() && window()->isMaximized());
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
