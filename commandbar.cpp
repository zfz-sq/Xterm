#include "commandbar.h"

#include "icons.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kRowRole = Qt::UserRole + 1; // 存 Row* 指针
constexpr int kThemeIdRole = Qt::UserRole + 2; // 主题列表项存主题 ID
// 卡片内边距：命令按钮/列表距卡片圆角边缘留隙，不压边（用户定稿：卡片形式）
constexpr int kCardPad = 12;
constexpr int kCardTopPad = 8; // 顶部内边距单独收窄（用户定稿 2026-11-14：与参数栏之间空白带太大）
// 统一页宽（用户定稿 2026-11-14：以主题页宽度为准，三页同宽）——
// 切换标签时卡片宽度保持不变，不再随页内容变来变去；
// 卡片宽尺寸唯一真源 = 主题页 minimumWidth（用户手调 kPageWidth 一处即全同步）
constexpr int kPageWidth = 240;

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

// QStackedWidget 的 sizeHint/minimumSizeHint 默认遍历所有页取 max（防页切换抖动），
// 会让顶层布局把浮层卡片钳死在最大页宽（隐藏页也计入），按当前页取的宽/高自适应全部失效——
// 这里按当前页取：三页统一页宽后宽度钳制不再引发变宽，但高度 hint 仍需按当前页
//（卡片高度随页内容伸缩），且未来新增变宽页时这层仍是防线
class PageStack : public QStackedWidget
{
public:
    using QStackedWidget::QStackedWidget;

    QSize sizeHint() const override
    {
        return currentWidget() ? currentWidget()->sizeHint() : QSize();
    }

    QSize minimumSizeHint() const override
    {
        return currentWidget() ? currentWidget()->minimumSizeHint() : QSize();
    }
};

// 主题缩略图（Termius 式）：终端底色圆角矩形 + 几条调色板色"文本行"横条，
// 一眼看出主题的底色与正文配色；色样取自 ThemeDefinition 调色板
QPixmap makeThemePreview(const ThemeDefinition &t, const QSize &size)
{
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(t.terminalBg);
    p.drawRoundedRect(QRectF(0.5, 0.5, size.width() - 1.0, size.height() - 1.0), 4, 4);
    const QColor cols[] = { t.normal[7], t.normal[2], t.normal[1], t.bright[6], t.normal[3] };
    const qreal lens[] = { 0.30, 0.52, 0.40, 0.62, 0.34 }; // 各行长度比例，模拟短长不一的输出
    const qreal rowH = size.height() / 6.0;
    qreal y = rowH * 0.9;
    for (int i = 0; i < 5; ++i) {
        p.setBrush(cols[i]);
        p.drawRoundedRect(QRectF(size.width() * 0.10, y, size.width() * lens[i], rowH * 0.42), 1, 1);
        y += rowH;
    }
    return pm;
}
} // namespace

CommandBar::CommandBar(QWidget *parent)
    : QWidget(parent)
{
    // 浮层毛玻璃底色靠 QSS rgba 生效：QWidget 直接派生类不开此属性会忽略样式表背景
    setAttribute(Qt::WA_StyledBackground, true);
    buildUi();
    rebuildRows(QStringList{}, -1);
    syncFloatingWidth(); // 浮层不进布局：初始宽度按默认页（命令按钮列）自适应
}

