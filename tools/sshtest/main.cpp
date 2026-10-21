#include <QByteArray>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include <cstdio>

#include "sshsession.h"
#include "sshtarget.h"

namespace {

constexpr int kCols = 101;
constexpr int kRows = 31;
constexpr auto kMarker = "XTERM_RAW_MARKER";
constexpr auto kClear = "\x1b[2J";

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

bool parseSttySize(const QByteArray &output, int *rows, int *cols)
{
    const QRegularExpression pattern(
        QStringLiteral(R"((?m)^[ \t]*(\d+)[ \t]+(\d+)[ \t]*$)"));
    const auto match = pattern.match(QString::fromUtf8(output));
    if (!match.hasMatch())
        return false;
    *rows = match.captured(1).toInt();
    *cols = match.captured(2).toInt();
    return *rows > 0 && *cols > 0;
}

} // namespace

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

    SshSession session;
    QByteArray output;
    int exitStatus = 2;

    QObject::connect(&session, &SshSession::received, [&](const QByteArray &chunk) {
        output += chunk;
    });
    QObject::connect(&session, &SshSession::authAnswersRequired,
                     [&](SshRequestId requestId,
                         const QList<SshAuthPrompt> &prompts) {
                         QStringList answers;
                         answers.reserve(prompts.size());
                         for (int i = 0; i < prompts.size(); ++i)
                             answers.append(target.password);
                         session.answerAuthentication(requestId, answers);
                     });
    QObject::connect(&session, &SshSession::hostKeyDecisionRequired,
                     [&](const SshHostKeyChange &change) {
                         session.answerHostKey(change.requestId, false);
                     });
    QObject::connect(&session, &SshSession::errorOccurred,
                     [&](const QString &message) {
                         std::fprintf(stderr, "ssh error: %s\n",
                                      qPrintable(message));
                     });
    QObject::connect(&session, &SshSession::opened, [&] {
        session.send(
            QByteArrayLiteral(
                "printf '\\033[2JXTERM_RAW_MARKER\\n'; stty size; exit\n"));
    });
    QObject::connect(&session, &SshSession::closed, [&] {
        int rows = 0;
        int cols = 0;
        const bool hasClear = output.contains(kClear);
        const bool hasMarker = output.contains(kMarker);
        const bool hasSize = parseSttySize(output, &rows, &cols);
        std::printf("bytes=%d clear=%d marker=%d size=%dx%d expected=%dx%d\n",
                    int(output.size()), int(hasClear), int(hasMarker),
                    rows, cols, kRows, kCols);
        if (hasClear && hasMarker && hasSize && rows == kRows && cols == kCols)
            exitStatus = 0;
        else
            exitStatus = 1;
        QCoreApplication::exit(exitStatus);
    });

    session.setPtySize(kCols, kRows);
    const QString openError = session.open(target);
    if (!openError.isEmpty()) {
        std::fprintf(stderr, "open failed: %s\n", qPrintable(openError));
        return 1;
    }

    QTimer::singleShot(20000, [&] {
        std::fputs("timeout waiting for SSH session\n", stderr);
        session.close();
        QTimer::singleShot(2000, [&] { QCoreApplication::exit(1); });
    });

    const int rc = app.exec();
    return rc == 0 ? exitStatus : rc;
}
