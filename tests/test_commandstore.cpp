#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "commandstore.h"

/*
 * SSH 密码不得进 config.json。保存后文件里没有 password 字段，
 * 私钥路径仍在；重新 load 后密码从凭据库回到内存。
 * 应用名带 pid，避免碰到用户真实的 Xterm 配置目录。
 */
class CommandStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void passwordRoundTripSkipsConfigFile();
    void legacyPasswordMigratesOutOfFile();
    void themeIdRoundTripsThroughConfig();
    void statusBarVisibilityRoundTripsThroughConfig();
    void serialBehaviorSettingsRoundTrip();
    void keywordSlotsFollowDefaultRules();
    void legacyKeywordsGainThemeSlotOnLoad();
    void themeSlotRoundTripsThroughConfig();
    void keywordColorAdaptsToBackground();

private:
    QJsonObject sshOfFirstProfile() const;
};

void CommandStoreTest::initTestCase()
{
    QCoreApplication::setOrganizationName(QStringLiteral("XtermUnit"));
    QCoreApplication::setApplicationName(
        QStringLiteral("XtermCmd-%1").arg(QCoreApplication::applicationPid()));
}

void CommandStoreTest::cleanupTestCase()
{
    CommandStore store;
    if (store.load()) {
        const QStringList names = store.profileNames();
        for (const QString &name : names)
            store.removeProfile(name);
        store.save();
    }
    QFile::remove(CommandStore::configPath());
}

void CommandStoreTest::keywordColorAdaptsToBackground()
{
    // 多主题可读性兑底：对比度足的零变化，不足的保持色相只调亮度

    // 1. 内置 dark 经典色画在 dark 终端底上（对比 ~5:1）：原样返回
    const QColor classicRed = CommandStore::classicKeywordColor(1, true);
    QCOMPARE(CommandStore::readableKeywordColor(classicRed, QColor(0x1E, 0x1E, 0x1E)),
             classicRed);

    // 内置 light 经典色画在白底上（红 ~5.5:1 / 黄 3.2:1 都达标）：原样返回
    const QColor classicYellow = CommandStore::classicKeywordColor(3, false);
    QCOMPARE(CommandStore::readableKeywordColor(classicYellow, QColor(0xFF, 0xFF, 0xFF)),
             classicYellow);

    // 2. 暗底亮色系画在白底上（右键高亮/改色规则切浅色主题的典型场景）：
    //    暗系经典黄 0xE5C07A 白底对比仅 ~1.75:1，必须被压暗到可辨
    const QColor paleYellow = CommandStore::classicKeywordColor(3, true);
    const QColor fixed = CommandStore::readableKeywordColor(paleYellow, QColor(0xFF, 0xFF, 0xFF));
    QVERIFY(fixed != paleYellow);
    QVERIFY(fixed.lightness() < paleYellow.lightness()); // 压暗而非提亮
    int fixedHue = 0, origHue = 0;
    fixed.getHsv(&fixedHue, nullptr, nullptr);
    paleYellow.getHsv(&origHue, nullptr, nullptr);
    QVERIFY(qAbs(fixedHue - origHue) <= 2); // 色相保留（黄还是黄）

    // 3. 暗底亮紫画在暗底上不受影响，画在白底上被压暗
    const QColor palePurple(0xC6, 0x78, 0xDD); // 新词轮换色板之一
    QCOMPARE(CommandStore::readableKeywordColor(palePurple, QColor(0x1E, 0x1E, 0x1E)),
             palePurple); // 暗底 ~5:1 达标
    QVERIFY(CommandStore::readableKeywordColor(palePurple, QColor(0xFF, 0xFF, 0xFF))
            != palePurple); // 白底 ~2.9:1 需调整

    // 4. 结果稳定（缓存命中路径）与无效参数透传
    QCOMPARE(CommandStore::readableKeywordColor(paleYellow, QColor(0xFF, 0xFF, 0xFF)), fixed);
    QVERIFY(!CommandStore::readableKeywordColor(QColor(), QColor(0xFF, 0xFF, 0xFF)).isValid());
    QCOMPARE(CommandStore::readableKeywordColor(paleYellow, QColor()), paleYellow);
}

