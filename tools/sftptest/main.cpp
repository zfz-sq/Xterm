// sftptest: 文件管理链路对真机的端到端验证。
//
// 流程: SshSession 连接（TOFU 指纹）→ realpath/list home → 上传 2MB
//       → 传输期间持续向 shell 发 marker 命令并断言有响应 → 下载
//       → SHA-256 比对 → rename → chmod → delete → 断开会话。
//       任何一步失败即 FAIL 退出；全部通过时 exit 0。
//
// 用法（与 sshtest 相同的环境变量）:
//   XTERM_SSH_HOST / XTERM_SSH_USER / XTERM_SSH_PASSWORD
//   XTERM_SSH_KEY / XTERM_SSH_PORT / XTERM_SSH_TIMEOUT
//   未设置 XTERM_SSH_HOST 时输出 SKIP 并以 77 退出。
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTimer>

#include <cstdio>

#include "sftpclient.h"
#include "sshsession.h"

namespace {

constexpr auto kMarker = "XTERM_SFTP_MARKER";

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char *what, const QString &detail = QString())
{
    if (ok) {
        ++g_pass;
        std::printf("PASS  %s\n", what);
    } else {
        ++g_fail;
        std::printf("FAIL  %s  %s\n", what, qPrintable(detail));
    }
    std::fflush(stdout);
}

SshTarget targetFromEnvironment()
{
    SshTarget target;
    target.host = qEnvironmentVariable("XTERM_SSH_HOST");
    target.user = qEnvironmentVariable("XTERM_SSH_USER");
    target.password = qEnvironmentVariable("XTERM_SSH_PASSWORD");
    target.privateKey = qEnvironmentVariable("XTERM_SSH_KEY");
    bool portOk = false;
    const int port = qEnvironmentVariableIntValue("XTERM_SSH_PORT", &portOk);
    target.port = portOk && port > 0 ? port : 22;
    bool timeoutOk = false;
    const int timeout =
        qEnvironmentVariableIntValue("XTERM_SSH_TIMEOUT", &timeoutOk);
    target.connectTimeoutSec = timeoutOk && timeout > 0 ? timeout : 15;
    return target;
}

QByteArray fileSha256(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f))
        return QByteArray();
    return hash.result().toHex();
}

} // namespace

class Test : public QObject
{
    Q_OBJECT
public:
    explicit Test(const SshTarget &target, QObject *parent = nullptr)
        : QObject(parent), m_target(target)
    {
        m_client = new SftpClient(&m_session, this);

        connect(&m_session, &SshSession::received, this,
                [this](const QByteArray &chunk) { m_shellOutput += chunk; });
        connect(&m_session, &SshSession::authAnswersRequired, this,
                [this](SshRequestId requestId,
                       const QList<SshAuthPrompt> &prompts) {
                    QStringList answers;
                    answers.reserve(prompts.size());
                    for (int i = 0; i < prompts.size(); ++i)
                        answers.append(m_target.password);
                    m_session.answerAuthentication(requestId, answers);
                });
        connect(&m_session, &SshSession::hostKeyDecisionRequired, this,
                [this](const SshHostKeyChange &change) {
                    std::printf("---- host key: %s ----\n",
                                qPrintable(change.host));
                    m_session.answerHostKey(change.requestId, true);
                });
        connect(&m_session, &SshSession::errorOccurred, this,
                [](const QString &message) {
                    std::fprintf(stderr, "ssh error: %s\n", qPrintable(message));
                });
        connect(&m_session, &SshSession::opened, this, [this] {
            m_started = true;
            next(QStringLiteral("realpath"));
            m_client->realPath(QStringLiteral("."));
        });
        connect(&m_session, &SshSession::closed, this, [this] {
            finish();
        });

        connect(m_client, &SftpClient::listed, this, &Test::onListed);
        connect(m_client, &SftpClient::listedFailed, this,
                [this](const QString &p, const QString &e) {
                    check(false, "listDir", QStringLiteral("%1: %2").arg(p, e));
                    quit();
                });
        connect(m_client, &SftpClient::transferProgress, this,
                [this](int id, qint64 done, qint64 total) {
                    Q_UNUSED(id);
                    // 传输进行中：shell 必须仍然有来有回。
                    if (!m_markerTimer->isActive()) {
                        m_baseMarkers = m_shellOutput.count(kMarker);
                        m_markerTimer->start();
                    }
                    std::printf("      progress done=%lld total=%lld\n",
                                static_cast<long long>(done),
                                static_cast<long long>(total));
                    std::fflush(stdout);
                });
        connect(m_client, &SftpClient::transferFinished, this,
                &Test::onTransferFinished);
        connect(m_client, &SftpClient::opFinished, this, &Test::onOpFinished);

        // 传输期间每 200ms 向 shell 发一条 marker 命令
        m_markerTimer = new QTimer(this);
        m_markerTimer->setInterval(200);
        connect(m_markerTimer, &QTimer::timeout, this, [this] {
            ++m_markersSent;
            m_session.send(QByteArrayLiteral("echo ") + kMarker + "\n");
        });

        // 看门狗: 任何环节 45 秒没有推进就判失败退出
        m_watchdog = new QTimer(this);
        m_watchdog->setSingleShot(true);
        connect(m_watchdog, &QTimer::timeout, this, [this] {
            check(false, "watchdog", QStringLiteral("step %1 超时").arg(m_step));
            m_session.close();
            QTimer::singleShot(2000, this, [this] { QCoreApplication::exit(1); });
        });
        m_watchdog->start(45000);
    }

