#ifndef TITLEBAR_H
#define TITLEBAR_H

#include <QColor>
#include <QToolButton>
#include <QWidget>

class QLabel;
class QMenu;
class QPaintEvent;

// 标题栏系统按钮: 用 QPainter 画 Win11 风格字形（— □ ❐ ✕），不依赖字体渲染
class CaptionButton : public QToolButton
{
public:
    enum Glyph { Minimize, Maximize, Restore, Close };
    explicit CaptionButton(Glyph glyph, QWidget *parent = nullptr);

    void setGlyph(Glyph glyph) { m_glyph = glyph; update(); }
    void setGlyphColor(const QColor &color) { m_color = color; update(); }
    // 标题栏字形前景色（主题深浅决定），主窗口与对话框标题栏共用
    static QColor captionForeground(bool dark)
    {
        return dark ? QColor(0xCC, 0xCC, 0xCC) : QColor(0x24, 0x29, 0x2E);
    }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Glyph m_glyph;
    QColor m_color{0x24, 0x29, 0x2E}; // 默认亮色主题前景，applyCaptionTheme 会覆盖
};

// 自定义标题栏（无边框窗口）。
// [应用图标(点击弹主菜单)] [连接参数组+连接/文件/模式按钮] [状态栏开关] [侧边栏] [—] [□] [✕]
// 文字菜单按钮已整体移除（用户定稿 2026-11-15：设置/关于全部集成到左上角图标）；
// 主题按钮同日被状态栏开关替换（用户定稿 2026-11-16：深浅快切不再保留，主题页是唯一入口）；
// 连接参数栏运行时 reparent 进中段（同日定稿，栏高回调 40px 容纳 28px 参数控件）。
// 依赖 objectName 供全局 QSS 主题化（style-dark / style-light 里的 #TitleBar 规则）。
class TitleBar : public QWidget
{
    Q_OBJECT

public:
    explicit TitleBar(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setConnectionBar(QWidget *bar);          // 连接参数栏整条嵌入标题栏中段（运行时 reparent）
    void setSidebarShown(bool shown);                // 程序化同步侧边栏状态（启动恢复，不发信号）
    void applyCaptionTheme(bool dark);                // 系统按钮字形颜色随主题切换
    void setMaximizedShown(bool maximized);           // 手动驱动最大化字形/提示（伪最大化不触发 WindowStateChange）

signals:
    void statusbarToggleRequested();     // 底部状态栏展开/收起请求，由 UiBuilder 接线翻转显隐
    void logoClicked();                  // 左上角应用图标被点击：弹出主菜单（设置+关于集成）
    void sidebarToggled(bool shown);     // 侧边栏展开/收起（命令栏卡片显隐），由 UiBuilder 接线
    void minimizedRequested();
    void maximizedToggled();             // 最大化 <-> 还原
    void closedRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateMaxButtonGlyph();         // 跟随窗口状态切换 □ / ❐

    QLabel *m_iconLabel = nullptr;
    QWidget *m_connBar = nullptr;         // 连接参数栏（嵌入标题栏中段，空白区仍可拖窗口）
    QLabel *m_titleLabel = nullptr;
    QToolButton *m_statusBtn = nullptr;  // 底部状态栏展开/收起（原主题按钮位，2026-11-16 替换）
    QToolButton *m_sidebarBtn = nullptr; // 侧边栏展开/收起（状态栏按钮右侧，Termius 同位）
    CaptionButton *m_minBtn = nullptr;
    CaptionButton *m_maxBtn = nullptr;
    CaptionButton *m_closeBtn = nullptr;
    bool m_dragging = false;
    QPoint m_dragOrigin;
    bool m_sidebarShown = true; // 侧边栏展开态（按钮不再用 checked 态，避免常亮高亮）
};

// Win11 系统级窗口圆角（DwmSetWindowAttribute DWMWA_WINDOW_CORNER_PREFERENCE=33,
// DWMWCP_ROUND=2 / DONOTROUND=1），主窗口与 FramelessDialog 共用。QLibrary 动态解析免改链接配置，
// Win10 / 非 Windows 上静默跳过。rounded=false 用于伪最大化期间禁用：窗口非系统
// zoomed 态，DWM 仍按普通窗口绘制圆角，四角透出桌面（实测复现）。
void setWin11RoundedCorners(QWidget *window, bool rounded);
inline void applyWin11RoundedCorners(QWidget *window) { setWin11RoundedCorners(window, true); }

#endif // TITLEBAR_H
