#include "commandbar.h"

#include <QtTest/QtTest>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QWidget>

#include <algorithm>

/*
 * 多功能侧边栏（命令栏卡片，用户定稿 2026-11-14）回归：
 * - 顶部标签行切换 命令/主题/MCP 三页；三页统一页宽（以主题页宽度为准）——
 *   切标签卡片宽度恒定，高度随页内容伸缩（不进布局的浮层，宽度没有布局替它
 *   管理——页显隐后必须自己 resize）
 * - 命令页 = 原设置页形态（按钮列已删）：行 = 序号|文本|发送|拖动手柄，
 *   编辑即持久化信号，发送钮直接发命令，底部上移/下移作用于选中行
 * - 主题页：Termius 式列表（缩略图+名称），当前主题高亮；用户点击发 themeRequested，
 *   程序化 setCurrentTheme 只同步高亮不发信号
 * 断言全部与实现同源：内边距取根布局 contentsMargins()，页宽取堆栈页 minimumWidth()
 * ——常量 kCardPad/kPageWidth 可被手动调参而测试不破。
 * 只通过子件 objectName/property 与 sizeHint()/width() 黑盒断言，不碰私有成员。
 */

namespace {
// 卡片左右内边距从根布局公开 API 同源获取
int cardPadLR(const QWidget &bar)
{
    const QMargins m = bar.layout()->contentsMargins();
    return m.left() + m.right();
}

// 侧边栏标签按钮经 cbTab 动态属性定位，tooltip 区分命令/主题/MCP
QToolButton *findTab(const QWidget &bar, const QString &tipPart)
{
    for (QToolButton *b : bar.findChildren<QToolButton *>())
        if (b->property("cbTab").toBool() && b->toolTip().contains(tipPart))
            return b;
    return nullptr;
}
} // namespace

class CommandBarTest : public QObject
{
    Q_OBJECT

private slots:
    void unifiedPageWidthAndSizeHint();
    void tabSwitchKeepsCardWidth();
    void commandPageRowsAndEditing();
    void themeListRequestsThemeSwitch();
    void mcpPageShowsInjectedContent();
    void contentChangeKeepsCardWidth();
};

void CommandBarTest::unifiedPageWidthAndSizeHint()
{
    CommandBar bar;
    bar.setCommands(QList<CommandItem>{ CommandItem(QStringLiteral("ls -l")) });
    QTest::qWait(20); // rebuildRows → 排队 syncFloatingWidth 落地

    // 统一页宽（用户定稿：以主题页宽度为准）：三页 minimumWidth 相同，
    // 卡片宽 = 页宽 + 左右内边距
    auto *pages = bar.findChild<QStackedWidget *>();
    QVERIFY(pages);
    const int pageW = pages->widget(1)->minimumWidth(); // 主题页 = 基准页
    QVERIFY(pageW > 0);
    for (int i = 0; i < pages->count(); ++i)
        QCOMPARE(pages->widget(i)->minimumWidth(), pageW);
    QCOMPARE(bar.sizeHint().width(), pageW + cardPadLR(bar));

    // 必须先 show 浮层：Qt 对父链未显示的子件是 deferred show——setVisible(true)
    // 只清 WA_WState_Hidden 不派发 QEvent::Show，eventFilter 拿不到 Show 就不会
    // 排队 syncFloatingWidth（真实浮层总是 show 的，此处对齐）
    bar.show();
    QTest::qWait(20);
    QCOMPARE(bar.width(), bar.sizeHint().width()); // 浮层自身宽度保持
}

void CommandBarTest::tabSwitchKeepsCardWidth()
{
    // 用户定稿（2026-11-14）：三页统一页宽——切换标签卡片宽度不再变来变去
    CommandBar bar;
    bar.show(); // deferred show：宽度跟随链生效前提（对齐真实浮层）
    QTest::qWait(20);
    const int wAll = bar.sizeHint().width();

    auto *pages = bar.findChild<QStackedWidget *>();
    QVERIFY(pages);
    QCOMPARE(pages->currentIndex(), 0); // 初始命令页

    // 切主题页：宽度不变
    QToolButton *themeTab = findTab(bar, QStringLiteral("主题"));
    QVERIFY(themeTab);
    themeTab->click();
    QTest::qWait(20); // syncFloatingWidth 是 Queued
    QCOMPARE(pages->currentIndex(), 1);
    QCOMPARE(bar.sizeHint().width(), wAll);
    QCOMPARE(bar.width(), wAll);

    // 切 MCP 页：宽度不变
    QToolButton *mcpTab = findTab(bar, QStringLiteral("MCP"));
    QVERIFY(mcpTab);
    mcpTab->click();
    QTest::qWait(20);
    QCOMPARE(pages->currentIndex(), 2);
    QCOMPARE(bar.sizeHint().width(), wAll);
    QCOMPARE(bar.width(), wAll);

    // 回命令页：宽度仍不变
    QToolButton *cmdTab = findTab(bar, QStringLiteral("命令"));
    QVERIFY(cmdTab);
    cmdTab->click();
    QTest::qWait(20);
    QCOMPARE(pages->currentIndex(), 0);
    QCOMPARE(bar.sizeHint().width(), wAll);
    QCOMPARE(bar.width(), wAll);
}

