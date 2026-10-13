#ifndef SSHSESSION_H
#define SSHSESSION_H

#include <QString>
#include <QStringList>

#include "conpty.h"
#include "sshtarget.h"
#include "transport.h"

/*
 * SshSession: SSH 后端，实现 Transport 契约。
 *
 * 实现方式：在 Windows 伪终端（ConPty）里运行系统自带的 OpenSSH 客户端
 * ssh.exe。子进程面对的是真 tty，因此远端会分配 PTY、开启回显、输出 ANSI
 * 彩色，top/vim 这类全屏程序也照常可用；密码、host key 确认都在终端里由
 * ssh.exe 自己完成，程序不经手也不落盘任何凭据。
 *
 * 与 SerialSession 的差别只在"异步"：open() 返回空串只代表已开始建立，
 * 成功要等 opened()，失败看 errorOccurred()。
 */
class SshSession : public Transport
{
    Q_OBJECT

public:
    explicit SshSession(QObject *parent = nullptr);
    ~SshSession() override;

    Kind kind() const override { return SshLink; }

    // 定位系统 OpenSSH 客户端；找不到返回空字符串。
    static QString sshExecutable();
    static QString availabilityError(); // 空 = 本平台的 SSH 后端可用

    void setPtySize(int cols, int rows);

    QString open(const SshTarget &target);   // 空串 = 已开始建立
    void close() override;
    QString send(const QByteArray &data) override;

    SshTarget target() const { return m_target; }
    QString summary() const;                 // "user@host:port"，用于状态栏 / 提示

signals:
    // 会话结束的原因码，供上层在终端里写一行人类可读的说明。
    void sessionEnded(int exitCode, bool userRequested);

private:
    void onLaunched(const QString &errorText);
    void onFinished(int exitCode, bool terminatedByUs);
    void beginClose();
    QStringList buildArguments() const;
    static QString exitHint(int exitCode);

    ConPty m_pty;
    SshTarget m_target;
    int m_cols = 80;
    int m_rows = 24;
    bool m_userRequestedClose = false;
    // 已连接时再次 open()：先收旧会话，closed() 之后自动接着建新会话，
    // 全程不阻塞（不做「断开—等待—重连」那种会让 UI 卡住的同步等待）。
    SshTarget m_pendingReopen;
    bool m_hasPendingReopen = false;
};

#endif // SSHSESSION_H
