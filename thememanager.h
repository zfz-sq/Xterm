#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QColor>
#include <QList>
#include <QObject>
#include <QString>

class CommandStore;

/*
 * ThemeDefinition: 主题配置定义，包含终端与界面全套配色。
 */
struct ThemeDefinition
{
    QString id;
    QString name;
    bool isDark = true;

    // 终端配色
    QColor terminalBg;
    QColor terminalFg;
    QColor selectionBg;
    QColor infoColor;
    QColor normal[8];
    QColor bright[8];

    // 界面配色（用于 QSS 动态覆盖注入）
    QColor windowBg;
    QColor panelBg;
    QColor panelBorder;
    QColor inputBg;
    QColor inputBorder;
    QColor accentColor;

    // 界面正文文字色：覆盖 QLabel/列表文字与容器兑底（无显式 color 的部件继承）。
    // dark/light 取基础 QSS 里 QLabel 的原值，内置主题零视觉变化
    QColor textFg;

    // 浮层底色（FramelessDialog 对话框与 QMenu 菜单）：基础 QSS 里两者同源，
    // dark/light 取原值零变化；第三方主题下弹窗/菜单整体跟随
    QColor dialogBg;

    // 状态栏专用：背景与次级文字色。基础 QSS 里写死的只有 dark/light 两套，
    // 第三方主题必须靠这两个值动态覆盖，否则切主题后状态栏仍停留在
    // dark/light 的底色（dark/light 自身取与原 QSS 相同的值，零视觉变化）
    QColor statusBarBg;
    QColor statusBarFg;

    // 按钮四态（普通/悬停/按下）：基础 QSS 里按钮色同样只备 dark/light 两套，
    // 第三方主题靠动态覆盖注入，否则切主题后按钮仍是旧灰。文字色不单设
    //（继承容器 color）；connectBtn 语义色（连接绿/断开红）不覆盖。
    // dark/light 取基础 QSS 原值，内置主题零视觉变化
    QColor buttonBg;
    QColor buttonBorder;
    QColor buttonHoverBg;
    QColor buttonHoverBorder;
    QColor buttonPressedBg;

    // 按钮禁用态（底/边框/文字）：基础 QSS 同样只备 dark/light 两套，第三方
    // 主题靠动态覆盖，否则 SFTP 文件管理未选中时整排禁用面板钮（下载/删除/
    // 重命名等）仍是旧灰黑。connectBtn 语义绿仍由专属规则保留不覆盖。
    // dark/light 取基础 QSS 原值，内置主题零视觉变化
    QColor buttonDisabledBg;
    QColor buttonDisabledBorder;
    QColor buttonDisabledFg;

    // 通用 QToolButton 的选中态（终端/调试切换等 checkable 工具钮）：
    // 基础 QSS 只有 dark（VSCode 蓝底）/light（淡蓝底）两套，第三方主题
    // 靠动态覆盖；dark/light 取原值，内置主题零视觉变化
    QColor toolCheckedBg;
    QColor toolCheckedBorder;

    // 列表/树/表格（SFTP 文件浏览器、传输队列、命令设置列表）：
    // 基础 QSS 只备 dark/light 两套，第三方主题靠动态覆盖，否则 SFTP
    // 文件区/表头/选中行仍停留在旧灰。dark/light 取基础 QSS 原值零变化
    QColor listBg;         // 列表背景
    QColor listAltBg;      // 斑马纹交替行
    QColor listBorder;     // 列表边框与表头分隔线
    QColor listHoverBg;    // 条目悬停
    QColor listSelectedBg; // 条目选中背景
    QColor listSelectedFg; // 条目选中文字
    QColor headerBg;       // 表头背景
    QColor headerFg;       // 表头文字
    QColor gridlineColor;  // 表格网格线

    // 滚动条滑块两态（常态/悬停吸附）：6px 细轨道+同宽全幅滑块的配色随主题，
    // Termius 主题用户钦定 #285049→悬停 #21B568；dark/light 取基础 QSS 原值
    QColor scrollHandleBg;
    QColor scrollHandleHoverBg;

    // 滚动条浮现态轨道色：支持半透明（内置主题用 5% 黑/白叠加，Termius 实色 #2C2F3F）
    QColor scrollTrackBg;
};

/*
 * ThemeManager: 主题管理协作类（MainWindow 拆分 M1.a）。
 *
 * 职责：
 * - 多主题管理与切换（状态保存在 CommandStore）
 * - 应用全局基础 QSS 并动态注入当前主题的窗口/面板/强调色
 * - 同步图标主题（setIconThemeDark）与无边框对话框主题提示
 * - 为终端及 UI 各部件提供当前主题配色
 */
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    explicit ThemeManager(CommandStore *store, QObject *parent = nullptr);

    // 静态内置主题库
    static const QList<ThemeDefinition> &availableThemes();
    static ThemeDefinition themeById(const QString &id);

    // 当前主题信息与 ID
    ThemeDefinition currentTheme() const;
    QString currentThemeId() const;

    // 切换到指定 ID 的主题
    void setTheme(const QString &id);

    // 当前是否深色主题（状态源是 CommandStore）
    bool isDark() const;

    // 快速在暗色/亮色之间翻转
    void toggleTheme();

    // 立即应用全套主题
    void applyNow();

signals:
    // 主题切换后发出
    void themeChanged(bool dark);
    void themeUpdated(const ThemeDefinition &theme);

private:
    CommandStore *m_store = nullptr; // 主题状态源（不拥有）
};

#endif // THEMEMANAGER_H
