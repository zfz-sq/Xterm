#include "commandstore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

namespace {

// 凭据名带应用名：单测改了 applicationName 就不会碰到用户正在用的 Xterm 凭据。
std::wstring sshPasswordTarget(const QString &profile)
{
    const QString key = QStringLiteral("Xterm/%1/ssh/%2")
                            .arg(QCoreApplication::applicationName(), profile);
    return std::wstring(reinterpret_cast<const wchar_t *>(key.utf16()), key.size());
}

void forgetSshPassword(const QString &profile)
{
    if (profile.isEmpty())
        return;
    const std::wstring target = sshPasswordTarget(profile);
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

// WCAG 相对亮度（readableKeywordColor 用）
qreal wcagLuminance(const QColor &c)
{
    const auto lin = [](int v8) {
        const qreal v = v8 / 255.0;
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
}

bool storeSshPassword(const QString &profile, const QString &password)
{
    if (profile.isEmpty())
        return false;
    const std::wstring target = sshPasswordTarget(profile);
    const QByteArray blob = password.toUtf8();
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = blob.isEmpty()
                              ? nullptr
                              : reinterpret_cast<LPBYTE>(const_cast<char *>(blob.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&cred, 0)) {
        qWarning().noquote()
            << QStringLiteral("Xterm: 无法保存 SSH 密码，错误码 %1").arg(GetLastError());
        return false;
    }
    return true;
}

QString loadSshPassword(const QString &profile)
{
    if (profile.isEmpty())
        return {};
    const std::wstring target = sshPasswordTarget(profile);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) || cred == nullptr)
        return {};
    const QString password = QString::fromUtf8(
        reinterpret_cast<const char *>(cred->CredentialBlob), int(cred->CredentialBlobSize));
    CredFree(cred);
    return password;
}

} // namespace

CommandStore::CommandStore(QObject *parent)
    : QObject(parent)
{
}

QString CommandStore::configPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(base).filePath(QStringLiteral("config.json"));
}

Profile CommandStore::defaultProfile()
{
    Profile p;
    p.name = QStringLiteral("默认");
    p.commands = {
        CommandItem(QStringLiteral("help")),
        CommandItem(QStringLiteral("ls -l")),
        CommandItem(QStringLiteral("uname -a")),
        CommandItem(QStringLiteral("reboot")),
        // 内核日志刷屏（WiFi/蓝牙驱动 printk）会异步插进 vi 的画面字节流，
        // 终端侧无法分离，只能降低控制台日志级别挡住；dmesg 仍可随时查看。
        // 1 = 只允许 KERN_EMERG 上控制台；7 = 恢复默认（全部输出）。
        CommandItem(QStringLiteral("echo 1 > /proc/sys/kernel/printk")),
        CommandItem(QStringLiteral("echo 7 > /proc/sys/kernel/printk")),
    };
    return p;
}

QStringList CommandStore::profileNames() const
{
    QStringList names;
    names.reserve(m_profiles.size());
    for (const Profile &p : m_profiles)
        names << p.name;
    return names;
}

bool CommandStore::hasProfile(const QString &name) const
{
    return std::any_of(m_profiles.cbegin(), m_profiles.cend(),
                       [&](const Profile &p) { return p.name == name; });
}

Profile CommandStore::profile(const QString &name) const
{
    for (const Profile &p : m_profiles)
        if (p.name == name)
            return p;
    return Profile();
}

void CommandStore::upsertProfile(const Profile &p)
{
    for (Profile &cur : m_profiles) {
        if (cur.name == p.name) {
            cur = p;
            return;
        }
    }
    m_profiles.append(p);
}

bool CommandStore::removeProfile(const QString &name)
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].name == name) {
            forgetSshPassword(name);
            m_profiles.removeAt(i);
            if (m_lastProfile == name)
                m_lastProfile = m_profiles.isEmpty() ? QString() : m_profiles.first().name;
            return true;
        }
    }
    return false;
}

