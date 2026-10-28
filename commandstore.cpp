#include "commandstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

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

    QJsonObject s;
    s[QStringLiteral("host")] = p.ssh.host;
    s[QStringLiteral("port")] = p.ssh.port;
    s[QStringLiteral("user")] = p.ssh.user;
    s[QStringLiteral("password")] = p.ssh.password;
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

    const QJsonObject s = o.value(QStringLiteral("ssh")).toObject();
    p.ssh.host = s.value(QStringLiteral("host")).toString();
    p.ssh.port = s.value(QStringLiteral("port")).toInt(22);
    p.ssh.user = s.value(QStringLiteral("user")).toString();
    p.ssh.password = s.value(QStringLiteral("password")).toString();
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
    const QJsonArray arr = root.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue &v : arr) {
        Profile p = profileFromJson(v.toObject());
        if (!p.name.isEmpty())
            m_profiles.append(p);
    }
    if (m_profiles.isEmpty())
        m_profiles = { defaultProfile() };

    m_lastProfile = root.value(QStringLiteral("lastProfile")).toString();
    if (!hasProfile(m_lastProfile))
        m_lastProfile = m_profiles.first().name;

    const QString theme = root.value(QStringLiteral("theme")).toString();
    m_theme = (theme == QStringLiteral("light")) ? QStringLiteral("light")
                                                 : QStringLiteral("dark");
    const QString mode = root.value(QStringLiteral("mode")).toString();
    m_mode = (mode == QStringLiteral("ssh")) ? QStringLiteral("ssh")
                                             : QStringLiteral("serial");
    m_fontSize = qBound(0, root.value(QStringLiteral("fontSize")).toInt(0), 72);
    m_commandBarCollapsed = root.value(QStringLiteral("commandBarCollapsed")).toBool(false);

    m_keywords.clear();
    const QJsonArray kws = root.value(QStringLiteral("keywords")).toArray();
    for (const QJsonValue &v : kws) {
        const QJsonObject o = v.toObject();
        KeywordRule rule;
        rule.text = o.value(QStringLiteral("text")).toString();
        rule.color = QColor(o.value(QStringLiteral("color")).toString());
        rule.enabled = o.value(QStringLiteral("enabled")).toBool(true);
        if (!rule.text.isEmpty() && rule.color.isValid())
            m_keywords.append(rule);
    }
    return true;
}

bool CommandStore::save() const
{
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonArray arr;
    for (const Profile &p : m_profiles)
        arr.append(profileToJson(p));

    QJsonArray kws;
    for (const KeywordRule &r : m_keywords)
        kws.append(QJsonObject{ { QStringLiteral("text"), r.text },
                                 { QStringLiteral("color"), r.color.name() },
                                 { QStringLiteral("enabled"), r.enabled } });

    const QJsonObject root{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("lastProfile"), m_lastProfile },
        { QStringLiteral("theme"), m_theme },
        { QStringLiteral("mode"), m_mode },
        { QStringLiteral("fontSize"), m_fontSize },
        { QStringLiteral("commandBarCollapsed"), m_commandBarCollapsed },
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
