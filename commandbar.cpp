#include "commandbar.h"

#include "icons.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDrag>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kRowRole = Qt::UserRole + 1; // 存 Row* 指针

// 拖动排序时，QListWidget 默认用代理把整行渲染成跟随光标的缩略图；但本列表的
// 内容都在 setItemWidget 的自定义控件里，代理渲染不出东西，于是缩略图是一块
// 空白矩形，拖到哪都盖住下面的行，看不清落点。这里只把缩略图换成 1×1 透明图，
// 排序逻辑与 Qt 原生的插入位置指示线（那条"光标"）完全保持不变。
class DragListWidget : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        const QModelIndexList indexes = selectionModel()->selectedIndexes();
        if (indexes.isEmpty())
            return;
        QMimeData *mime = model()->mimeData(indexes); // 内部移动用的 mime，保证仍能原序搬移
        if (!mime)
            return;
        QDrag drag(this);
        drag.setMimeData(mime);
        QPixmap blank(1, 1);
        blank.fill(Qt::transparent);
        drag.setPixmap(blank);
        drag.setHotSpot(QPoint(0, 0));
        drag.exec(supportedActions);
    }
};
} // namespace

CommandBar::CommandBar(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    rebuildRows(QStringList{}, -1);
}

void CommandBar::buildUi()
{
    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(4);

    // ---------- 按钮列 ----------
    m_buttonColumn = new QWidget(this);
    m_buttonColumn->setFixedWidth(150);
    auto *colLayout = new QVBoxLayout(m_buttonColumn);
    colLayout->setContentsMargins(0, 0, 0, 0);
    colLayout->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setSpacing(2);
    m_gearBtn = new QToolButton(m_buttonColumn);
    m_gearBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    m_gearBtn->setToolTip(tr("展开 / 收起命令设置页"));
    m_gearBtn->setAutoRaise(true);
    m_collapseBtn = new QToolButton(m_buttonColumn);
    m_collapseBtn->setIcon(themedIcon(QStringLiteral("chevron_right.png")));
    m_collapseBtn->setToolTip(tr("收起命令栏"));
    m_collapseBtn->setAutoRaise(true);
    header->addWidget(m_gearBtn);
    header->addStretch();
    header->addWidget(m_collapseBtn);
    colLayout->addLayout(header);

    m_scroll = new QScrollArea(m_buttonColumn);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    auto *buttonHost = new QWidget(m_scroll);
    m_buttonLayout = new QVBoxLayout(buttonHost);
    m_buttonLayout->setContentsMargins(0, 0, 0, 0);
    m_buttonLayout->setSpacing(4);
    m_emptyHint = new QLabel(tr("（无预设命令）"), buttonHost);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setStyleSheet(QStringLiteral("color: #9E9E9E;"));
    m_buttonLayout->addWidget(m_emptyHint);
    m_buttonLayout->addStretch(1);
    m_scroll->setWidget(buttonHost);
    colLayout->addWidget(m_scroll, 1);

    // ---------- 设置页 ----------
    m_editPanel = new QWidget(this);
    m_editPanel->setFixedWidth(340);
    m_editPanel->hide(); // 默认收起，仅显示按钮列
    auto *panelLayout = new QVBoxLayout(m_editPanel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(4);

    auto *title = new QLabel(tr("命令设置（拖动手柄排序）"), m_editPanel);
    title->setStyleSheet(QStringLiteral("font-weight: 600;"));

    // 设置页左上角的收起按钮: 回到按钮列（齿轮在按钮列里，藏掉后靠它关回）
    m_panelCollapseBtn = new QToolButton(m_editPanel);
    m_panelCollapseBtn->setIcon(themedIcon(QStringLiteral("chevron_left.png")));
    m_panelCollapseBtn->setToolTip(tr("收起命令设置页"));
    m_panelCollapseBtn->setAutoRaise(true);

    auto *panelHeader = new QHBoxLayout;
    panelHeader->setSpacing(2);
    panelHeader->addWidget(m_panelCollapseBtn);
    panelHeader->addWidget(title, 1);
    panelLayout->addLayout(panelHeader);

    m_list = new DragListWidget(m_editPanel);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDropIndicatorShown(true); // 只保留插入位置指示线
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setAlternatingRowColors(true);
    panelLayout->addWidget(m_list, 1);

    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(2);
    bottom->addStretch(1);
    auto mkBtn = [this, bottom](const QString &ico, const QString &tip) {
        auto *b = new QToolButton(m_editPanel);
        b->setIcon(themedIcon(ico));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setIconSize(QSize(20, 20));
        bottom->addWidget(b);
        return b;
    };
    m_copyBtn = mkBtn(QStringLiteral("copy.png"), tr("复制选中命令"));
    m_addBtn = mkBtn(QStringLiteral("add.png"), tr("添加命令"));
    m_delBtn = mkBtn(QStringLiteral("remove.png"), tr("删除选中命令"));
    m_upBtn = mkBtn(QStringLiteral("up.png"), tr("上移"));
    m_downBtn = mkBtn(QStringLiteral("down.png"), tr("下移"));
    panelLayout->addLayout(bottom);

    // ---------- 完全收起后的侧边按钮组 ----------
    // 收起态整栏只剩这一竖排：顶部是“展开命令栏”按钮，下方经 addSideButton()
    // 挂外部功能按钮（如 MCP）；命令栏任一形态展开时整组隐藏。
    m_sideRail = new QWidget(this);
    m_sideRail->setFixedWidth(28);
    m_sideRail->hide();
    m_sideRailLayout = new QVBoxLayout(m_sideRail);
    m_sideRailLayout->setContentsMargins(0, 0, 0, 0);
    m_sideRailLayout->setSpacing(4);

    m_restoreBtn = new QToolButton(m_sideRail);
    m_restoreBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    m_restoreBtn->setToolTip(tr("展开命令栏"));
    m_restoreBtn->setAutoRaise(true);
    m_restoreBtn->setFixedSize(28, 28);
    m_sideRailLayout->addWidget(m_restoreBtn);
    m_sideRailLayout->addStretch(1); // 按钮贴顶，不被垂直居中

    rootLayout->addWidget(m_buttonColumn);
    rootLayout->addWidget(m_editPanel);
    // 全部收起后只剩侧边按钮组: 固定贴顶，避免被布局垂直居中
    rootLayout->addWidget(m_sideRail, 0, Qt::AlignTop | Qt::AlignLeft);

    // ---------- 三态收起 ----------
    // 展开设置页时隐藏按钮列（设置页自带收起按钮），收起时恢复按钮列
    connect(m_gearBtn, &QToolButton::clicked, this, [this] {
        const bool show = !m_editPanel->isVisible();
        m_editPanel->setVisible(show);
        m_buttonColumn->setVisible(!show);
    });
    connect(m_panelCollapseBtn, &QToolButton::clicked, this, [this] {
        m_editPanel->hide();
        m_buttonColumn->show();
    });
    connect(m_collapseBtn, &QToolButton::clicked, this, [this] {
        setCollapsed(true);
        emit collapsedChanged(true);
    });
    connect(m_restoreBtn, &QToolButton::clicked, this, [this] {
        setCollapsed(false);
        emit collapsedChanged(false);
    });

    // ---------- 增删复制排序 ----------
    connect(m_addBtn, &QToolButton::clicked, this, &CommandBar::onAddClicked);
    connect(m_delBtn, &QToolButton::clicked, this, &CommandBar::onRemoveClicked);
    connect(m_copyBtn, &QToolButton::clicked, this, &CommandBar::onCopyClicked);
    connect(m_upBtn, &QToolButton::clicked, this, &CommandBar::onMoveUpClicked);
    connect(m_downBtn, &QToolButton::clicked, this, &CommandBar::onMoveDownClicked);

    // 拖动排序完成（InternalMove 触发 model 的 rowsMoved / layoutChanged）
    connect(m_list->model(), &QAbstractItemModel::rowsMoved,
            this, &CommandBar::onRowsReordered, Qt::DirectConnection);
    connect(m_list->model(), &QAbstractItemModel::layoutChanged,
            this, &CommandBar::onRowsReordered, Qt::DirectConnection);
}

void CommandBar::applyTheme()
{
    m_gearBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    m_collapseBtn->setIcon(themedIcon(QStringLiteral("chevron_right.png")));
    m_panelCollapseBtn->setIcon(themedIcon(QStringLiteral("chevron_left.png")));
    m_restoreBtn->setIcon(themedIcon(QStringLiteral("gear.png")));
    m_copyBtn->setIcon(themedIcon(QStringLiteral("copy.png")));
    m_addBtn->setIcon(themedIcon(QStringLiteral("add.png")));
    m_delBtn->setIcon(themedIcon(QStringLiteral("remove.png")));
    m_upBtn->setIcon(themedIcon(QStringLiteral("up.png")));
    m_downBtn->setIcon(themedIcon(QStringLiteral("down.png")));

    for (Row *row : m_rows) {
        if (!row)
            continue;
        row->sendBtn->setIcon(themedIcon(QStringLiteral("send.png")));
        row->gripLabel->setPixmap(themedIcon(QStringLiteral("grip.png")).pixmap(16, 16));
    }
}

void CommandBar::addSideButton(QToolButton *btn)
{
    if (!btn || !m_sideRailLayout)
        return;
    // 统一成与“展开命令栏”按钮一致的纯图标 28×28 小方块，
    // 插到末尾 stretch 之前（还原按钮下方），随整组一起显隐。
    btn->setParent(m_sideRail);
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn->setAutoRaise(true);
    btn->setFixedSize(28, 28);
    m_sideRailLayout->insertWidget(m_sideRailLayout->count() - 1, btn);
}

void CommandBar::setCollapsed(bool collapsed)
{
    if (collapsed) {
        m_buttonColumn->hide();
        m_editPanel->hide();
        m_sideRail->show();
    } else {
        // 恢复到默认形态：按钮列（设置页若曾展开也不再恢复）
        m_sideRail->hide();
        m_editPanel->hide();
        m_buttonColumn->show();
    }
    m_collapsed = collapsed;
}

// ---------- 对外接口 ----------

void CommandBar::setCommands(const QList<CommandItem> &commands)
{
    QStringList texts;
    texts.reserve(commands.size());
    for (const CommandItem &c : commands)
        texts << c.text;
    rebuildRows(texts, texts.isEmpty() ? -1 : 0);
}

QList<CommandItem> CommandBar::commands() const
{
    QList<CommandItem> out;
    const QStringList texts = currentTexts();
    out.reserve(texts.size());
    for (const QString &t : texts)
        out.append(CommandItem(t));
    return out;
}

void CommandBar::setSendEnabled(bool enabled)
{
    m_sendEnabled = enabled;
    // 按钮保持可点击: 未连接时由 MainWindow 给出"未连接"提示，而不是静默无反应。
    // 只有空命令按钮禁用。
    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
        Row *r = it.value();
        r->sendBtn->setEnabled(!r->edit->text().isEmpty());
    }
    for (QPushButton *b : m_commandButtons)
        b->setEnabled(!b->toolTip().isEmpty());
}

