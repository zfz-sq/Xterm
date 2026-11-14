// M1.a ThemeManager 协作类测试：状态翻转、信号广播、落盘边界。
// 不 new MainWindow——只验证协作类自身的窄职责与信号契约。
#include <QSignalSpy>
#include <QTest>

#include "commandstore.h"
#include "thememanager.h"

class TestThemeManager : public QObject
{
    Q_OBJECT

private slots:
    void defaultsToDark();
    void toggleFlipsStateAndEmits();
    void toggleTwiceReturnsToOriginal();
    void applyNowDoesNotEmit();
    void applyNowLoadsThemeQss();
    void applyNowInjectsStatusBarColors();
    void applyNowInjectsTextAndDialogColors();
    void applyNowInjectsButtonStates();
    void applyNowInjectsListStates();
    void applyNowInjectsScrollBarColors();
    void applyNowInjectsCommandBarOverlay();
    void isDarkTracksStore();
    void themeLibraryHasUniqueIds();
    void themeByIdFallsBackToDark();
    void setThemeUpdatesStateAndEmitsBoth();
    void setThemeLightVariantCountsAsLight();
    void toggleFromThirdPartyThemeFlipsToDark();
};

void TestThemeManager::defaultsToDark()
{
    CommandStore store;
    ThemeManager theme(&store);
    QVERIFY(theme.isDark());
}

void TestThemeManager::toggleFlipsStateAndEmits()
{
    CommandStore store;
    ThemeManager theme(&store);
    QSignalSpy spy(&theme, &ThemeManager::themeChanged);

    theme.toggleTheme();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), false); // 默认 dark → 切到 light
    QVERIFY(!theme.isDark());
    QVERIFY(!store.isDarkTheme());
}

void TestThemeManager::toggleTwiceReturnsToOriginal()
{
    CommandStore store;
    ThemeManager theme(&store);
    QSignalSpy spy(&theme, &ThemeManager::themeChanged);

    theme.toggleTheme();
    theme.toggleTheme();

    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), true);
    QVERIFY(theme.isDark());
}

void TestThemeManager::applyNowDoesNotEmit()
{
    // applyNow 是初启路径（load 之后调用一次），不广播级联刷新
    CommandStore store;
    ThemeManager theme(&store);
    QSignalSpy spy(&theme, &ThemeManager::themeChanged);

    theme.applyNow();

    QCOMPARE(spy.count(), 0);
    // 重复 applyNow 不改变状态
    QVERIFY(theme.isDark());
}

void TestThemeManager::applyNowLoadsThemeQss()
{
    // QSS 真从 :/style-*.qss 读入并落到应用级样式表；两主题内容不同
    CommandStore store;
    ThemeManager theme(&store);

    store.setDarkTheme(true);
    theme.applyNow();
    const QString darkQss = qApp->styleSheet();
    QVERIFY(!darkQss.isEmpty());

    store.setDarkTheme(false);
    theme.applyNow();
    const QString lightQss = qApp->styleSheet();
    QVERIFY(!lightQss.isEmpty());
    QVERIFY(lightQss != darkQss);
}

