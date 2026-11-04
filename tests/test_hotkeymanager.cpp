// M1.b HotkeyManager 协作类测试：快捷键注册、触发→信号回抛、文案联动。
// 不 new MainWindow——只验证协作类自身的窄职责与信号契约。
#include <QAction>
#include <QKeySequence>
#include <QSignalSpy>
#include <QTest>

#include "hotkeymanager.h"

class TestHotkeyManager : public QObject
{
    Q_OBJECT

private slots:
    void actionsCreatedWithShortcuts();
    void connectTriggerEmitsSignal();
    void switchModeTriggerEmitsSignal();
    void sftpTriggerEmitsSignal();
    void textSettersUpdateActions();
};

void TestHotkeyManager::actionsCreatedWithShortcuts()
{
    HotkeyManager hk;
    QVERIFY(hk.connectAction() != nullptr);
    QVERIFY(hk.switchModeAction() != nullptr);
    QVERIFY(hk.sftpAction() != nullptr);

    QCOMPARE(hk.connectAction()->shortcuts(),
             QList<QKeySequence>{QKeySequence(Qt::Key_F5)});
    QCOMPARE(hk.switchModeAction()->shortcuts(),
             QList<QKeySequence>{QKeySequence(Qt::Key_F9)});
    QCOMPARE(hk.sftpAction()->shortcuts(),
             QList<QKeySequence>{QKeySequence(Qt::Key_F10)});
}

void TestHotkeyManager::connectTriggerEmitsSignal()
{
    HotkeyManager hk;
    QSignalSpy spy(&hk, &HotkeyManager::connectRequested);
    hk.connectAction()->trigger();
    QCOMPARE(spy.count(), 1);
}

void TestHotkeyManager::switchModeTriggerEmitsSignal()
{
    HotkeyManager hk;
    QSignalSpy spy(&hk, &HotkeyManager::switchModeRequested);
    hk.switchModeAction()->trigger();
    QCOMPARE(spy.count(), 1);
}

void TestHotkeyManager::sftpTriggerEmitsSignal()
{
    HotkeyManager hk;
    QSignalSpy spy(&hk, &HotkeyManager::sftpRequested);
    hk.sftpAction()->trigger();
    QCOMPARE(spy.count(), 1);
}

void TestHotkeyManager::textSettersUpdateActions()
{
    HotkeyManager hk;
    hk.setConnectText(QStringLiteral("断开"));
    QCOMPARE(hk.connectAction()->text(), QStringLiteral("断开"));

    hk.setSwitchModeText(QStringLiteral("切换到串口"));
    QCOMPARE(hk.switchModeAction()->text(), QStringLiteral("切换到串口"));

    // 其他 action 文案不受影响
    QCOMPARE(hk.sftpAction()->text(), QStringLiteral("SFTP"));
}

QTEST_MAIN(TestHotkeyManager) // QAction 需 QApplication（guiless 下构造会崩）
#include "test_hotkeymanager.moc"
