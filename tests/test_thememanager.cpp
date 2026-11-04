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
    void isDarkTracksStore();
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

QTEST_MAIN(TestThemeManager) // 有窗口系统才能安全操作应用级样式表
#include "test_thememanager.moc"