static QJsonObject profileToJson(const Profile &p)
{
    QJsonObject c;
    c[QStringLiteral("port")] = p.connection.port;
    c[QStringLiteral("baud")] = p.connection.baud;
    c[QStringLiteral("dataBits")] = p.connection.dataBits;
    c[QStringLiteral("parity")] = p.connection.parity;
    c[QStringLiteral("stopBits")] = p.connection.stopBits;
    c[QStringLiteral("flowControl")] = p.connection.flowControl;
    c[QStringLiteral("dtr")] = p.connection.dtr;
    c[QStringLiteral("rts")] = p.connection.rts;
    c[QStringLiteral("pureTerminal")] = p.connection.pureTerminal;

    QJsonObject s;
    s[QStringLiteral("host")] = p.ssh.host;
    s[QStringLiteral("port")] = p.ssh.port;
    s[QStringLiteral("user")] = p.ssh.user;
    // 密码进 Windows 凭据库，不写进 config.json（和 agent.json 放在同一目录）。
    s[QStringLiteral("privateKey")] = p.ssh.privateKey;
    s[QStringLiteral("connectTimeoutSec")] = p.ssh.connectTimeoutSec;
    s[QStringLiteral("extraArgs")] = p.ssh.extraArgs;

    QJsonArray cmds;
    for (const CommandItem &ci : p.commands)
        cmds.append(QJsonObject{ { QStringLiteral("text"), ci.text } });

    return QJsonObject{
        { QStringLiteral("name"), p.name },
        { QStringLiteral("connection"), c },
        { QStringLiteral("ssh"), s },
        { QStringLiteral("commands"), cmds },
    };
}

static Profile profileFromJson(const QJsonObject &o)
{
    Profile p;
    p.name = o.value(QStringLiteral("name")).toString();
    const QJsonObject c = o.value(QStringLiteral("connection")).toObject();
    p.connection.port = c.value(QStringLiteral("port")).toString();
    p.connection.baud = c.value(QStringLiteral("baud")).toInt(115200);
    p.connection.dataBits = c.value(QStringLiteral("dataBits")).toInt(8);
    p.connection.parity = c.value(QStringLiteral("parity")).toString(QStringLiteral("none"));
    p.connection.stopBits = c.value(QStringLiteral("stopBits")).toDouble(1.0);
    p.connection.flowControl =
        (c.value(QStringLiteral("flowControl")).toString() == QStringLiteral("hardware"))
            ? QStringLiteral("hardware")
            : QStringLiteral("none");
    p.connection.dtr = c.value(QStringLiteral("dtr")).toBool(true);
    p.connection.rts = c.value(QStringLiteral("rts")).toBool(true);
    p.connection.pureTerminal = c.value(QStringLiteral("pureTerminal")).toBool(true);

    const QJsonObject s = o.value(QStringLiteral("ssh")).toObject();
    p.ssh.host = s.value(QStringLiteral("host")).toString();
    p.ssh.port = s.value(QStringLiteral("port")).toInt(22);
    p.ssh.user = s.value(QStringLiteral("user")).toString();
    // 旧文件里的 password 由 load() 迁进凭据库，这里不再读进内存。
    p.ssh.privateKey = s.value(QStringLiteral("privateKey")).toString();
    p.ssh.connectTimeoutSec = s.value(QStringLiteral("connectTimeoutSec")).toInt(10);
    p.ssh.extraArgs = s.value(QStringLiteral("extraArgs")).toString();

    const QJsonArray cmds = o.value(QStringLiteral("commands")).toArray();
    for (const QJsonValue &v : cmds) {
        const QString text = v.toObject().value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            p.commands.append(CommandItem(text));
    }
    return p;
}

bool CommandStore::isDarkTheme() const
{
    return m_theme != QStringLiteral("light") && m_theme != QStringLiteral("solarized-light");
}