void CommandBar::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    // 卡片内边距：内容与圆角卡片边缘的间隙，按钮不压边；顶部单独收窄
    rootLayout->setContentsMargins(kCardPad, kCardTopPad, kCardPad, kCardPad);
    rootLayout->setSpacing(4);

    // ---------- 标签行：命令 / 主题 / MCP（后续可再扩） ----------
    m_tabRow = new QWidget(this);
    auto *tabLayout = new QHBoxLayout(m_tabRow);
    tabLayout->setContentsMargins(0, 0, 0, 0);
    tabLayout->setSpacing(10); // 标签间距拉开（用户定稿 2026-11-14：太挤不协调）
    auto mkTab = [this, tabLayout](const QString &ico, const QString &tip) {
        auto *b = new QToolButton(m_tabRow);
        b->setIcon(themedIcon(ico));
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setProperty("cbTab", true); // QSS 选中态选择器（style-*.qss 的 #commandBar 段）
        tabLayout->addWidget(b);
        m_tabBtns.append(b);
        return b;
    };
    auto *cmdTab = mkTab(QStringLiteral("command.png"), tr("命令"));
    auto *themeTab = mkTab(QStringLiteral("palette.png"), tr("主题")); // 颜料盘样式（用户定稿）
    auto *mcpTab = mkTab(QStringLiteral("mcp.png"), tr("MCP"));
    tabLayout->addStretch();

    // ---------- 三页堆栈 ----------
    m_pages = new PageStack(this);
    m_pages->setFrameShape(QFrame::NoFrame);

    buildCommandPage();
    buildThemePage();
    buildMcpPage();
    m_pages->addWidget(m_commandPage);
    m_pages->addWidget(m_themePage);
    m_pages->addWidget(m_mcpPage);

    rootLayout->addWidget(m_tabRow);
    rootLayout->addWidget(m_pages, 1);

    connect(cmdTab, &QToolButton::clicked, this, [this] { switchTab(0); });
    connect(themeTab, &QToolButton::clicked, this, [this] { switchTab(1); });
    connect(mcpTab, &QToolButton::clicked, this, [this] { switchTab(2); });
    switchTab(0); // 初始命令页
}

// ---------- 命令页：按钮列 + 设置页（原卡片结构原样保留） ----------

void CommandBar::buildCommandPage()
{
    m_commandPage = new QWidget(this);
    m_commandPage->setMinimumWidth(kPageWidth); // 三页统一页宽（切 tab 卡片不变宽）
    m_commandPage->installEventFilter(this); // 堆栈页切换 → 浮层宽度自适应
    auto *pageLayout = new QVBoxLayout(m_commandPage);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(4);

    // ---------- 命令页 = 原设置页形态（用户定稿 2026-11-14：按钮列整体删除） ----------
    auto *title = new QLabel(tr("命令（拖动手柄排序）"), m_commandPage);
    title->setStyleSheet(QStringLiteral("font-weight: 600;"));
    pageLayout->addWidget(title);

    m_list = new DragListWidget(m_commandPage);
    m_list->setObjectName(QStringLiteral("commandBarCommandList")); // 测试锚点
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDropIndicatorShown(true); // 只保留插入位置指示线
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setAlternatingRowColors(true);
    // 列表持焦点时（删除后焦点归还于此）拦截 Ctrl+Z 撤销删除
    m_list->installEventFilter(this);
    pageLayout->addWidget(m_list, 1);

    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(2);
    bottom->addStretch(1);
    auto mkBtn = [this, bottom](const QString &ico, const QString &tip) {
        auto *b = new QToolButton(m_commandPage);
        b->setIcon(themedIcon(ico));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setIconSize(QSize(20, 20));
        b->installEventFilter(this); // 按钮持焦点时（如点完删除）Ctrl+Z 撤销删除
        bottom->addWidget(b);
        return b;
    };
    m_copyBtn = mkBtn(QStringLiteral("copy.png"), tr("复制选中命令"));
    m_addBtn = mkBtn(QStringLiteral("add.png"), tr("添加命令"));
    m_delBtn = mkBtn(QStringLiteral("remove.png"), tr("删除选中命令（Ctrl+Z 撤销）"));
    m_upBtn = mkBtn(QStringLiteral("up.png"), tr("上移"));
    m_downBtn = mkBtn(QStringLiteral("down.png"), tr("下移"));
    pageLayout->addLayout(bottom);

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

// ---------- 主题页：Termius 式主题列表（缩略图 + 名称，点击切换） ----------

void CommandBar::buildThemePage()
{
    m_themePage = new QWidget(this);
    m_themePage->setMinimumWidth(kPageWidth); // 统一页宽的基准页
    m_themePage->installEventFilter(this); // 堆栈页切换 → 浮层宽度自适应
    auto *pageLayout = new QVBoxLayout(m_themePage);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(4);

    auto *title = new QLabel(tr("主题"), m_themePage);
    title->setStyleSheet(QStringLiteral("font-weight: 600;"));
    pageLayout->addWidget(title);

    m_themeList = new QListWidget(m_themePage);
    m_themeList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_themeList->setFrameShape(QFrame::NoFrame);
    m_themeList->setObjectName(QStringLiteral("commandBarThemeList")); // QSS/测试锚点
    pageLayout->addWidget(m_themeList, 1);

    // 点击其他主题 → 发信号（UiBuilder 接 ThemeManager::setTheme）；重建列表期间的
    // currentChanged 被守卫拦截，不会误发
    connect(m_themeList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *cur) {
        if (m_themeRebuilding || !cur)
            return;
        emit themeRequested(cur->data(kThemeIdRole).toString());
    });
}

