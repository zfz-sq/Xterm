#ifndef HOSTSTATS_H
#define HOSTSTATS_H

#include <QObject>
#include <QString>
#include <functional>

class QThread;
class QTimer;

#include "sshtarget.h"

/*
 * HostStats: 给 SSH 状态栏采样远端 CPU 和内存。
 *
 * 和终端会话、SFTP 都不是同一条连接。工作线程里用 libssh2 开一条
 * 只执行 /bin/cat /proc/stat /proc/meminfo 的会话，大约每 3 秒读一次。
 * 终端里的按键和回显不会被这些采样插进去。
 *
 * 主机指纹沿用 SFTP 那份 known_hosts。指纹变了询问一次界面；
 * 用户拒绝后本次 SSH 连接不再重试。
 */
class HostStats : public QObject
{
    Q_OBJECT

public:
    explicit HostStats();
    ~HostStats() override;

    using HostKeyPrompter = std::function<bool(const QString &host,
                                               const QString &storedFingerprint,
                                               const QString &currentFingerprint)>;
    void setHostKeyPrompter(HostKeyPrompter prompter);

public slots:
    void start(const SshTarget &target);
    void stop();

signals:
    // 任一字段无效时对应的 ok 为 false，界面显示 "--"。
    // usedKiB / totalKiB 来自 /proc/meminfo，单位是 KiB。
    void statsUpdated(bool cpuOk, int cpuPercent,
                      bool memOk, bool memPercentOk, int memPercent,
                      qint64 usedKiB, qint64 totalKiB);

private:
    void onTick();
    bool deferToWorker(const std::function<void()> &fn);
    bool openSession();
    void closeSession();
    bool execRead(const char *command, QByteArray *output);
    bool readProc(QByteArray *output);
    bool confirmHostKey(const QString &host, const QString &stored, const QString &current);
    bool rememberHost(const QString &key, const QString &fingerprint);

    HostKeyPrompter m_prompter;
    QThread *m_thread = nullptr;
    QTimer *m_timer = nullptr;

    SshTarget m_target;
    void *m_session = nullptr;   // LIBSSH2_SESSION*
    quintptr m_sock = 0;
    bool m_stopped = true;
    bool m_giveUp = false;       // 认证失败或用户拒绝新指纹：本次 start 不再重试

    bool m_haveCpu = false;
    quint64 m_prevTotal = 0;
    quint64 m_prevIdle = 0;
};

#endif // HOSTSTATS_H