void TestThemeManager::applyNowInjectsStatusBarColors()
{
    // 状态栏底/文字色写死在基础 QSS 里只有 dark/light 两套，第三方主题靠
    // 动态覆盖注入（QStatusBar + QLabel 子选择器）；不注入则切主题后
    // 状态栏停留在旧底色——回归用户反馈的“状态栏没同步”
    CommandStore store;
    ThemeManager theme(&store);

    store.setTheme(QStringLiteral("dracula"));
    theme.applyNow();
    const QString qss = qApp->styleSheet();
    QVERIFY(qss.contains(ThemeManager::themeById(QStringLiteral("dracula"))
                             .statusBarBg.name()));
    QVERIFY(qss.contains(QStringLiteral("QStatusBar QLabel { color: ")
                         + ThemeManager::themeById(QStringLiteral("dracula"))
                               .statusBarFg.name()));

    // 内置 dark 主题的注入值与原 QSS 相同，但规则仍要存在（展开时覆盖生效）
    store.setTheme(QStringLiteral("dark"));
    theme.applyNow();
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QStatusBar { background-color: #1e1e1e")));
}

void TestThemeManager::applyNowInjectsTextAndDialogColors()
{
    // 界面正文文字色与浮层（对话框/菜单）底色同样只备了 dark/light 两套，
    // 第三方主题靠动态覆盖注入；否则 dracula 界面下弹窗仍是 VS Code 灰、
    // 文字停在旧灰——回归“字体默认色/弹窗没跟主题”问题
    CommandStore store;
    ThemeManager theme(&store);
    const ThemeDefinition dracula =
        ThemeManager::themeById(QStringLiteral("dracula"));

    store.setTheme(QStringLiteral("dracula"));
    theme.applyNow();
    const QString qss = qApp->styleSheet();
    QVERIFY(qss.contains(QStringLiteral("QLabel { color: ") + dracula.textFg.name()));
    QVERIFY(qss.contains(QStringLiteral("QDialog#FramelessDialog { background-color: ")
                         + dracula.dialogBg.name()));
    QVERIFY(qss.contains(QStringLiteral("QMenu { background-color: ")
                         + dracula.dialogBg.name()));
    QVERIFY(qss.contains(QStringLiteral("QWidget#TitleBar { background-color: ")
                         + dracula.statusBarBg.name()));

    // 内置主题零变化红线：注入值必须与基础 QSS 原值一致
    const ThemeDefinition dark = ThemeManager::themeById(QStringLiteral("dark"));
    store.setTheme(QStringLiteral("dark"));
    theme.applyNow();
    QVERIFY(dark.textFg.name() == QLatin1String("#b8b8b8"));
    QVERIFY(dark.dialogBg.name() == QLatin1String("#252528"));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QLabel { color: #b8b8b8")));
}

void TestThemeManager::applyNowInjectsButtonStates()
{
    // 按钮色基础 QSS 只备 dark/light 两套：第三方主题靠动态覆盖四态注入，
    // 否则切主题后连接栏/对话框里的按钮仍是旧灰——回归“按钮没跟主题”
    CommandStore store;
    ThemeManager theme(&store);
    const ThemeDefinition dracula =
        ThemeManager::themeById(QStringLiteral("dracula"));

    store.setTheme(QStringLiteral("dracula"));
    theme.applyNow();
    const QString qss = qApp->styleSheet();
    QVERIFY(qss.contains(QStringLiteral("QPushButton { background-color: ")
                         + dracula.buttonBg.name()));
    QVERIFY(qss.contains(QStringLiteral("QPushButton:hover { background-color: ")
                         + dracula.buttonHoverBg.name()));
    QVERIFY(qss.contains(QStringLiteral("QPushButton:pressed { background-color: ")
                         + dracula.buttonPressedBg.name()));
    // #id 特异性的工具按钮（设置齿轮/密码眼睛）也要同规则压住基础 QSS
    QVERIFY(qss.contains(QStringLiteral("QToolButton#settingsBtn, QToolButton#pwToggleBtn { background-color: ")
                         + dracula.buttonBg.name()));
    // 模式切换按钮的基础 QSS 是 #id 选择器（特异性高），必须同款选择器才能压住
    QVERIFY(qss.contains(QStringLiteral("QToolButton#modeBtn, QToolButton#mcpBtn, QToolButton#keyBrowseBtn { background-color: ")
                         + dracula.buttonBg.name()));
    // 焦点态：基础 QSS 的 QComboBox:focus 带旧背景色（启动后首个输入控件即焦点），
    // 覆盖漏了背景会让 portCombo 永远停在旧灰——仅边框跟随不算修复
    QVERIFY(qss.contains(QStringLiteral("QSpinBox:focus { border-color: ")
                         + dracula.accentColor.name()
                         + QStringLiteral("; background-color: ")
                         + dracula.inputBg.name()));
    QVERIFY(qss.contains(QStringLiteral("QComboBox:hover, QLineEdit:hover, QSpinBox:hover { background-color: ")
                         + dracula.inputBg.name()));
    // 数字输入（字号等）与 QLineEdit 同口径，普通态也要跟随主题
    QVERIFY(qss.contains(QStringLiteral("QSpinBox { background-color: ")
                         + dracula.inputBg.name()));

    // 内置主题零变化红线：注入值与基础 QSS 原值一致
    const ThemeDefinition dark = ThemeManager::themeById(QStringLiteral("dark"));
    QVERIFY(dark.buttonBg.name() == QLatin1String("#2c2c30"));
    QVERIFY(dark.buttonHoverBg.name() == QLatin1String("#35353a"));
    QVERIFY(dark.buttonPressedBg.name() == QLatin1String("#2a2a2e"));
    store.setTheme(QStringLiteral("dark"));
    theme.applyNow();
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QPushButton { background-color: #2c2c30")));

    // 主题库全体字段完整性：新加主题漏填按钮色会在运行时出黑块
    for (const auto &t : ThemeManager::availableThemes()) {
        QVERIFY2(t.buttonBg.isValid() && t.buttonBorder.isValid(), qPrintable(t.name));
        QVERIFY2(t.buttonHoverBg.isValid() && t.buttonHoverBorder.isValid(), qPrintable(t.name));
        QVERIFY2(t.buttonPressedBg.isValid(), qPrintable(t.name));
    }
}

void TestThemeManager::applyNowInjectsListStates()
{
    // SFTP 文件浏览器/传输队列/命令设置列表（树/表格/表头/pathBar）基础 QSS
    // 只备 dark/light 两套：第三方主题靠动态覆盖注入，否则 SFTP 文件区
    // 仍停留在旧灰——回归“sftp 主题适配不完整”
    CommandStore store;
    ThemeManager theme(&store);
    const ThemeDefinition dracula =
        ThemeManager::themeById(QStringLiteral("dracula"));

    store.setTheme(QStringLiteral("dracula"));
    theme.applyNow();
    const QString qss = qApp->styleSheet();
    // 列表背景/斑马纹/边框/网格线整体注入（仅 color 不够，背景是主要反差）
    QVERIFY(qss.contains(QStringLiteral("QTreeWidget, QTreeView, QTableWidget, QTableView { background-color: ")
                         + dracula.listBg.name()
                         + QStringLiteral("; alternate-background-color: ")
                         + dracula.listAltBg.name()));
    // 条目悬停与选中（子选择器特异性 (0,1,1) 与基础规则同级，必须后写覆盖）
    QVERIFY(qss.contains(QStringLiteral("::item:selected { background-color: ")
                         + dracula.listSelectedBg.name()
                         + QStringLiteral("; color: ")
                         + dracula.listSelectedFg.name()));
    QVERIFY(qss.contains(QStringLiteral("::item:hover { background-color: ")
                         + dracula.listHoverBg.name()));
    // 表头与左上角钮：传输队列表头不跟随是最显眼的破绽
    QVERIFY(qss.contains(QStringLiteral("QHeaderView::section { background-color: ")
                         + dracula.headerBg.name()
                         + QStringLiteral("; color: ")
                         + dracula.headerFg.name()));
    QVERIFY(qss.contains(QStringLiteral("QTableCornerButton::section { background-color: ")
                         + dracula.headerBg.name()));
    // pathBar 基础 QSS 是 #id 选择器（特异性高），必须同款选择器才能压住
    QVERIFY(qss.contains(QStringLiteral("QFrame#pathBar { background-color: ")
                         + dracula.inputBg.name()));

    // 内置主题零变化红线：注入值与基础 QSS 原值一致
    const ThemeDefinition dark = ThemeManager::themeById(QStringLiteral("dark"));
    QVERIFY(dark.listBg.name() == QLatin1String("#202023"));
    QVERIFY(dark.listAltBg.name() == QLatin1String("#252528"));
    QVERIFY(dark.listSelectedBg.name() == QLatin1String("#094771"));
    QVERIFY(dark.headerBg.name() == QLatin1String("#242427"));
    const ThemeDefinition light = ThemeManager::themeById(QStringLiteral("light"));
    QVERIFY(light.listBg.name() == QLatin1String("#fbfbfc"));
    QVERIFY(light.listSelectedBg.name() == QLatin1String("#d6e9ff"));
    QVERIFY(light.headerBg.name() == QLatin1String("#ededef"));
    store.setTheme(QStringLiteral("dark"));
    theme.applyNow();
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QTreeWidget, QTreeView, QTableWidget, QTableView { background-color: #202023")));

    // 主题库全体字段完整性：新加主题漏填列表色会在运行时出黑块
    for (const auto &t : ThemeManager::availableThemes()) {
        QVERIFY2(t.listBg.isValid() && t.listAltBg.isValid() && t.listBorder.isValid(), qPrintable(t.name));
        QVERIFY2(t.listHoverBg.isValid() && t.listSelectedBg.isValid() && t.listSelectedFg.isValid(), qPrintable(t.name));
        QVERIFY2(t.headerBg.isValid() && t.headerFg.isValid() && t.gridlineColor.isValid(), qPrintable(t.name));
    }
}

void TestThemeManager::applyNowInjectsScrollBarColors()
{
    // 滚动条滑块两态随主题：Termius 主题用户钦定暗绿滑块/悬停招牌亮绿，
    // 基础 QSS 的硬色靠 override 注入覆盖（待机透明规则特异性更高不受影响）
    CommandStore store;
    ThemeManager theme(&store);

    store.setTheme(QStringLiteral("termius"));
    theme.applyNow();
    const QString qss = qApp->styleSheet();
    QVERIFY(qss.contains(QStringLiteral("QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background-color: #285049")));
    QVERIFY(qss.contains(QStringLiteral("QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background-color: #21b568")));
    // Termius 实色轨道（用户钦定 #2C2F3F）：浮现态轨道不再用写死的 5% 白叠加
    QVERIFY(qss.contains(QStringLiteral("QScrollBar[flash=\"true\"]:vertical, QScrollBar[flash=\"true\"]:horizontal { background-color: #2c2f3f")));

    // 内置主题零变化红线：dark/light 字段与基础 QSS 原值一致；
    // 轨道为 5% 半透明叠加，注入格式为 rgba()（QColor::name() 不带 alpha）
    const ThemeDefinition dark = ThemeManager::themeById(QStringLiteral("dark"));
    QVERIFY(dark.scrollHandleBg.name() == QLatin1String("#4a4a52"));
    QVERIFY(dark.scrollHandleHoverBg.name() == QLatin1String("#5a5a62"));
    QCOMPARE(dark.scrollTrackBg.red(), 255);
    QCOMPARE(dark.scrollTrackBg.green(), 255);
    QCOMPARE(dark.scrollTrackBg.blue(), 255);
    QVERIFY(dark.scrollTrackBg.alpha() < 255); // 半透明叠加
    store.setTheme(QStringLiteral("dark"));
    theme.applyNow();
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QScrollBar[flash=\"true\"]:vertical, QScrollBar[flash=\"true\"]:horizontal { background-color: rgba(255,255,255,")));
    const ThemeDefinition light = ThemeManager::themeById(QStringLiteral("light"));
    QVERIFY(light.scrollHandleBg.name() == QLatin1String("#a9a9b0"));
    QVERIFY(light.scrollHandleHoverBg.name() == QLatin1String("#8e8e96"));

    // 主题库全体字段完整性
    for (const auto &t : ThemeManager::availableThemes()) {
        QVERIFY2(t.scrollHandleBg.isValid() && t.scrollHandleHoverBg.isValid() && t.scrollTrackBg.isValid(), qPrintable(t.name));
    }
}

void TestThemeManager::applyNowInjectsCommandBarOverlay()
{
    // 命令栏浮层毛玻璃底色（Termius 式「主体+浮层」）：panelBg 抬档 + 半透明 rgba
    // 注入 + 圆角卡片（无边框线，浮在主背景上）
    CommandStore store;
    ThemeManager theme(&store);

    const ThemeDefinition dracula = ThemeManager::themeById(QStringLiteral("dracula"));
    QVERIFY(dracula.panelBg == dracula.terminalBg); // 前提：同色主题必须抬档

    store.setTheme(QStringLiteral("dracula"));
    theme.applyNow();
    const QString qss = qApp->styleSheet();
    QVERIFY(qss.contains(QStringLiteral("CommandBar { background-color: rgba("))
             && qss.contains(QStringLiteral("; border-radius: 8px; }"))); // 圆角卡片无独立边框

    // 注入串与实现同构：lighter/darker 抬档 + alpha 200 → rgba(r,g,b,200)
    QColor overlay = dracula.isDark ? dracula.panelBg.lighter(112)
                                    : dracula.panelBg.darker(104);
    overlay.setAlpha(200);
    QVERIFY(overlay != dracula.terminalBg); // 抬档后与终端底可辨（区分度红线）
    QVERIFY(qss.contains(QStringLiteral("CommandBar { background-color: rgba(%1,%2,%3,%4)")
                             .arg(overlay.red()).arg(overlay.green())
                             .arg(overlay.blue()).arg(overlay.alpha())));

    // 设置页内列表去卡片化（用户定稿：融入毛玻璃卡片整体）：无边框+透明底
    QVERIFY(qss.contains(QStringLiteral(
        "CommandBar QListWidget { background-color: transparent; "
        "alternate-background-color: transparent; border: none; }")));

    // 半透明（毛玻璃透出底下淡影）：dark 注入同样为 rgba 整数 alpha
    store.setTheme(QStringLiteral("dark"));
    theme.applyNow();
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("CommandBar { background-color: rgba(")));
}