    void start()
    {
        std::printf("---- connect %s@%s:%d (shared session) ----\n",
                    m_target.user.toUtf8().constData(),
                    m_target.host.toUtf8().constData(), m_target.port);
        std::fflush(stdout);
        if (!m_session.open(m_target).isEmpty()) {
            check(false, "connect", QStringLiteral("open 失败"));
            QCoreApplication::exit(1);
        }
    }

private:
    void next(const QString &step)
    {
        m_step = step;
        m_watchdog->start(45000); // 重置看门狗
    }

    void quit()
    {
        m_watchdog->stop();
        m_markerTimer->stop();
        m_session.close();
    }

    void finish()
    {
        if (!m_started) {
            check(false, "connect", QStringLiteral("会话未能建立"));
            QCoreApplication::exit(1);
            return;
        }
        // shell 在传输期间每发一条 marker 都要有响应（echo 回显 + 命令输出）。
        const int responses =
            m_shellOutput.count(kMarker) - m_baseMarkers;
        check(m_markersSent > 3,
              "markers sent during transfer",
              QString::number(m_markersSent));
        check(responses >= m_markersSent, "shell alive during transfer",
              QStringLiteral("sent=%1 responses=%2")
                  .arg(m_markersSent)
                  .arg(responses));
        std::printf("---- %d passed, %d failed ----\n", g_pass, g_fail);
        std::fflush(stdout);
        QCoreApplication::exit(g_fail == 0 ? 0 : 1);
    }

    void onOpFinished(const QString &op, const QString &arg, bool ok,
                      const QString &error)
    {
        if (op == QLatin1String("realpath")) {
            check(ok && !arg.isEmpty(), "connect+realpath", error);
            m_home = arg;
            next(QStringLiteral("listDir"));
            m_client->listDir(m_home);
        } else if (op == QLatin1String("rename")) {
            check(ok, "rename", error);
            next(QStringLiteral("chmod"));
            m_client->setPermissions(m_remoteUp, 0600);
        } else if (op == QLatin1String("chmod")) {
            check(ok, "chmod 0600", error);
            next(QStringLiteral("remove"));
            m_client->removeEntry(m_remoteUp, false);
        } else if (op == QLatin1String("remove")) {
            check(ok, "remove", error);
            quit();   // 全部步骤完成
        }
    }

    void onListed(const QString &path, const QVector<SftpEntry> &entries)
    {
        if (m_step != QLatin1String("listDir")) {
            Q_UNUSED(path);
            return;
        }
        check(!entries.isEmpty(), "listDir non-empty", path);
        std::printf("      home 含 %d 个条目\n", int(entries.size()));
        std::fflush(stdout);

        // 生成 2MB 测试文件并上传（伪随机内容，避免压缩掩盖传错）
        m_localUp = QDir::temp().filePath(QStringLiteral("sftptest-up.bin"));
        {
            QFile f(m_localUp);
            f.open(QIODevice::WriteOnly);
            QByteArray block(4096, 0);
            for (int i = 0; i < 512; ++i) {
                for (int j = 0; j < block.size(); ++j)
                    block[j] = char((i * 131 + j * 7) & 0xFF);
                f.write(block);
            }
        }
        m_remoteUp = m_home + QStringLiteral("/sftptest-up.bin");
        next(QStringLiteral("upload"));
        m_client->uploadFile(1, m_localUp, m_remoteUp);
    }

    void onTransferFinished(int id, bool ok, const QString &error)
    {
        if (id == 1) {
            check(ok, "upload 2MB", error);
            m_markerTimer->stop();
            m_localDown = QDir::temp().filePath(QStringLiteral("sftptest-down.bin"));
            QFile::remove(m_localDown);
            next(QStringLiteral("download"));
            m_client->downloadFile(2, m_remoteUp, m_localDown);
        } else if (id == 2) {
            check(ok, "download 2MB", error);
            const QByteArray a = fileSha256(m_localUp);
            const QByteArray b = fileSha256(m_localDown);
            check(a == b && !a.isEmpty(), "sha256 round-trip");
            next(QStringLiteral("rename"));
            m_client->renameEntry(m_remoteUp,
                                  m_home + QStringLiteral("/sftptest-up2.bin"));
            m_remoteUp = m_home + QStringLiteral("/sftptest-up2.bin");
        }
    }

    SshTarget m_target;
    SshSession m_session;
    SftpClient *m_client = nullptr;
    QTimer *m_watchdog = nullptr;
    QTimer *m_markerTimer = nullptr;
    QString m_step;
    QString m_home;
    QString m_localUp, m_remoteUp, m_localDown;
    QByteArray m_shellOutput;
    bool m_started = false;
    int m_baseMarkers = 0;
    int m_markersSent = 0;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const SshTarget target = targetFromEnvironment();
    if (target.host.trimmed().isEmpty()) {
        std::puts("SKIP: XTERM_SSH_HOST is not set");
        return 77;
    }

    if (const QString err = SshSession::availabilityError(); !err.isEmpty()) {
        std::fprintf(stderr, "libssh unavailable: %s\n", qPrintable(err));
        return 1;
    }

    Test test(target);
    test.start();
    return app.exec();
}

#include "main.moc"