void CommandBar::setThemes(const QList<ThemeDefinition> &themes)
{
    m_themes = themes;
    rebuildThemeList();
}

void CommandBar::setCurrentTheme(const QString &themeId)
{
    if (m_currentThemeId == themeId)
        return;
    m_currentThemeId = themeId;
    // 只同步选中行，不重建列表（缩略图重绘成本高）；无匹配项时保持现状
    for (int i = 0; i < m_themeList->count(); ++i) {
        if (m_themeList->item(i)->data(kThemeIdRole).toString() == themeId) {
            m_themeRebuilding = true; // 程序化选中不算"用户点击"，不发 themeRequested
            m_themeList->setCurrentRow(i);
            m_themeRebuilding = false;
            return;
        }
    }
}

void CommandBar::rebuildThemeList()
{
    m_themeRebuilding = true;
    m_themeList->clear();
    for (const ThemeDefinition &t : m_themes) {
        auto *item = new QListWidgetItem(m_themeList);
        item->setData(kThemeIdRole, t.id);
        item->setSizeHint(QSize(0, 48));
        auto *row = new QWidget;
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(6, 4, 6, 4);
        lay->setSpacing(8);
        auto *thumb = new QLabel(row);
        thumb->setPixmap(makeThemePreview(t, QSize(56, 36)));
        auto *name = new QLabel(t.name, row);
        lay->addWidget(thumb);
        lay->addWidget(name, 1);
        m_themeList->setItemWidget(item, row);
        if (t.id == m_currentThemeId)
            m_themeList->setCurrentItem(item);
    }
    m_themeRebuilding = false;
}

// ---------- MCP 页：原「MCP 提示词」弹窗内容（说明/提示词/配置/复制） ----------

void CommandBar::buildMcpPage()
{
    m_mcpPage = new QWidget(this);
    m_mcpPage->setMinimumWidth(kPageWidth);
    m_mcpPage->installEventFilter(this); // 堆栈页切换 → 浮层宽度自适应
    auto *pageLayout = new QVBoxLayout(m_mcpPage);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(4);

    // MCP 提示词全文（顶部提示语已删，用户定稿 2026-11-14：多此一举）
    m_mcpPrompt = new QTextEdit(m_mcpPage);
    m_mcpPrompt->setReadOnly(true);
    m_mcpPrompt->setFrameShape(QFrame::NoFrame);
    m_mcpPrompt->document()->setDocumentMargin(2); // 文本贴左，不与卡片内边距叠加
    m_mcpPrompt->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pageLayout->addWidget(m_mcpPrompt, 1);

    auto *cfgTitle = new QLabel(tr("MCP 配置："), m_mcpPage);
    pageLayout->addWidget(cfgTitle);
    m_mcpConfig = new QPlainTextEdit(m_mcpPage);
    m_mcpConfig->setObjectName(QStringLiteral("commandBarMcpConfig")); // 主题代码块样式锚点
    m_mcpConfig->setReadOnly(true);
    m_mcpConfig->setFixedHeight(110);
    QFont mono(QStringLiteral("Consolas")); // JSON 用等宽字体，缩进对齐可读
    mono.setStyleHint(QFont::Monospace);
    m_mcpConfig->setFont(mono);
    pageLayout->addWidget(m_mcpConfig);

    auto *copy = new QPushButton(tr("复制提示词"), m_mcpPage);
    connect(copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(m_mcpPrompt->toPlainText());
    });
    pageLayout->addWidget(copy);
}