void CommandStore::setDarkTheme(bool dark)
{
    if (dark) {
        if (!isDarkTheme())
            m_theme = QStringLiteral("dark");
    } else {
        if (isDarkTheme())
            m_theme = QStringLiteral("light");
    }
}

bool CommandStore::load()
{
    QFile file(configPath());
    if (!file.exists()) {
        m_profiles = { defaultProfile() };
        m_lastProfile = m_profiles.first().name;
        m_theme = QStringLiteral("dark");
        m_mode = QStringLiteral("serial");
        save();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    file.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    const QJsonObject root = doc.object();
    m_profiles.clear();
    bool stripLegacyPasswords = false;
    const QJsonArray arr = root.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        Profile p = profileFromJson(obj);
        if (p.name.isEmpty())
            continue;
        const QString legacy = obj.value(QStringLiteral("ssh")).toObject()
                                   .value(QStringLiteral("password")).toString();
        const QString stored = loadSshPassword(p.name);
        if (!stored.isEmpty())
            p.ssh.password = stored;
        else if (!legacy.isEmpty()) {
            storeSshPassword(p.name, legacy);
            p.ssh.password = legacy;
        }
        if (!legacy.isEmpty())
            stripLegacyPasswords = true;
        m_profiles.append(p);
    }
    if (m_profiles.isEmpty())
        m_profiles = { defaultProfile() };

    m_lastProfile = root.value(QStringLiteral("lastProfile")).toString();
    if (!hasProfile(m_lastProfile))
        m_lastProfile = m_profiles.first().name;

    const QString theme = root.value(QStringLiteral("theme")).toString();
    m_theme = theme.isEmpty() ? QStringLiteral("dark") : theme;
    const QString mode = root.value(QStringLiteral("mode")).toString();
    m_mode = (mode == QStringLiteral("ssh")) ? QStringLiteral("ssh")
                                             : QStringLiteral("serial");
    m_fontSize = qBound(0, root.value(QStringLiteral("fontSize")).toInt(0), 72);
    m_terminalFont = root.value(QStringLiteral("terminalFont")).toString();
    m_sidebarVisible = root.value(QStringLiteral("sidebarVisible")).toBool(true);
    m_statusBarVisible = root.value(QStringLiteral("statusBarVisible")).toBool(true);
    setOsc52Policy(root.value(QStringLiteral("osc52Policy")).toString(QStringLiteral("ask")));
    m_selectToCopy = root.value(QStringLiteral("selectToCopy")).toBool(false);
    setMaxScrollback(root.value(QStringLiteral("maxScrollback")).toInt(10000));
    setMcpAllowWrite(root.value(QStringLiteral("mcpAllowWrite")).toBool(false));
    setSerialSlowPaste(root.value(QStringLiteral("serialSlowPaste")).toBool(true));
    setSerialSlowPasteDelay(root.value(QStringLiteral("serialSlowPasteDelay")).toInt(50));
    setSerialAutoReconnect(root.value(QStringLiteral("serialAutoReconnect")).toBool(true));
    setSilenceAlert(root.value(QStringLiteral("silenceAlert")).toBool(true));
    setSilenceAlertSeconds(root.value(QStringLiteral("silenceAlertSeconds")).toInt(5));

    m_keywords.clear();
    // 老配置没有 keywords 键 = 从没用过高亮：给一套日志惯例默认规则
    if (!root.contains(QStringLiteral("keywords")))
        m_keywords = defaultKeywords(isDarkTheme());
    const QJsonArray kws = root.value(QStringLiteral("keywords")).toArray();
    for (const QJsonValue &v : kws) {
        const QJsonObject o = v.toObject();
        KeywordRule rule;
        rule.text = o.value(QStringLiteral("text")).toString();
        rule.color = QColor(o.value(QStringLiteral("color")).toString());
        rule.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        rule.themeSlot = o.value(QStringLiteral("themeSlot")).toInt(-1);
        if (!rule.text.isEmpty() && rule.color.isValid())
            m_keywords.append(rule);
    }
    // 旧配置迁移：文本与内置默认规则相同、颜色也没改过的规则补上槽位，
    // 让这批高亮随主题调色板换色；改过色的（含同词异色）保持固定色
    for (KeywordRule &rule : m_keywords) {
        if (rule.themeSlot >= 0)
            continue;
        for (int variant = 0; variant < 2; ++variant) {
            for (const KeywordRule &def : defaultKeywords(variant == 0)) {
                if (rule.text.compare(def.text, Qt::CaseInsensitive) == 0
                        && rule.color == def.color) {
                    rule.themeSlot = def.themeSlot;
                    break;
                }
            }
            if (rule.themeSlot >= 0)
                break;
        }
    }
    // 旧配置把密码明文写在 json 里：迁进凭据库后立刻重写，把字段删掉。
    if (stripLegacyPasswords)
        save();
    return true;
}

