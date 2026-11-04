#include "sessionbridge.h"

void SessionBridge::registerTools(AgentBridge::Hooks *hooks)
{
    Q_ASSERT(hooks);
    if (!m_hooks.list || !m_hooks.current || !m_hooks.switchTo || !m_hooks.openSsh
        || !m_hooks.openSerial || !m_hooks.close) {
        return; // 后端未注入：六件一起缺席，不注册半套
    }

    hooks->tools.insert(QStringLiteral("xterm.session.list"),
                        [this](const AgentBridge::ToolCall &) -> AgentBridge::ToolResult {
                            return {true, false,
                                    QJsonObject{{QStringLiteral("sessions"), m_hooks.list()}},
                                    QString()};
                        });
    hooks->tools.insert(
        QStringLiteral("xterm.session.current"),
        [this](const AgentBridge::ToolCall &) -> AgentBridge::ToolResult {
            return {true, false, m_hooks.current(), QString()};
        });
    hooks->tools.insert(
        QStringLiteral("xterm.session.switch"),
        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
            QJsonObject out;
            const QString err = m_hooks.switchTo(
                call.params.value(QStringLiteral("kind")).toString(), &out);
            if (!err.isEmpty())
                return {false, false, {}, err};
            return {true, false, out, QString()};
        });
    hooks->tools.insert(
        QStringLiteral("xterm.session.openSsh"),
        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
            QJsonObject out;
            const QString err = m_hooks.openSsh(call.params, &out);
            if (!err.isEmpty())
                return {false, false, {}, err};
            return {true, false, out, QString()};
        });
    hooks->tools.insert(
        QStringLiteral("xterm.session.openSerial"),
        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
            QJsonObject out;
            const QString err = m_hooks.openSerial(call.params, &out);
            if (!err.isEmpty())
                return {false, false, {}, err};
            return {true, false, out, QString()};
        });
    hooks->tools.insert(
        QStringLiteral("xterm.session.close"),
        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
            QJsonObject out;
            const QString err =
                m_hooks.close(call.params.value(QStringLiteral("kind")).toString(), &out);
            if (!err.isEmpty())
                return {false, false, {}, err};
            return {true, false, out, QString()};
        });

    // tools/list 元数据（与内置工具同款：name/description/params 扁平结构）
    auto meta = [hooks](const char *name, const QString &desc, QJsonObject params) {
        const QString full = QString::fromLatin1(name);
        QJsonObject m;
        m.insert(QStringLiteral("name"), full);
        m.insert(QStringLiteral("description"), desc);
        m.insert(QStringLiteral("params"), params);
        hooks->toolMeta.insert(full, m);
    };
    const auto stringProp = [](const QString &desc) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                           {QStringLiteral("description"), desc}};
    };
    const auto intProp = [](const QString &desc) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                           {QStringLiteral("description"), desc}};
    };

    meta("xterm.session.list",
         QStringLiteral("列出两条会话链路（串口/SSH）的连接状态与摘要"), {});
    meta("xterm.session.current",
         QStringLiteral("当前活动会话详情（模式、连接状态、摘要，附两条链路）"), {});
    {
        QJsonObject params;
        params.insert(QStringLiteral("kind"), stringProp(QStringLiteral("serial 或 ssh")));
        meta("xterm.session.switch",
             QStringLiteral("切换活动会话：serial 或 ssh。只切显示与发送目标，不断开另一条"),
             params);
    }
    {
        QJsonObject params;
        params.insert(QStringLiteral("host"), stringProp(QStringLiteral("主机名或 IP")));
        params.insert(QStringLiteral("port"), intProp(QStringLiteral("端口，默认 22")));
        params.insert(QStringLiteral("user"), stringProp(QStringLiteral("登录用户名")));
        meta("xterm.session.openSsh",
             QStringLiteral("发起 SSH 连接。参数缺省用界面已配置的；凭据（密码/密钥）"
                            "只认界面配置，不从参数来。已连接时不重复发起，换参数先 close"),
             params);
    }
    {
        QJsonObject params;
        params.insert(QStringLiteral("port"), stringProp(QStringLiteral("端口名，如 COM3")));
        params.insert(QStringLiteral("baud"), intProp(QStringLiteral("波特率，如 115200")));
        meta("xterm.session.openSerial",
             QStringLiteral("打开串口。参数缺省用界面已配置的。已连接时不重复发起，"
                            "换参数先 close"),
             params);
    }
    {
        QJsonObject params;
        params.insert(
            QStringLiteral("kind"),
            stringProp(QStringLiteral("serial、ssh 或留空（当前活动链路）")));
        meta("xterm.session.close",
             QStringLiteral("断开会话。kind 缺省断当前活动链路；serial/ssh 指定其一"),
             params);
    }
}
