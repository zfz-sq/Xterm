#ifndef UIBUILDER_H
#define UIBUILDER_H

#include <QObject>

#include "commandstore.h" // QList<KeywordRule>

class CommandBar;
class HotkeyManager;
class KmsgMuteToggle;
class QLabel;
class QMainWindow;
class McpMainHook;
class QComboBox;
class ScreenView;
class SessionController;
class ThemeManager;
class TitleBar;

namespace Ui {
class MainWindow;
} // namespace Ui

/*
 * UiBuilder: 主窗口 UI 编排协作类（MainWindow 拆分 M1.d）。
 *
 * 职责：
 * - 搭建主窗口骨架：自定义标题栏（含设置/工具/帮助菜单与关于对话框）、
 *   图标、串口/SSH 参数控件初始化、终端两页、命令栏、状态栏常驻部件
 *   （连接状态、XOFF 提示、MCP 标签挂载、内核日志静音开关）
 * - 模式/连接状态的控件级刷新（updateModeUi / updateConnectionUi /
 *   updatePersistentStatus），由 SessionController 的信号驱动
 * - 主题级联刷新（onThemeToggled）、窗口留白（applyWindowMargins）
 * - 设置菜单对话框：关键词高亮、终端字体；关键词规则落到两页终端
 *
 * 边界：只做控件编排与刷新，会话决策一律经 SessionController；
 * 依赖 SessionController 的接线（参数控件→会话槽、快捷键行为、状态刷新）
 * 在 setSessionController() 中完成（创建顺序：本类先于 SessionController）。
 * 持有 QMainWindow 仅用于窗口级操作（menuWidget/windowTitle/windowState），
 * 不反向调用主窗口业务。
 */
class UiBuilder : public QObject
{
    Q_OBJECT

public:
    explicit UiBuilder(QMainWindow *window, Ui::MainWindow *ui, CommandStore *store,
                       ThemeManager *theme, HotkeyManager *hotkey, McpMainHook *mcp,
                       QObject *parent = nullptr);

    // SessionController 创建后注入：参数控件/快捷键接线 + 状态刷新接线
    void setSessionController(SessionController *session);

    // 供主窗口接线与挂载的部件（内容/可见性由本类管理）
    ScreenView *serialView() const { return m_serialView; }
    ScreenView *sshView() const { return m_sshView; }
    CommandBar *commandBar() const { return m_commandBar; }
    TitleBar *titleBar() const { return m_titleBar; }
    KmsgMuteToggle *kmsgButton() const { return m_kmsgBtn; }

    // —— 状态刷新（SessionController 信号驱动；主题级联也走这里）——
    void updateModeUi();
    void updateConnectionUi();
    void updatePersistentStatus();
    void onThemeToggled(bool dark);
    void applyWindowMargins(bool maximized);

    // —— 设置菜单与关键词 ——
    void openKeywordSettings();  // 关键词高亮编辑（表格，改即预览）
    void openFontSettings();     // 终端字体族 + 字号（改即预览，两页同步）
    void applyKeywords(const QList<KeywordRule> &rules); // 落存储并同步两页

    // —— 启动恢复（store load 之后由主窗口调用）——
    void restoreTerminalFontSettings();

    // SSH 端口下拉的固定项与「多出的项」清理（SessionController 回填参数时共用）
    static const QStringList &fixedSshPorts();
    static void pruneCustomPortItems(QComboBox *combo);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupTitleBar();
    void setupIcons();
    void setupConnectionCombos();
    void setupSshInputs();
    void fitSshPortEdit(); // 端口编辑框拉到箭头前，避免数字被裁、中间留白
    void setupTerminal();
    void setupStatusBar();
    void onHighlightRequested(const QString &word); // 终端选中词右键「高亮此词」

    QMainWindow *m_window = nullptr; // 仅窗口级操作，见类注释
    Ui::MainWindow *m_ui = nullptr;
    CommandStore *m_store = nullptr;
    ThemeManager *m_theme = nullptr;
    HotkeyManager *m_hotkey = nullptr;
    McpMainHook *m_mcp = nullptr;
    SessionController *m_session = nullptr; // setSessionController 注入（不拥有）
    TitleBar *m_titleBar = nullptr;
    CommandBar *m_commandBar = nullptr;
    ScreenView *m_serialView = nullptr;
    ScreenView *m_sshView = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_xoffLabel = nullptr; // XOFF 提示：发出 Ctrl+S 亮、Ctrl+Q 灭
    KmsgMuteToggle *m_kmsgBtn = nullptr;
    QString m_shellTitle; // SSH 视图标题（OSC 0/2），窗口标题跟随
};

#endif // UIBUILDER_H