QColor CommandStore::classicKeywordColor(int slot, bool dark)
{
    switch (slot) {
    case 1: return dark ? QColor(0xE0, 0x6C, 0x75) : QColor(0xC4, 0x2F, 0x2F);
    case 2: return dark ? QColor(0x7C, 0xD0, 0x77) : QColor(0x0E, 0x8A, 0x3E);
    case 3: return dark ? QColor(0xE5, 0xC0, 0x7A) : QColor(0xB5, 0x89, 0x00);
    case 6: return dark ? QColor(0x56, 0xB6, 0xC2) : QColor(0x11, 0x8A, 0xB3);
    default: return QColor();
    }
}

QColor CommandStore::readableKeywordColor(const QColor &color, const QColor &bg)
{
    if (!color.isValid() || !bg.isValid())
        return color;
    // 进程级缓存：键 = 原色|底色。绘制端逐行取色，规则虽少但全量重绘行数
    // 可达滚动历史上限，pow 换算不能每行重来（都在 GUI 线程，无并发）
    static QHash<QString, QColor> cache;
    const QString key = color.name() + QLatin1Char('|') + bg.name();
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return hit.value();

    const qreal bgL = wcagLuminance(bg);
    const auto contrast = [&bgL](const QColor &c) {
        const qreal l = wcagLuminance(c);
        return (qMax(l, bgL) + 0.05) / (qMin(l, bgL) + 0.05);
    };
    QColor result = color;
    // 3:1 = WCAG 图形/大字下限：关键词高亮是辅助信息，够「可辨」即可；
    // 阈值再高会把内置 light 的 Solarized 黄（白底 3.2:1）也改掉，破坏零变化
    constexpr qreal kMinContrast = 3.0;
    if (contrast(color) < kMinContrast) {
        float h, s, l, a; // Qt6 getHslF/fromHslF 用 float 通道
        color.getHslF(&h, &s, &l, &a);
        // 压暗/提亮两个方向各试一遍，取改动最小的达标者（亮底压暗、暗底
        // 提亮通常步数更少，但中灰底两向都可行，交由步数自然选择）
        QColor best;
        qreal bestDl = 1.0;
        for (int dir = 0; dir < 2; ++dir) {
            const qreal step = dir == 0 ? -0.02 : 0.02;
            qreal l2 = l;
            for (int i = 0; i < 50; ++i) {
                l2 += step;
                if (l2 < 0.0 || l2 > 1.0)
                    break;
                const QColor cand = QColor::fromHslF(h, s, l2, a);
                if (contrast(cand) >= kMinContrast) {
                    const qreal dl = qAbs(l2 - l);
                    if (dl < bestDl) {
                        bestDl = dl;
                        best = cand;
                    }
                    break;
                }
            }
        }
        if (best.isValid())
            result = best; // 两向都不达标（极端底色）：保持原色不强改黑白
    }
    if (cache.size() > 512) // 防御性上限：规则×主题组合正常远达不到
        cache.clear();
    cache.insert(key, result);
    return result;
}