void TestThemeManager::isDarkTracksStore()
{
    // 状态源是 CommandStore：外部直接改 store，isDark 要如实反映
    CommandStore store;
    ThemeManager theme(&store);
    store.setDarkTheme(false);
    QVERIFY(!theme.isDark());
    store.setDarkTheme(true);
    QVERIFY(theme.isDark());
}

void TestThemeManager::themeLibraryHasUniqueIds()
{
    // 内置主题库是多主题功能的注册表：id 必须唯一、至少覆盖暗/亮各一套，
    // 否则设置对话框/菜单的单选与 themeById 回落都会错乱
    const auto &themes = ThemeManager::availableThemes();
    QVERIFY(themes.size() >= 2);
    QSet<QString> ids;
    bool hasDark = false, hasLight = false;
    for (const auto &t : themes) {
        QVERIFY2(!t.id.isEmpty(), qPrintable(t.name));
        QVERIFY2(!ids.contains(t.id), qPrintable(QStringLiteral("重复 id: %1").arg(t.id)));
        ids.insert(t.id);
        QVERIFY(t.terminalBg.isValid() && t.terminalFg.isValid());
        QVERIFY(t.accentColor.isValid());
        QVERIFY(t.statusBarBg.isValid() && t.statusBarFg.isValid());
        QVERIFY(t.textFg.isValid() && t.dialogBg.isValid());
        hasDark = hasDark || t.isDark;
        hasLight = hasLight || !t.isDark;
    }
    QVERIFY(hasDark);
    QVERIFY(hasLight);

    // Termius 复刻主题的招牌组合：深夜蓝底 + 亮绿正文（用户点名参考 Termius），
    // 防止后续调色误改这两个定海神针
    const ThemeDefinition termius =
        ThemeManager::themeById(QStringLiteral("termius"));
    QCOMPARE(termius.terminalBg, QColor(0x14, 0x17, 0x29));
    QCOMPARE(termius.terminalFg, QColor(0x00, 0xCC, 0x74));
    QVERIFY(termius.isDark);
}