void CommandBarTest::commandPageRowsAndEditing()
{
    // 命令页 = 原设置页形态（用户定稿 2026-11-14：按钮列已删）：
    // 行 = 序号 | 文本框 | 发送 | 拖动手柄；编辑即持久化信号；发送钮直接发命令；
    // 底部上移/下移作用于选中行
    CommandBar bar;
    bar.setCommands(QList<CommandItem>{ CommandItem(QStringLiteral("first")),
                                        CommandItem(QStringLiteral("second")),
                                        CommandItem(QStringLiteral("third")) });
    bar.show(); // deferred show：宽度跟随链生效前提
    QTest::qWait(20);

    auto *list = bar.findChild<QListWidget *>(QStringLiteral("commandBarCommandList"));
    QVERIFY(list);
    QCOMPARE(list->count(), 3);

    // 行结构：每行一个文本框（=命令全文）+ 一个发送按钮
    const auto rowEdits = list->findChildren<QLineEdit *>();
    QCOMPARE(rowEdits.size(), 3);
    QStringList texts;
    for (QLineEdit *e : rowEdits)
        texts << e->text();
    texts.sort();
    const QStringList expected{QStringLiteral("first"), QStringLiteral("second"),
                               QStringLiteral("third")};
    QCOMPARE(texts, expected);
    QCOMPARE(list->findChildren<QPushButton *>().size(), 3); // 每行一个发送钮

    // 点击首行发送按钮 → commandRequested 携带该行全文
    QSignalSpy sent(&bar, &CommandBar::commandRequested);
    auto *firstRow = list->itemWidget(list->item(0));
    QVERIFY(firstRow);
    firstRow->findChildren<QPushButton *>().first()->click();
    QCOMPARE(sent.count(), 1);
    QCOMPARE(sent.takeFirst().at(0).toString(), QStringLiteral("first"));

    // 编辑行文本 → commandsChanged（UiBuilder 落库）
    QSignalSpy changed(&bar, &CommandBar::commandsChanged);
    firstRow->findChild<QLineEdit *>()->setText(QStringLiteral("first*"));
    QCOMPARE(changed.count(), 1);

    // 底部上移作用于选中行：选中第二行点上移 → second 到首行
    QToolButton *upBtn = nullptr;
    for (QToolButton *b : bar.findChildren<QToolButton *>())
        if (b->toolTip() == QStringLiteral("上移"))
            upBtn = b;
    QVERIFY(upBtn);
    list->setCurrentRow(1);
    upBtn->click();
    QCOMPARE(changed.count(), 2);
    auto *newFirst = list->itemWidget(list->item(0))->findChild<QLineEdit *>();
    QVERIFY(newFirst);
    QCOMPARE(newFirst->text(), QStringLiteral("second"));

    // 卡片宽度在编辑/移动前后保持恒定（统一页宽）
    QCOMPARE(bar.width(), bar.sizeHint().width());
}

void CommandBarTest::themeListRequestsThemeSwitch()
{
    CommandBar bar;
    QList<ThemeDefinition> themes;
    for (int i = 0; i < 3; ++i) {
        ThemeDefinition t;
        t.id = QStringLiteral("t%1").arg(i);
        t.name = QStringLiteral("主题%1").arg(i);
        t.isDark = true;
        t.terminalBg = QColor(20, 20, 20);
        for (int c = 0; c < 8; ++c) {
            t.normal[c] = QColor(200, 200, 200);
            t.bright[c] = QColor(240, 240, 240);
        }
        themes << t;
    }
    bar.setThemes(themes);
    bar.setCurrentTheme(QStringLiteral("t0"));

    auto *list = bar.findChild<QListWidget *>(QStringLiteral("commandBarThemeList"));
    QVERIFY(list);
    QCOMPARE(list->count(), 3);
    QCOMPARE(list->currentRow(), 0); // 当前主题高亮

    // 用户点击其他主题 → 发 themeRequested（UiBuilder 接 ThemeManager::setTheme）
    QSignalSpy spy(&bar, &CommandBar::themeRequested);
    list->setCurrentRow(2);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("t2"));

    // 外部切换（标题栏按钮/主题设置对话框）后同步高亮：幂等、不发信号
    bar.setCurrentTheme(QStringLiteral("t2"));
    QCOMPARE(list->currentRow(), 2);
    QCOMPARE(spy.count(), 0);
}

void CommandBarTest::mcpPageShowsInjectedContent()
{
    CommandBar bar;
    bar.setMcpContent(QStringLiteral("PROMPT-TEXT"), QStringLiteral("{\"json\":true}"));

    // 说明/提示词用 QTextEdit（富文本布局，块行距才能生效）；配置 JSON 保持 QPlainTextEdit
    const auto prompts = bar.findChildren<QTextEdit *>();
    QCOMPARE(prompts.size(), 1);
    QCOMPARE(prompts.first()->toPlainText(), QStringLiteral("PROMPT-TEXT"));
    const auto configs = bar.findChildren<QPlainTextEdit *>();
    QCOMPARE(configs.size(), 1);
    QCOMPARE(configs.first()->toPlainText(), QStringLiteral("{\"json\":true}"));
}

void CommandBarTest::contentChangeKeepsCardWidth()
{
    // 统一页宽下卡片宽恒定：内容加载/增删只改高度不改变宽度（用户定稿）
    CommandBar bar;
    const int wEmpty = bar.width();

    bar.setCommands(QList<CommandItem>{ CommandItem(QStringLiteral(
        "/opt/ohsp/HPCS3000_crontab_check_every_five_minutes.sh")) });
    QTest::qWait(20); // rebuildRows → 排队 syncFloatingWidth
    QCOMPARE(bar.width(), wEmpty); // 内容变化不改变卡片宽
    QCOMPARE(bar.width(), bar.sizeHint().width());
}

QTEST_MAIN(CommandBarTest)
#include "test_commandbar.moc"
