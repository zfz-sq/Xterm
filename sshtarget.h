#ifndef SSHTARGET_H
#define SSHTARGET_H

#include <QString>

/*
 * SshTarget: 建立一条 SSH 会话需要的全部参数。
 *
 * 单独一个头文件，是因为它同时被两边使用：CommandStore 按值存进 Profile 做
 * JSON 持久化，SshSession 按值收作连接输入。让持久化层去 include SSH 后端
 * 既不必要也会拖慢编译，纯数据类型独立出来最干净。
 */
struct SshTarget
{
    QString host;                 // 主机名 / IP，也可以是 ~/.ssh/config 里的别名
    int port = 22;
    QString user;                 // 留空则交给 ~/.ssh/config 的 User 或系统用户名推断
    QString password;             // 非空时自动应答终端里的 password 提示；留空走手动输入
    QString privateKey;           // 留空走默认密钥 / 密码交互
    int connectTimeoutSec = 10;
    QString extraArgs;            // 旧配置里的字段，界面已去掉，连接时不再使用
};

#endif // SSHTARGET_H
