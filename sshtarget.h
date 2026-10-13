#ifndef SSHTARGET_H
#define SSHTARGET_H

#include <QString>

/*
 * SshTarget: 建立一条 SSH 会话需要的全部参数。
 *
 * 单独一个头文件，是因为它同时被两边使用：CommandStore 按值存进 Profile 做
 * JSON 持久化，SshSession 按值收作连接输入。让持久化层去 include SSH 后端
 * （连带 ConPTY 的 Win32 头）既不必要也会拖慢编译，纯数据类型独立出来最干净。
 */
struct SshTarget
{
    QString host;                 // 主机名 / IP，也可以是 ~/.ssh/config 里的别名
    int port = 22;
    QString user;                 // 留空则由 ssh.exe 用 ~/.ssh/config 或用户名推断
    QString privateKey;           // 留空走默认密钥 / 密码交互
    int connectTimeoutSec = 10;
    QString extraArgs;            // 追加的原生 ssh 选项，按 shell 规则分词
};

#endif // SSHTARGET_H
