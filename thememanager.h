#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>

class CommandStore;

/*
 * ThemeManager: 主题切换协作类（MainWindow 拆分 M1.a）。
 *
 * 职责：
 * - 深色/亮色状态的读取与切换（状态存在 CommandStore，落盘由 CommandStore 负责）
 * - 应用全局 QSS（:/style-dark.qss / :/style-light.qss）
 * - 同步图标主题（setIconThemeDark）与无边框对话框主题提示
 *
 * 级联刷新（终端两页、图标、命令栏、MCP 按钮等依赖 MainWindow 持有的部件）
 * 通过 themeChanged 信号回抛给 MainWindow 编排，本类不反向持有主窗口。
 * 后续 M1.b–M1.d 的协作类沿用这一「窄职责 + 信号回抛」模式。
 */
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    explicit ThemeManager(CommandStore *store, QObject *parent = nullptr);

    // 当前是否深色主题（状态源是 CommandStore）
    bool isDark() const;

    // 翻转主题并广播 themeChanged；落盘由调用方（切换入口）负责。
    // 主题切换入口在自定义标题栏，图标显示"将要切换到"的主题。
    void toggleTheme();

    // 立即应用全套主题：图标主题 → 全局 QSS → 对话框主题提示。
    // 初启在 m_store->load() 之后调用一次；此后一律经 toggleTheme 广播。
    void applyNow();

signals:
    // toggleTheme 之后发出；MainWindow 接线做级联刷新（终端页/图标/状态栏）。
    void themeChanged(bool dark);

private:
    CommandStore *m_store = nullptr; // 主题状态源（不拥有）
};

#endif // THEMEMANAGER_H
