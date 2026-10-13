#include "completionengine.h"

#include <QSet>

#include <algorithm>

CompletionEngine::CompletionEngine()
{
    m_builtins = defaultBuiltins();
}

QStringList CompletionEngine::defaultBuiltins()
{
    // 嵌入式 Linux 常见命令 / shell 内建，按使用频次粗排，无分组依赖。
    return {
        // 文件 / 目录
        QStringLiteral("ls"), QStringLiteral("cd"), QStringLiteral("pwd"),
        QStringLiteral("mkdir"), QStringLiteral("rmdir"), QStringLiteral("rm"),
        QStringLiteral("cp"), QStringLiteral("mv"), QStringLiteral("ln"),
        QStringLiteral("touch"), QStringLiteral("stat"), QStringLiteral("tree"),
        // 查看
        QStringLiteral("cat"), QStringLiteral("more"), QStringLiteral("less"),
        QStringLiteral("head"), QStringLiteral("tail"), QStringLiteral("tac"),
        QStringLiteral("rev"), QStringLiteral("od"), QStringLiteral("xxd"),
        QStringLiteral("hexdump"),
        // 文本处理
        QStringLiteral("grep"), QStringLiteral("egrep"), QStringLiteral("fgrep"),
        QStringLiteral("sed"), QStringLiteral("awk"), QStringLiteral("cut"),
        QStringLiteral("tr"), QStringLiteral("sort"), QStringLiteral("uniq"),
        QStringLiteral("wc"), QStringLiteral("tee"), QStringLiteral("xargs"),
        QStringLiteral("paste"), QStringLiteral("join"),
        // 查找
        QStringLiteral("find"), QStringLiteral("locate"), QStringLiteral("which"),
        QStringLiteral("whereis"),
        // 系统 / 进程
        QStringLiteral("uname"), QStringLiteral("hostname"), QStringLiteral("uptime"),
        QStringLiteral("date"), QStringLiteral("cal"), QStringLiteral("who"),
        QStringLiteral("whoami"), QStringLiteral("id"), QStringLiteral("groups"),
        QStringLiteral("ps"), QStringLiteral("top"), QStringLiteral("htop"),
        QStringLiteral("free"), QStringLiteral("kill"), QStringLiteral("killall"),
        QStringLiteral("pkill"), QStringLiteral("nice"), QStringLiteral("renice"),
        QStringLiteral("reboot"), QStringLiteral("shutdown"), QStringLiteral("halt"),
        QStringLiteral("poweroff"), QStringLiteral("logout"), QStringLiteral("exit"),
        QStringLiteral("sleep"),
        // 网络
        QStringLiteral("ping"), QStringLiteral("ping6"), QStringLiteral("traceroute"),
        QStringLiteral("ifconfig"), QStringLiteral("ip"), QStringLiteral("netstat"),
        QStringLiteral("ss"), QStringLiteral("curl"), QStringLiteral("wget"),
        QStringLiteral("telnet"), QStringLiteral("ssh"), QStringLiteral("scp"),
        // 权限
        QStringLiteral("chmod"), QStringLiteral("chown"), QStringLiteral("chgrp"),
        QStringLiteral("umask"), QStringLiteral("sudo"), QStringLiteral("su"),
        // 编辑器
        QStringLiteral("vi"), QStringLiteral("vim"), QStringLiteral("nano"), QStringLiteral("ed"),
        // shell 内建
        QStringLiteral("echo"), QStringLiteral("printf"), QStringLiteral("set"),
        QStringLiteral("export"), QStringLiteral("alias"), QStringLiteral("unalias"),
        QStringLiteral("type"), QStringLiteral("source"), QStringLiteral("history"),
        QStringLiteral("env"), QStringLiteral("read"), QStringLiteral("eval"),
        QStringLiteral("exec"), QStringLiteral("true"), QStringLiteral("false"),
        QStringLiteral("test"),
        // 磁盘
        QStringLiteral("mount"), QStringLiteral("umount"), QStringLiteral("df"),
        QStringLiteral("du"), QStringLiteral("fsck"), QStringLiteral("mkfs"),
        QStringLiteral("fdisk"), QStringLiteral("lsblk"), QStringLiteral("blkid"),
        // 包管理
        QStringLiteral("apt"), QStringLiteral("dpkg"), QStringLiteral("yum"),
        QStringLiteral("dnf"), QStringLiteral("rpm"), QStringLiteral("pacman"),
        QStringLiteral("opkg"),
        // 用户 / 组
        QStringLiteral("useradd"), QStringLiteral("userdel"), QStringLiteral("usermod"),
        QStringLiteral("groupadd"), QStringLiteral("groupdel"), QStringLiteral("groupmod"),
        QStringLiteral("passwd"),
        // shell / busybox
        QStringLiteral("bash"), QStringLiteral("sh"), QStringLiteral("ash"),
        QStringLiteral("dash"), QStringLiteral("zsh"), QStringLiteral("busybox"),
        QStringLiteral("help"), QStringLiteral("man"),
    };
}

void CompletionEngine::setUserCommands(const QStringList &cmds)
{
    m_user.clear();
    for (const QString &raw : cmds) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty())
            continue;
        // 补全只关心命令名（第一个 token），"ls -l" 也算 ls
        const int sp = trimmed.indexOf(QChar(' '));
        const QString name = sp < 0 ? trimmed : trimmed.left(sp);
        if (!name.isEmpty())
            m_user << name;
    }
}

QStringList CompletionEngine::complete(const QString &prefix) const
{
    QStringList out;
    QSet<QString> seen;
    auto add = [&](const QString &s) {
        if (s.isEmpty())
            return;
        if (prefix.isEmpty() || s.startsWith(prefix, Qt::CaseInsensitive)) {
            if (!seen.contains(s)) {
                seen.insert(s);
                out << s;
            }
        }
    };
    // 用户命令优先（前置），再补全内置
    for (const QString &u : m_user)
        add(u);
    for (const QString &b : m_builtins)
        add(b);
    std::sort(out.begin(), out.end(), [](const QString &x, const QString &y) {
        return x.compare(y, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString CompletionEngine::longestCommonPrefix(const QStringList &items)
{
    if (items.isEmpty())
        return {};
    if (items.size() == 1)
        return items.first();
    QString p = items.first();
    for (int i = 1; i < items.size(); ++i) {
        const QString &s = items[i];
        int j = 0;
        const int n = qMin(p.size(), s.size());
        while (j < n && p.at(j).toLower() == s.at(j).toLower())
            ++j;
        p = p.left(j);
        if (p.isEmpty())
            break;
    }
    return p;
}