QJsonObject CommandStoreTest::sshOfFirstProfile() const
{
    QFile file(CommandStore::configPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonArray profiles = QJsonDocument::fromJson(file.readAll()).object()
                                    .value(QStringLiteral("profiles")).toArray();
    if (profiles.isEmpty())
        return {};
    return profiles.at(0).toObject().value(QStringLiteral("ssh")).toObject();
}

void CommandStoreTest::passwordRoundTripSkipsConfigFile()
{
    CommandStore store;
    Profile profile = CommandStore::defaultProfile();
    profile.name = QStringLiteral("board");
    profile.ssh.host = QStringLiteral("10.0.0.8");
    profile.ssh.user = QStringLiteral("root");
    profile.ssh.password = QStringLiteral("s3cret");
    profile.ssh.privateKey = QStringLiteral("C:/keys/id_rsa");
    store.upsertProfile(profile);
    store.setLastProfile(profile.name);
    QVERIFY(store.save());

    const QJsonObject ssh = sshOfFirstProfile();
    QVERIFY(!ssh.contains(QStringLiteral("password")));
    QCOMPARE(ssh.value(QStringLiteral("privateKey")).toString(),
             QStringLiteral("C:/keys/id_rsa"));
    QCOMPARE(ssh.value(QStringLiteral("host")).toString(), QStringLiteral("10.0.0.8"));

    CommandStore again;
    QVERIFY(again.load());
    QCOMPARE(again.profile(QStringLiteral("board")).ssh.password, QStringLiteral("s3cret"));
    QCOMPARE(again.profile(QStringLiteral("board")).ssh.privateKey,
             QStringLiteral("C:/keys/id_rsa"));
}

void CommandStoreTest::legacyPasswordMigratesOutOfFile()
{
    const QString path = CommandStore::configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArrayLiteral(
        "{"
        "\"version\":1,"
        "\"lastProfile\":\"legacy-board\","
        "\"profiles\":[{\"name\":\"legacy-board\",\"ssh\":{"
        "\"host\":\"10.0.0.9\",\"user\":\"root\","
        "\"password\":\"legacy-secret\","
        "\"privateKey\":\"C:/keys/id_ed25519\""
        "},\"connection\":{},\"commands\":[]}]"
        "}"));
    file.close();

    CommandStore store;
    QVERIFY(store.load());
    QCOMPARE(store.profile(QStringLiteral("legacy-board")).ssh.password,
             QStringLiteral("legacy-secret"));
    QCOMPARE(store.profile(QStringLiteral("legacy-board")).ssh.privateKey,
             QStringLiteral("C:/keys/id_ed25519"));

    const QJsonObject ssh = sshOfFirstProfile();
    QVERIFY(!ssh.contains(QStringLiteral("password")));
    QCOMPARE(ssh.value(QStringLiteral("privateKey")).toString(),
             QStringLiteral("C:/keys/id_ed25519"));
}

void CommandStoreTest::themeIdRoundTripsThroughConfig()
{
    // 主题存的是完整 id（dracula/nord/...），不再是 dark|light 二选一；
    // 回读后 isDarkTheme 要按 id 归属判定，而非字面比较
    CommandStore store;
    QVERIFY(store.load());
    store.setTheme(QStringLiteral("dracula"));
    QVERIFY(store.save());

    CommandStore again;
    QVERIFY(again.load());
    QCOMPARE(again.theme(), QStringLiteral("dracula"));
    QVERIFY(again.isDarkTheme());

    again.setTheme(QStringLiteral("solarized-light"));
    QVERIFY(!again.isDarkTheme());

    // setDarkTheme 保持“翻到代表色”语义：不会把 dracula 改写成 dark
    again.setTheme(QStringLiteral("monokai"));
    again.setDarkTheme(true);
    QCOMPARE(again.theme(), QStringLiteral("monokai"));
    again.setDarkTheme(false);
    QCOMPARE(again.theme(), QStringLiteral("light"));
}

void CommandStoreTest::serialBehaviorSettingsRoundTrip()
{
    // 慢速粘贴/自动重连/静默提醒：非默认值落盘后重新 load 要原样回来
    CommandStore store;
    QVERIFY(store.load());
    store.setSerialSlowPaste(false);
    store.setSerialSlowPasteDelay(120);
    store.setSerialAutoReconnect(false);
    store.setSilenceAlert(false);
    store.setSilenceAlertSeconds(30);
    QVERIFY(store.save());

    CommandStore again;
    QVERIFY(again.load());
    QCOMPARE(again.serialSlowPaste(), false);
    QCOMPARE(again.serialSlowPasteDelay(), 120);
    QCOMPARE(again.serialAutoReconnect(), false);
    QCOMPARE(again.silenceAlert(), false);
    QCOMPARE(again.silenceAlertSeconds(), 30);

    // 边界钳制：延时不低于 10ms（防 0ms 等于没设），静默秒数不低于 1
    again.setSerialSlowPasteDelay(0);
    QCOMPARE(again.serialSlowPasteDelay(), 10);
    again.setSilenceAlertSeconds(0);
    QCOMPARE(again.silenceAlertSeconds(), 1);
}

void CommandStoreTest::statusBarVisibilityRoundTripsThroughConfig()
{
    CommandStore store;
    QVERIFY(store.load());
    store.setStatusBarVisible(false);
    QVERIFY(store.save());

    CommandStore again;
    QVERIFY(again.load());
    QCOMPARE(again.statusBarVisible(), false);

    again.setStatusBarVisible(true);
    QVERIFY(again.save());
    CommandStore restored;
    QVERIFY(restored.load());
    QCOMPARE(restored.statusBarVisible(), true);
}

