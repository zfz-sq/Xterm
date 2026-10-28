#ifndef COMMANDSTORE_H
#define COMMANDSTORE_H

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>

#include "sshtarget.h" // SshTarget: SSH 连接参数，同时也是持久化字段

struct CommandItem
{
    QString text;

    CommandItem() = default;
    explicit CommandItem(const QString &t) : text(t) {}
};

// 终端关键词高亮规则：命中（不区分大小写）的文本按配置颜色绘制
struct KeywordRule
{
    QString text;
    QColor color = QColor(0xE0, 0x6C, 0x75); // 默认暖红，报错类关键词常用
    bool enabled = true;
};

struct ConnectionConfig
{
    QString port;
    int baud = 115200;
    int dataBits = 8;
    QString parity = QStringLiteral("none"); // none / even / odd
    double stopBits = 1.0;                   // 1 / 1.5 / 2
    QString flowControl = QStringLiteral("none"); // none / hardware（RTS/CTS）
    bool dtr = true; // 打开串口后的 DTR 电平（false 适合 DTR 接复位电路的板子）
    bool rts = true;
};

struct Profile
{
    QString name;
    ConnectionConfig connection; // 串口侧参数
    SshTarget ssh;               // SSH 侧参数，与串口平级，互不影响
    QList<CommandItem> commands;
};

/*
 * CommandStore: 本地 JSON 配置的读写。
 * 一个配置(Profile) = 串口参数 + SSH 参数 + 预设命令列表。
 * 文件位于 AppConfigLocation/config.json，启动时恢复 lastProfile 与上次所在模式。
 */
class CommandStore : public QObject
{
    Q_OBJECT

public:
    explicit CommandStore(QObject *parent = nullptr);

    static QString configPath();

    bool load();          // 失败时保持默认配置
    bool save() const;

    QList<Profile> profiles() const { return m_profiles; }
    QStringList profileNames() const;
    bool hasProfile(const QString &name) const;
    Profile profile(const QString &name) const;
    void upsertProfile(const Profile &p);       // 不存在则新建
    bool removeProfile(const QString &name);

    QString lastProfile() const { return m_lastProfile; }
    void setLastProfile(const QString &name) { m_lastProfile = name; }

    // 主题: "dark" / "light"
    bool isDarkTheme() const { return m_theme != QStringLiteral("light"); }
    void setDarkTheme(bool dark) { m_theme = dark ? QStringLiteral("dark") : QStringLiteral("light"); }

    // 终端字号（pt，0 = 未设置，用系统默认）。全局设置，两页共用
    int fontSize() const { return m_fontSize; }
    void setFontSize(int pt) { m_fontSize = pt; }

    // 命令栏收起状态（true = 整栏收起，只剩右侧竖排按钮组）
    bool commandBarCollapsed() const { return m_commandBarCollapsed; }
    void setCommandBarCollapsed(bool collapsed) { m_commandBarCollapsed = collapsed; }

    // 终端关键词高亮规则（全局，串口 / SSH 两页共用）
    QList<KeywordRule> keywords() const { return m_keywords; }
    void setKeywords(const QList<KeywordRule> &rules) { m_keywords = rules; }

    // 上次使用的工作模式: "serial" / "ssh"
    QString mode() const { return m_mode; }
    void setMode(const QString &mode)
    {
        m_mode = (mode == QStringLiteral("ssh")) ? QStringLiteral("ssh")
                                                 : QStringLiteral("serial");
    }

    static Profile defaultProfile();

private:
    QList<Profile> m_profiles;
    QString m_lastProfile;
    QString m_theme = QStringLiteral("dark");
    QString m_mode = QStringLiteral("serial");
    int m_fontSize = 0;
    bool m_commandBarCollapsed = false;
    QList<KeywordRule> m_keywords;
};

#endif // COMMANDSTORE_H