// ---------- 行构建 ----------

CommandBar::Row *CommandBar::rowAt(int i) const
{
    QListWidgetItem *item = m_list->item(i);
    if (!item)
        return nullptr;
    return reinterpret_cast<Row *>(item->data(kRowRole).toULongLong());
}

QStringList CommandBar::currentTexts() const
{
    QStringList texts;
    const int n = m_list->count();
    texts.reserve(n);
    for (int i = 0; i < n; ++i) {
        Row *r = rowAt(i);
        texts << (r ? r->edit->text() : QString());
    }
    return texts;
}

void CommandBar::rebuildRows(const QStringList &texts, int selectRow)
{
    m_rebuilding = true;

    m_list->clear(); // item widgets 随之销毁
    qDeleteAll(m_rows);
    m_rows.clear();

    for (int i = 0; i < texts.size(); ++i) {
        auto *item = new QListWidgetItem(m_list);
        auto *row = new Row;
        row->item = item;

        auto *w = new QWidget(m_list);
        auto *lay = new QHBoxLayout(w);
        lay->setContentsMargins(4, 2, 4, 2);
        lay->setSpacing(4);

        row->idxLabel = new QLabel(QString::number(i + 1), w);
        row->idxLabel->setFixedWidth(20);
        row->idxLabel->setAlignment(Qt::AlignCenter);

        row->edit = new QLineEdit(texts[i], w);
        row->edit->setPlaceholderText(tr("命令文本"));

        row->sendBtn = new QPushButton(w);
        row->sendBtn->setIcon(themedIcon(QStringLiteral("send.png")));
        row->sendBtn->setIconSize(QSize(18, 18));
        row->sendBtn->setFixedSize(30, 26);
        row->sendBtn->setToolTip(tr("发送此命令"));
        row->sendBtn->setEnabled(!texts[i].isEmpty());

        row->gripLabel = new QLabel(w);
        row->gripLabel->setPixmap(themedIcon(QStringLiteral("grip.png")).pixmap(16, 16));
        row->gripLabel->setFixedSize(18, 26);
        row->gripLabel->setCursor(Qt::SizeAllCursor);
        row->gripLabel->setToolTip(tr("拖动排序"));
        row->gripLabel->installEventFilter(this);

        lay->addWidget(row->idxLabel);
        lay->addWidget(row->edit, 1);
        lay->addWidget(row->sendBtn);
        lay->addWidget(row->gripLabel);

        item->setData(kRowRole, reinterpret_cast<qulonglong>(row));
        item->setSizeHint(w->sizeHint());
        m_list->setItemWidget(item, w);
        m_rows.insert(item, row);

        const QPersistentModelIndex idx(m_list->model()->index(i, 0));
        connect(row->edit, &QLineEdit::textChanged, this, [this, idx, row] {
            if (m_rebuilding)
                return;
            row->sendBtn->setEnabled(!row->edit->text().isEmpty());
            // 实时同步对应按钮的文字
            const int pos = idx.row();
            if (pos >= 0 && pos < m_commandButtons.size()) {
                const QFontMetrics fm(m_commandButtons[pos]->font());
                m_commandButtons[pos]->setText(
                    fm.elidedText(row->edit->text(), Qt::ElideRight, 110));
                m_commandButtons[pos]->setToolTip(row->edit->text());
                m_commandButtons[pos]->setEnabled(!row->edit->text().isEmpty());
            }
            emit commandsChanged();
        });
        connect(row->sendBtn, &QPushButton::clicked, this, [this, row] {
            const QString text = row->edit->text();
            if (!text.isEmpty())
                emit commandRequested(text);
        });
    }

    if (selectRow >= 0 && selectRow < m_list->count())
        m_list->setCurrentRow(selectRow);
    m_rebuilding = false;
    syncCommandButtons();
}