void CommandBar::setMcpContent(const QString &prompt, const QString &config)
{
    m_mcpPrompt->setPlainText(prompt);
    m_mcpConfig->setPlainText(config);

    // 说明文本排版（用户定稿 2026-11-14，四轮收敛）：行距 160% + 段间距各 2px；
    // 空段（\n\n）作段落分隔时原本占满一整行 160% 行高 + margin，堆出大片空白
    // （用户否），压成 8px 固定高度、零 margin——段落间隔变紧凑的一条缝
    QTextBlockFormat body;
    body.setLineHeight(160, QTextBlockFormat::ProportionalHeight);
    body.setTopMargin(2);
    body.setBottomMargin(2);
    QTextBlockFormat blank = body;
    blank.setLineHeight(8, QTextBlockFormat::FixedHeight);
    blank.setTopMargin(0);
    blank.setBottomMargin(0);

    for (QTextBlock block = m_mcpPrompt->document()->firstBlock();
         block.isValid(); block = block.next()) {
        QTextCursor cur(block);
        cur.setBlockFormat(block.text().isEmpty() ? blank : body);
    }
}

// ---------- 标签切换与浮层宽度 ----------

void CommandBar::switchTab(int index)
{
    m_pages->setCurrentIndex(index);
    for (int i = 0; i < m_tabBtns.size(); ++i)
        m_tabBtns[i]->setChecked(i == index);
    syncFloatingWidth(); // 宽度恒定（三页统一页宽），此处为幂等同步兜底
}

void CommandBar::applyTheme()
{
    m_copyBtn->setIcon(themedIcon(QStringLiteral("copy.png")));
    m_addBtn->setIcon(themedIcon(QStringLiteral("add.png")));
    m_delBtn->setIcon(themedIcon(QStringLiteral("remove.png")));
    m_upBtn->setIcon(themedIcon(QStringLiteral("up.png")));
    m_downBtn->setIcon(themedIcon(QStringLiteral("down.png")));
    m_tabBtns[0]->setIcon(themedIcon(QStringLiteral("command.png")));
    m_tabBtns[1]->setIcon(themedIcon(QStringLiteral("palette.png"))); // 同 mkTab：颜料盘（用户定稿）
    m_tabBtns[2]->setIcon(themedIcon(QStringLiteral("mcp.png")));

    for (Row *row : m_rows) {
        if (!row)
            continue;
        row->sendBtn->setIcon(themedIcon(QStringLiteral("send.png")));
        row->gripLabel->setPixmap(themedIcon(QStringLiteral("grip.png")).pixmap(16, 16));
    }
}

// 浮层宽度自管：三页统一页宽（用户定稿 2026-11-14，以主题页宽度为准）——
// 切换标签/命令形态时卡片宽度恒定不变。宽度唯一真源 = 主题页 minimumWidth
//（kPageWidth 手调一处即全同步）；高度仍走布局链（随页内容伸缩）。形态子件
// 显隐判断曾用 WA_WState_Hidden（isVisible 父链未 show 时恒 false），宽度恒定后
// 不再读形态，仅高度自适应保留布局链原生行为
QSize CommandBar::sizeHint() const
{
    return QSize(m_themePage->minimumWidth() + 2 * kCardPad,
                 QWidget::sizeHint().height());
}

