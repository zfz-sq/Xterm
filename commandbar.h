#ifndef COMMANDBAR_H
#define COMMANDBAR_H

#include <QList>
#include <QMap>
#include <QStringList>
#include <QWidget>

#include "commandstore.h"

class QLabel;
class QKeyEvent;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

/*
 * CommandBar: 终端右侧的预设命令栏。
 *
 * 三种形态:
 *   1. 按钮 + 设置页 (点击齿轮展开编辑面板)
 *   2. 仅按钮列      (设置页收起)
 *   3. 仅侧边按钮组  (整栏收起，顶部是“展开命令栏”按钮，下方可经 addSideButton()
 *                     挂外部功能按钮，如 MCP；命令栏任一形态展开时整组隐藏)
 *
 * 设置页每行: 序号 | 命令文本 | 发送按钮 | 拖动手柄(可拖动排序)
 * 底部右侧: 复制 | +添加 | -删除 | 上移 | 下移
 */
class CommandBar : public QWidget
{
    Q_OBJECT

public:
    explicit CommandBar(QWidget *parent = nullptr);

    void setCommands(const QList<CommandItem> &commands);
    QList<CommandItem> commands() const;
    void setSendEnabled(bool enabled);
    void applyTheme(); // 按当前图标主题重刷本栏图标

    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed); // 程序化设置收起状态（启动恢复用，不发信号）

    // 把外部功能按钮（如 MCP）挂到收起态侧边按钮组：位于“展开命令栏”按钮下方，
    // 命令栏展开时整组隐藏。按钮统一为纯图标 28×28，与“展开命令栏”按钮一致。
    void addSideButton(QToolButton *btn);

signals:
    void commandRequested(const QString &text); // 按下任一发送入口
    void commandsChanged();                     // 命令列表内容/顺序变化
    void collapsedChanged(bool collapsed);      // 用户切换整栏收起/展开时发出

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
    void rebuildRows(const QStringList &texts, int selectRow);
    void syncCommandButtons();
    void relabelRows();
    QStringList currentTexts() const;
    Row *rowAt(int i) const;

    void onAddClicked();
    void onRemoveClicked();
    void undoRemove(); // Ctrl+Z: 恢复最近一次删除的命令(设置页展开时)
    void onCopyClicked();
    void onMoveUpClicked();
    void onMoveDownClicked();
    void onRowsReordered();

    QWidget *m_buttonColumn = nullptr;
    QToolButton *m_gearBtn = nullptr;
    QToolButton *m_collapseBtn = nullptr;
    QScrollArea *m_scroll = nullptr;
    QVBoxLayout *m_buttonLayout = nullptr;
    QLabel *m_emptyHint = nullptr;

    QWidget *m_editPanel = nullptr;
    QListWidget *m_list = nullptr;
    QWidget *m_sideRail = nullptr;       // 收起态侧边按钮组（展开命令栏按钮 + 外挂功能按钮）
    QVBoxLayout *m_sideRailLayout = nullptr;
    QToolButton *m_restoreBtn = nullptr;
    QToolButton *m_panelCollapseBtn = nullptr;
    QToolButton *m_copyBtn = nullptr;
    QToolButton *m_addBtn = nullptr;
    QToolButton *m_delBtn = nullptr;
    QToolButton *m_upBtn = nullptr;
    QToolButton *m_downBtn = nullptr;

    QList<QPushButton *> m_commandButtons;
    QMap<QListWidgetItem *, Row *> m_rows;
    bool m_sendEnabled = false;
    bool m_rebuilding = false;
    bool m_collapsed = false; // 整栏是否收起成侧边按钮组（供持久化，isVisible 在窗口未显示时不可靠）
    QList<RemovedCmd> m_undoStack; // 删除历史（后删的先恢复）；setCommands 整表重载时清空
};

#endif // COMMANDBAR_H