void CommandBar::relabelRows()
{
    const int n = m_list->count();
    for (int i = 0; i < n; ++i) {
        Row *r = rowAt(i);
        if (r)
            r->idxLabel->setText(QString::number(i + 1));
    }
}

void CommandBar::syncCommandButtons()
{
    for (QPushButton *b : m_commandButtons) {
        m_buttonLayout->removeWidget(b);
        delete b;
    }
    m_commandButtons.clear();

    const QStringList texts = currentTexts();
    m_emptyHint->setVisible(texts.isEmpty());

    for (int i = 0; i < texts.size(); ++i) {
        auto *b = new QPushButton(this);
        b->setMinimumHeight(30);
        b->setToolTip(texts[i]);
        const QFontMetrics fm(b->font());
        b->setText(fm.elidedText(texts[i], Qt::ElideRight, 118));
        b->setEnabled(!texts[i].isEmpty());
        connect(b, &QPushButton::clicked, this, [this, i] {
            const QStringList texts = currentTexts();
            if (i < texts.size() && !texts[i].isEmpty())
                emit commandRequested(texts[i]);
        });
        // 插到末尾 stretch 之前
        m_buttonLayout->insertWidget(m_buttonLayout->count() - 1, b);
        m_commandButtons.append(b);
    }
}