void CommandStoreTest::keywordSlotsFollowDefaultRules()
{
    // 默认规则带调色板槽位（1红/2绿/3黄/6青）；固定色与经典色接口同源。
    // 亮系经典色与亮调色板槽位一致，暗系是独立的 One Dark 色（历史上就
    // 与终端调色板不同源，内置主题零变化靠经典色兑底）
    const QList<KeywordRule> darkRules = CommandStore::defaultKeywords(true);
    QVERIFY(darkRules.size() > 10);
    for (const KeywordRule &rule : darkRules) {
        QVERIFY(rule.themeSlot == 1 || rule.themeSlot == 2
                || rule.themeSlot == 3 || rule.themeSlot == 6);
        QCOMPARE(rule.color, CommandStore::classicKeywordColor(rule.themeSlot, true));
    }
    auto slotOf = [&darkRules](const char *text) {
        for (const KeywordRule &r : darkRules) {
            if (r.text == QLatin1String(text))
                return r.themeSlot;
        }
        return -1;
    };
    QCOMPARE(slotOf("error"), 1);
    QCOMPARE(slotOf("success"), 2);
    QCOMPARE(slotOf("warn"), 3);
    QCOMPARE(slotOf("info"), 6);

    // 四个语义槽位两系都有效；其余槽位无效色（调用方回退固定色）
    for (int slot : {1, 2, 3, 6}) {
        QVERIFY(CommandStore::classicKeywordColor(slot, true).isValid());
        QVERIFY(CommandStore::classicKeywordColor(slot, false).isValid());
    }
    for (int slot : {0, 4, 5, 7}) {
        QVERIFY(!CommandStore::classicKeywordColor(slot, true).isValid());
        QVERIFY(!CommandStore::classicKeywordColor(slot, false).isValid());
    }
    QCOMPARE(CommandStore::classicKeywordColor(1, false), QColor(0xC4, 0x2F, 0x2F));
    QCOMPARE(CommandStore::classicKeywordColor(1, true), QColor(0xE0, 0x6C, 0x75));
}

void CommandStoreTest::legacyKeywordsGainThemeSlotOnLoad()
{
    // 旧配置只有固定色：文本/颜色与默认规则一致的补上槽位（之后切主题
    // 随终端调色板换色），改过色或自定义词保持固定色不动
    const QString path = CommandStore::configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QByteArrayLiteral(
        "{" "\"version\":1,"
        "\"keywords\":["
        "{\"text\":\"error\",\"color\":\"#e06c75\",\"enabled\":true},"
        "{\"text\":\"success\",\"color\":\"#7cd077\",\"enabled\":true},"
        "{\"text\":\"warn\",\"color\":\"#ff0000\",\"enabled\":true},"
        "{\"text\":\"myword\",\"color\":\"#e06c75\",\"enabled\":true}"
        "]" "}"));
    file.close();

    CommandStore store;
    QVERIFY(store.load());
    const QList<KeywordRule> rules = store.keywords();
    QCOMPARE(rules.size(), 4);
    QCOMPARE(rules.at(0).themeSlot, 1); // error：dark 经典红 → 槽 1
    QCOMPARE(rules.at(1).themeSlot, 2); // success：dark 经典绿 → 槽 2
    QCOMPARE(rules.at(2).themeSlot, -1); // warn 改过色 → 固定色
    QCOMPARE(rules.at(3).themeSlot, -1); // 自定义词 → 固定色
}

void CommandStoreTest::themeSlotRoundTripsThroughConfig()
{
    // 槽位字段落盘后回读不丢；固定色规则不落 themeSlot 字段（旧文件兼容）
    CommandStore store;
    QVERIFY(store.load());
    QList<KeywordRule> rules;
    KeywordRule slotted;
    slotted.text = QStringLiteral("error");
    slotted.color = QColor(0xE0, 0x6C, 0x75);
    slotted.themeSlot = 1;
    KeywordRule fixed;
    fixed.text = QStringLiteral("warn");
    fixed.color = QColor(0xFF, 0x00, 0x00);
    rules << slotted << fixed;
    store.setKeywords(rules);
    QVERIFY(store.save());

    CommandStore again;
    QVERIFY(again.load());
    const QList<KeywordRule> back = again.keywords();
    QCOMPARE(back.size(), 2);
    QCOMPARE(back.at(0).themeSlot, 1);
    QCOMPARE(back.at(0).color, QColor(0xE0, 0x6C, 0x75));
    QCOMPARE(back.at(1).themeSlot, -1);
}

QTEST_MAIN(CommandStoreTest)
#include "test_commandstore.moc"
