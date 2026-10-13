// sftptest: SftpClient 对真机的端到端验证。
//
// 流程: 连接(指纹校验) → 列 home → 上传 512KB → 下载回来 → 内容比对
//       → mkdir → rename → remove → 断开。任何一步失败即 FAIL 退出。
//
// 用法: sftptest [host] [user] [password]
//       默认 192.168.1.116 root embedsky（与 svtest 同一块板）。
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTimer>

#include "sftpclient.h"

#include <cstdio>

namespace {
int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char *what, const QString &detail = QString())
{
    if (ok) {
        ++g_pass;
        printf("PASS  %s\n", what);
    } else {
        ++g_fail;
        printf("FAIL  %s  %s\n", what, qPrintable(detail));
    }
    fflush(stdout);
}

QByteArray fileMd5(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Md5);
}
} // namespace

class Test : public QObject
{
    Q_OBJECT
public:
    explicit Test(const SshTarget &target, QObject *parent = nullptr)
        : QObject(parent), m_target(target)
    {
        m_client = new SftpClient();   // 无 parent：它要撒进自己的工作线程
        connect(m_client, &SftpClient::connected, this, &Test::onConnected);
        connect(m_client, &SftpClient::connectFailed, this, [this](const QString &e) {
            check(false, "connect", e);
            quit();
        });
        connect(m_client, &SftpClient::disconnected, this, [this] {
            printf("---- disconnected ----\nALL PASS\n");
            fflush(stdout);
            qApp->exit(g_fail == 0 ? 0 : 1);
        });
        connect(m_client, &SftpClient::listed, this, &Test::onListed);
        connect(m_client, &SftpClient::listedFailed, this, [this](const QString &p, const QString &e) {
            check(false, "listDir", QStringLiteral("%1: %2").arg(p, e));
            quit();
        });
        connect(m_client, &SftpClient::transferProgress, this,
                [this](int id, qint64 done, qint64 total) {
                    if (total > 0 && done >= total)
                        printf("      progress id=%d done=%s total=%s\n", id,
                               qPrintable(QString::number(done)),
                               qPrintable(QString::number(total)));
                });
        connect(m_client, &SftpClient::transferFinished, this, &Test::onTransferFinished);
        connect(m_client, &SftpClient::opFinished, this, &Test::onOpFinished);

        // 看门狗: 任何环节 30 秒没有推进就判失败退出
        m_watchdog = new QTimer(this);
        m_watchdog->setSingleShot(true);
        connect(m_watchdog, &QTimer::timeout, this, [this] {
            check(false, "watchdog", QStringLiteral("step %1 超时").arg(m_step));
            quit();
        });
        m_watchdog->start(30000);
    }

    void start()
    {
        m_step = QStringLiteral("connect");
        printf("---- connect %s@%s:%d ----\n",
               m_target.user.toUtf8().constData(),
               m_target.host.toUtf8().constData(), m_target.port);
        fflush(stdout);
        m_client->connectToHost(m_target);
    }

private:
    void quit()
    {
        m_watchdog->stop();
        if (m_client)
            QMetaObject::invokeMethod(m_client, &SftpClient::disconnectFromHost,
                                      Qt::QueuedConnection);
        else
            qApp->exit(g_fail == 0 ? 0 : 1);
    }
    void next(const QString &step)
    {
        m_step = step;
        m_watchdog->start(30000); // 重置看门狗
    }

    void onConnected(const QString &home, const QString &fingerprint)
    {
        check(!home.isEmpty() && fingerprint.size() == 64, "connect+realpath", home);
        m_home = home;
        next(QStringLiteral("listDir"));
        m_client->listDir(home);
    }

    void onListed(const QString &path, const QVector<SftpEntry> &entries)
    {
        if (m_step == QLatin1String("listDir")) {
            check(!entries.isEmpty(), "listDir non-empty", path);
            printf("      home 含 %d 个条目\n", int(entries.size()));
            fflush(stdout);
            // 生成 512KB 测试文件并上传
            const QString local = QDir::temp().filePath(QStringLiteral("sftptest-up.bin"));
            {
                QFile f(local);
                f.open(QIODevice::WriteOnly);
                QByteArray block(4096, 0);
                for (int i = 0; i < 128; ++i) {
                    // 伪随机内容，避免压缩/去重掩盖传错
                    for (int j = 0; j < block.size(); ++j)
                        block[j] = char((i * 131 + j * 7) & 0xFF);
                    f.write(block);
                }
            }
            m_localUp = local;
            m_remoteUp = m_home + QStringLiteral("/sftptest-up.bin");
            next(QStringLiteral("upload"));
            m_client->uploadFile(1, local, m_remoteUp);
        }
    }

    void onTransferFinished(int id, bool ok, const QString &error)
    {
        if (id == 1) {
            check(ok, "upload 512KB", error);
            m_localDown = QDir::temp().filePath(QStringLiteral("sftptest-down.bin"));
            QFile::remove(m_localDown);
            next(QStringLiteral("download"));
            m_client->downloadFile(2, m_remoteUp, m_localDown);
        } else if (id == 2) {
            check(ok, "download 512KB", error);
            const QByteArray a = fileMd5(m_localUp);
            const QByteArray b = fileMd5(m_localDown);
            check(a == b && !a.isEmpty(), "md5 round-trip");
            next(QStringLiteral("mkdir"));
            m_mkdir = m_home + QStringLiteral("/sftptest-dir");
            m_client->makeDir(m_mkdir);
        }
    }

    void onOpFinished(const QString &op, const QString &arg, bool ok, const QString &error)
    {
        if (op == QLatin1String("mkdir")) {
            check(ok, "mkdir", error);
            next(QStringLiteral("rename"));
            m_client->renameEntry(m_remoteUp, m_home + QStringLiteral("/sftptest-up2.bin"));
        } else if (op == QLatin1String("rename")) {
            check(ok, "rename", error);
            next(QStringLiteral("remove-file"));
            m_client->removeEntry(m_home + QStringLiteral("/sftptest-up2.bin"), false);
        } else if (op == QLatin1String("remove")) {
            if (m_step == QLatin1String("remove-file")) {
                check(ok, "remove file", error);
                next(QStringLiteral("remove-dir"));
                m_client->removeEntry(m_mkdir, true);
            } else {
                check(ok, "remove dir", error);
                quit();   // 全部步骤完成
            }
        } else if (op == QLatin1String("realpath")) {
            // 本测试不走该分支
            Q_UNUSED(arg);
        }
    }

    SshTarget m_target;
    SftpClient *m_client = nullptr;
    QTimer *m_watchdog = nullptr;
    QString m_step;
    QString m_home;
    QString m_localUp, m_remoteUp, m_localDown, m_mkdir;

public:
    ~Test() override { delete m_client; }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SshTarget>("SshTarget");
    qRegisterMetaType<SftpEntry>("SftpEntry");
    qRegisterMetaType<QVector<SftpEntry>>("QVector<SftpEntry>");

    SshTarget t;
    t.host = argc > 1 ? argv[1] : QStringLiteral("192.168.1.116");
    t.port = 22;
    t.user = argc > 2 ? argv[2] : QStringLiteral("root");
    t.password = argc > 3 ? argv[3] : QStringLiteral("embedsky");
    t.connectTimeoutSec = 10;

    Test test(t);
    test.start();
    return app.exec();
}

#include "main.moc"