// ---------- 增删复制排序 ----------

void CommandBar::onAddClicked()
{
    QStringList texts = currentTexts();
    texts << tr("新命令");
    const int newRow = texts.size() - 1;
    rebuildRows(texts, newRow);
    emit commandsChanged();
    if (Row *r = rowAt(newRow))
        r->edit->setFocus();
}

void CommandBar::onRemoveClicked()
{
    const int cur = m_list->currentRow();
    if (cur < 0) {
        return;
    }
    QStringList texts = currentTexts();
    texts.removeAt(cur);
    const int sel = qBound(0, cur, qMax(0, texts.size() - 1));
    rebuildRows(texts, texts.isEmpty() ? -1 : sel);
    emit commandsChanged();
}

void CommandBar::onCopyClicked()
{
    const int cur = m_list->currentRow();
    if (cur < 0)
        return;
    QStringList texts = currentTexts();
    texts.insert(cur + 1, texts.value(cur));
    rebuildRows(texts, cur + 1);
    emit commandsChanged();
}

void CommandBar::onMoveUpClicked()
{
    const int cur = m_list->currentRow();
    if (cur <= 0)
        return;
    QStringList texts = currentTexts();
    texts.swapItemsAt(cur, cur - 1);
    rebuildRows(texts, cur - 1);
    emit commandsChanged();
}

void CommandBar::onMoveDownClicked()
{
    const int cur = m_list->currentRow();
    if (cur < 0 || cur >= m_list->count() - 1)
        return;
    QStringList texts = currentTexts();
    texts.swapItemsAt(cur, cur + 1);
    rebuildRows(texts, cur + 1);
    emit commandsChanged();
}

void CommandBar::onRowsReordered()
{
    if (m_rebuilding)
        return;
    relabelRows();
    syncCommandButtons();
    emit commandsChanged();
}

// ---------- 拖动手柄: 把按压事件转发给列表视口以启动 InternalMove 拖动 ----------

bool CommandBar::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        auto *grip = qobject_cast<QLabel *>(watched);
        auto *me = static_cast<QMouseEvent *>(event);
        if (grip && me->button() == Qt::LeftButton) {
            const QPoint local = m_list->viewport()->mapFromGlobal(me->globalPosition().toPoint());
            const int row = m_list->row(m_list->itemAt(local));
            if (row >= 0) {
                m_list->setCurrentRow(row);
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(local),
                                  me->globalPosition(), me->globalPosition(),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(m_list->viewport(), &press);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
