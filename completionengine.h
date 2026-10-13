#ifndef COMPLETIONENGINE_H
#define COMPLETIONENGINE_H

#include <QStringList>

/*
 * 客户端自动补全数据源（Tab 触发）：
 *  - 内置常用 Linux / shell 命令（静态列表）
 *  - 运行时可注入的用户命令（取 CommandBar 中每条命令的第一个 token）
 *  - 匹配规则：按前缀、不区分大小写；返回排序去重后的候选
 *  - longestCommonPrefix：取候选项的最大公共前缀（用于"补到不能再补"）
 *
 * 不试图猜测设备上的文件系统内容；如需路径补全，让设备自行 echo Tab。
 */
class CompletionEngine
{
public:
    CompletionEngine();

    void setUserCommands(const QStringList &cmds);

    QStringList complete(const QString &prefix) const;

    static QString longestCommonPrefix(const QStringList &items);

    static QStringList defaultBuiltins();

private:
    QStringList m_builtins;
    QStringList m_user;
};

#endif // COMPLETIONENGINE_H