void TestThemeManager::themeByIdFallsBackToDark()
{
    // 未知 id（老配置里的 id 被删、手改 config.json）不得返回无效定义：回落首套 dark
    QCOMPARE(ThemeManager::themeById(QStringLiteral("no-such-theme")).id,
             QStringLiteral("dark"));
    QCOMPARE(ThemeManager::themeById(QString()).id, QStringLiteral("dark"));
}

void TestThemeManager::setThemeUpdatesStateAndEmitsBoth()
{
    // setTheme 是设置对话框/菜单的统一入口：状态落 store、双信号广播
    CommandStore store;
    ThemeManager theme(&store);
    QSignalSpy changedSpy(&theme, &ThemeManager::themeChanged);
    QSignalSpy updatedSpy(&theme, &ThemeManager::themeUpdated);

    theme.setTheme(QStringLiteral("dracula"));

    QCOMPARE(theme.currentThemeId(), QStringLiteral("dracula"));
    QCOMPARE(store.theme(), QStringLiteral("dracula"));
    QVERIFY(theme.isDark()); // dracula 是暗色系
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(updatedSpy.count(), 1);
    QCOMPARE(updatedSpy.at(0).at(0).value<ThemeDefinition>().id,
             QStringLiteral("dracula"));
}

void TestThemeManager::setThemeLightVariantCountsAsLight()
{
    // solarized-light 不叫 "light" 但也是浅色：isDarkTheme 判定与 QSS 选择都得认
    CommandStore store;
    ThemeManager theme(&store);

    theme.setTheme(QStringLiteral("solarized-light"));

    QVERIFY(!theme.isDark());
    QVERIFY(!store.isDarkTheme());
}

void TestThemeManager::toggleFromThirdPartyThemeFlipsToDark()
{
    // 标题栏切换按钮在任何主题下都可用：浅色（含 solarized-light）→ dark，暗色 → light
    CommandStore store;
    ThemeManager theme(&store);

    theme.setTheme(QStringLiteral("solarized-light"));
    theme.toggleTheme();
    QCOMPARE(theme.currentThemeId(), QStringLiteral("dark"));

    theme.setTheme(QStringLiteral("nord"));
    theme.toggleTheme();
    QCOMPARE(theme.currentThemeId(), QStringLiteral("light"));
}

QTEST_MAIN(TestThemeManager) // 有窗口系统才能安全操作应用级样式表
#include "test_thememanager.moc"
