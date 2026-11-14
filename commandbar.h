#ifndef COMMANDBAR_H
#define COMMANDBAR_H

#include <QList>
#include <QMap>
#include <QStringList>
#include <QWidget>

#include "commandstore.h"
#include "thememanager.h" // ThemeDefinition：主题页列表项数据

class QLabel;
class QKeyEvent;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTextEdit;
class QToolButton;
class QVBoxLayout;

/*
 * CommandBar: 终端右侧的多功能侧边栏（Termius 式悬浮卡片，用户定稿 2026-11-14）。
 *
 * 顶部一行图标标签切换三页（后续可再扩）:
 *   1. 命令页  — 预设命令列表（可编辑，直接点击发送；按钮列形态已删）
 *   2. 主题页  — Termius 式主题列表（缩略图 + 名称，点击切换，当前项高亮）
 *   3. MCP 页  — 原「MCP 提示词」弹窗内容（说明/提示词/配置 JSON/复制）
 *
 * 显隐由标题栏侧边栏按钮（TitleBar::sidebarToggled）控制：收起后终端铺满窗口；
 * 原三态中的「收起成侧边按钮组」形态已废弃。tab 图标行常驻卡片顶部。
 *
 * 命令列表每行: 序号 | 命令文本 | 发送按钮 | 拖动手柄(可拖动排序)
 * 底部右侧: 复制 | +添加 | -删除 | 上移 | 下移
 *
 * 三页统一页宽（用户定稿 2026-11-14）：切换标签时卡片宽度恒定，
 * 不再随页内容变来变去（以主题页宽度为准，kPageWidth 常量一处调全同步）。
 */
class CommandBar : public QWidget
{
    Q_OBJECT

public:
    explicit CommandBar(QWidget *parent = nullptr);

    // 浮层宽度自管：三页统一页宽（用户定稿：以主题页宽度为准，kPageWidth 一处调全
    // 同步）——切标签卡片宽度恒定不变，高度仍按当前页随内容伸缩
    QSize sizeHint() const override;

    void setCommands(const QList<CommandItem> &commands);
    QList<CommandItem> commands() const;
    void setSendEnabled(bool enabled);
    void applyTheme(); // 按当前图标主题重刷本栏图标

    // 主题页数据：注入主题清单与当前主题 ID（UiBuilder 从 ThemeManager 取，解耦）
    void setThemes(const QList<ThemeDefinition> &themes);
    void setCurrentTheme(const QString &themeId); // 主题切换后同步高亮（幂等）

    // MCP 页内容：提示词全文 + 配置 JSON（UiBuilder 从 McpMainHook 侧数据生成）
    void setMcpContent(const QString &prompt, const QString &config);

signals:
    void commandRequested(const QString &text); // 按下任一发送入口
    void commandsChanged();                     // 命令列表内容/顺序变化
    void themeRequested(const QString &themeId); // 主题页点击某项（UiBuilder 接 setTheme）

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct Row
    {
        QListWidgetItem *item = nullptr;
        QWidget *widget = nullptr;
        QLineEdit *edit = nullptr;
        QPushButton *sendBtn = nullptr;
        QLabel *idxLabel = nullptr;
        QLabel *gripLabel = nullptr;
    };

    // 被删命令的恢复信息:原行号 + 命令文本(Ctrl+Z 撤销删除用)
    struct RemovedCmd
    {
        int row = 0;
        QString text;
    };

    void buildUi();
    void buildCommandPage();
    void buildThemePage();
    void buildMcpPage();
    void rebuildThemeList();   // setThemes 后重建主题列表（含缩略图）
    void switchTab(int index); // 切换标签页并同步按钮选中态与卡片宽度
    void syncFloatingWidth();  // 页显隐后自适应浮层宽度（脱离布局后没人替它 resize）
    void rebuildRows(const QStringList &texts, int selectRow);
    void relabelRows();
    QStringList currentTexts() const;
    Row *rowAt(int i) const;

    void onAddClicked();
    void onRemoveClicked();
    void undoRemove(); // Ctrl+Z: 恢复最近一次删除的命令
    void onCopyClicked();
    void onMoveUpClicked();
    void onMoveDownClicked();
    void onRowsReordered();

    // ---- 标签行与三页容器 ----
    QWidget *m_tabRow = nullptr;
    QList<QToolButton *> m_tabBtns;
    QStackedWidget *m_pages = nullptr;
    QWidget *m_commandPage = nullptr;
    QWidget *m_themePage = nullptr;
    QWidget *m_mcpPage = nullptr;

    // ---- 命令页（原设置页形态，用户定稿 2026-11-14：按钮列形态整体删除） ----
    QListWidget *m_list = nullptr;
    QToolButton *m_copyBtn = nullptr;
    QToolButton *m_addBtn = nullptr;
    QToolButton *m_delBtn = nullptr;
    QToolButton *m_upBtn = nullptr;
    QToolButton *m_downBtn = nullptr;

    // ---- 主题页 ----
    QListWidget *m_themeList = nullptr;
    QList<ThemeDefinition> m_themes;
    QString m_currentThemeId;
    bool m_themeRebuilding = false; // 重建列表时抑制 currentItemChanged 信号外发

    // ---- MCP 页 ----
    QTextEdit *m_mcpPrompt = nullptr;      // 说明/提示词：富文本布局，块行距才能生效
    QPlainTextEdit *m_mcpConfig = nullptr; // JSON 配置

    QMap<QListWidgetItem *, Row *> m_rows;
    bool m_sendEnabled = false;
    bool m_rebuilding = false;
    QList<RemovedCmd> m_undoStack; // 删除历史（后删的先恢复）；setCommands 整表重载时清空
};

#endif // COMMANDBAR_H
