#ifndef COMMANDBAR_H
#define COMMANDBAR_H

#include <QList>
#include <QMap>
#include <QStringList>
#include <QWidget>

#include "commandstore.h"

class QLabel;
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
 *   3. 仅扩展按钮    (整栏收起，只留一个齿轮按钮，点击还原)
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

signals:
    void commandRequested(const QString &text); // 按下任一发送入口
    void commandsChanged();                     // 命令列表内容/顺序变化

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

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

    void buildUi();
    void rebuildRows(const QStringList &texts, int selectRow);
    void syncCommandButtons();
    void relabelRows();
    QStringList currentTexts() const;
    Row *rowAt(int i) const;

    void onAddClicked();
    void onRemoveClicked();
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
};

#endif // COMMANDBAR_H