QList<KeywordRule> CommandStore::defaultKeywords(bool dark)
{
    // 颜色与终端调色板同源（暗 = One Dark 系，亮 = kLight 调色板）；
    // themeSlot 指向调色板 normal 槽位，切主题时高亮随调色板重解析
    const QColor red = classicKeywordColor(1, dark);
    const QColor green = classicKeywordColor(2, dark);
    const QColor yellow = classicKeywordColor(3, dark);
    const QColor cyan = classicKeywordColor(6, dark);
    auto make = [](const char *text, const QColor &color, int slot) {
        KeywordRule rule;
        rule.text = QString::fromLatin1(text);
        rule.color = color;
        rule.themeSlot = slot;
        return rule;
    };
    // 匹配是不区分大小写的子串，用词要避开常见误命中：用 passed 而非 pass
    // （passwd/bypass），用 disconnect 而非 connected（会把 disconnected 染绿），
    // 不收 ok（token/socket）、ready（already）、enabled（disabled）、
    // complete（incomplete）、oops（loops）、bug（debug）。
    return {
        make("error", red, 1),     make("fail", red, 1),  make("fatal", red, 1),
        make("panic", red, 1),     make("denied", red, 1), make("disconnect", red, 1),
        make("false", red, 1),     make("critical", red, 1), make("refused", red, 1),
        make("unable", red, 1),    make("invalid", red, 1), make("segfault", red, 1),
        make("segmentation", red, 1),
        make("success", green, 2), make("true", green, 2), make("passed", green, 2),
        make("done", green, 2),
        make("warn", yellow, 3),   make("timeout", yellow, 3), make("deprecated", yellow, 3),
        make("retry", yellow, 3),
        make("info", cyan, 6),     make("debug", cyan, 6),
    };
}

bool CommandStore::save() const
{
    for (const Profile &p : m_profiles) {
        if (p.ssh.password.isEmpty())
            forgetSshPassword(p.name);
        else
            storeSshPassword(p.name, p.ssh.password);
    }

    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonArray arr;
    for (const Profile &p : m_profiles)
        arr.append(profileToJson(p));

    QJsonArray kws;
    for (const KeywordRule &r : m_keywords) {
        QJsonObject o{ { QStringLiteral("text"), r.text },
                       { QStringLiteral("color"), r.color.name() },
                       { QStringLiteral("enabled"), r.enabled } };
        if (r.themeSlot >= 0)
            o.insert(QStringLiteral("themeSlot"), r.themeSlot);
        kws.append(o);
    }

    const QJsonObject root{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("lastProfile"), m_lastProfile },
        { QStringLiteral("theme"), m_theme },
        { QStringLiteral("mode"), m_mode },
        { QStringLiteral("fontSize"), m_fontSize },
        { QStringLiteral("terminalFont"), m_terminalFont },
        { QStringLiteral("sidebarVisible"), m_sidebarVisible },
        { QStringLiteral("statusBarVisible"), m_statusBarVisible },
        { QStringLiteral("osc52Policy"), m_osc52Policy },
        { QStringLiteral("selectToCopy"), m_selectToCopy },
        { QStringLiteral("maxScrollback"), m_maxScrollback },
        { QStringLiteral("mcpAllowWrite"), m_mcpAllowWrite },
        { QStringLiteral("serialSlowPaste"), m_serialSlowPaste },
        { QStringLiteral("serialSlowPasteDelay"), m_serialSlowPasteDelay },
        { QStringLiteral("serialAutoReconnect"), m_serialAutoReconnect },
        { QStringLiteral("silenceAlert"), m_silenceAlert },
        { QStringLiteral("silenceAlertSeconds"), m_silenceAlertSeconds },
        { QStringLiteral("keywords"), kws },
        { QStringLiteral("profiles"), arr },
    };

    // 原子写：QSaveFile 先写临时文件，commit() 时一次性替换正式文件。
    // 直接 Truncate 写会在崩溃/断电时留下半截 JSON，下次启动整个配置就丢了。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit(); // 失败时自动清理临时文件，正式文件保持原样
}