void CommandBar::syncFloatingWidth()
{
    resize(sizeHint().width(), height());
}

// ---------- 对外接口 ----------

void CommandBar::setCommands(const QList<CommandItem> &commands)
{
    m_undoStack.clear(); // 整表来自外部（切换 Profile 等），删除历史作废
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
    // 按钮保持可点击: 未连接时由 MainWindow 给出“未连接”提示，而不是静默无反应。
    // 只有空命令按钮禁用。
    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
        Row *r = it.value();
        r->sendBtn->setEnabled(!r->edit->text().isEmpty());
    }
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
        row->sendBtn->installEventFilter(this); // 持焦点时 Ctrl+Z 撤销删除

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

        connect(row->edit, &QLineEdit::textChanged, this, [this, row] {
            if (m_rebuilding)
                return;
            row->sendBtn->setEnabled(!row->edit->text().isEmpty());
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
    // 命令内容变化会改变页面高度 hint（卡片宽度/高度自适应的依据），排队重算；
    // 与页显隐共用 Queued 链路，多次触发幂等
    QMetaObject::invokeMethod(this, &CommandBar::syncFloatingWidth, Qt::QueuedConnection);
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
    // 记下被删内容与原位置，Ctrl+Z 恢复（后删的先回来）
    m_undoStack.append(RemovedCmd{ cur, texts.value(cur) });
    if (m_undoStack.size() > 100)
        m_undoStack.removeFirst();
    texts.removeAt(cur);
    const int sel = qBound(0, cur, qMax(0, texts.size() - 1));
    rebuildRows(texts, texts.isEmpty() ? -1 : sel);
    // 被删行的控件已随重建销毁，焦点会飘走；还给列表，紧接着按 Ctrl+Z 即可撤销
    m_list->setFocus();
    emit commandsChanged();
}

// Ctrl+Z 撤销最近一次删除：命令回到被删时的行号（其间又有增删则就近
// 放置）。命令页列表常驻（用户定稿 2026-11-14：按钮列形态已删），随时生效；
// 焦点在命令文本框里时 Ctrl+Z 归 QLineEdit 自己的文字撤销（eventFilter 不拦）。
void CommandBar::undoRemove()
{
    if (m_undoStack.isEmpty())
        return;
    const RemovedCmd cmd = m_undoStack.takeLast();
    QStringList texts = currentTexts();
    const int row = qBound(0, cmd.row, texts.size());
    texts.insert(row, cmd.text);
    rebuildRows(texts, row);
    m_list->setFocus();
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
    emit commandsChanged();
}

// ---------- 拖动手柄: 把按压事件转发给列表视口以启动 InternalMove 拖动 ----------

// 焦点落在命令栏自身（点过空白处）时的 Ctrl+Z
void CommandBar::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Z && event->modifiers() == Qt::ControlModifier
        && !event->isAutoRepeat()) {
        undoRemove();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool CommandBar::eventFilter(QObject *watched, QEvent *event)
{
    // 堆栈页显隐：浮层宽度跟随；不拦事件本身，显隐照常生效。
    // 不能在 Hide/Show 派发中同步 resize 父级——会重入布局/hide_helper
    // 直接崩（子件还处在半隐藏中间态），排队到下一拍
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide) {
        if (watched == m_commandPage || watched == m_themePage || watched == m_mcpPage) {
            QMetaObject::invokeMethod(this, &CommandBar::syncFloatingWidth, Qt::QueuedConnection);
            return false;
        }
    }
    // 命令页里的 Ctrl+Z：撤销最近一次删除。只拦非 QLineEdit 的控件——
    // 命令文本框内的 Ctrl+Z 是编辑框自己的文字撤销，不能劫持。
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Z && ke->modifiers() == Qt::ControlModifier
            && !ke->isAutoRepeat() && qobject_cast<QLineEdit *>(watched) == nullptr) {
            undoRemove();
            return true;
        }
    }
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
