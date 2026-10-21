#include "knownhostsstore.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace {

struct LegacyRow
{
    QString host;
    quint16 port = 0;
    QString fingerprint;
    bool valid = false;
};

LegacyRow parseLegacyRow(const QByteArray &line)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^(\S+):(\d+)\s+([0-9A-Fa-f]{64})$)"));

    const QString text = QString::fromUtf8(line).trimmed();
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) {
        return {};
    }

    bool portOk = false;
    const uint parsedPort = match.captured(2).toUInt(&portOk);
    if (!portOk || parsedPort > 65535) {
        return {};
    }

    return {match.captured(1), static_cast<quint16>(parsedPort),
            match.captured(3).toLower(), true};
}

bool isIgnoredLegacyLine(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    return trimmed.isEmpty() || trimmed.startsWith('#');
}

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

bool createEmptyFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        setError(error, QStringLiteral("Cannot create known_hosts file %1: %2")
                            .arg(path, file.errorString()));
        return false;
    }
    return true;
}

} // namespace

KnownHostsStore::KnownHostsStore(QString rootDirectory)
    : m_rootDirectory(rootDirectory.isEmpty()
                          ? QStandardPaths::writableLocation(
                                QStandardPaths::AppConfigLocation)
                          : std::move(rootDirectory))
{
}

QString KnownHostsStore::path() const
{
    return QDir(m_rootDirectory).filePath(QStringLiteral("known_hosts"));
}

QString KnownHostsStore::legacyPath() const
{
    return QDir(m_rootDirectory).filePath(QStringLiteral("known_hosts.legacy"));
}

bool KnownHostsStore::prepareMigration(QString *error)
{
    setError(error, {});

    QDir root(m_rootDirectory);
    if (!root.exists() && !root.mkpath(QStringLiteral("."))) {
        setError(error, QStringLiteral("Cannot create known_hosts directory %1")
                            .arg(m_rootDirectory));
        return false;
    }

    QFile current(path());
    if (!current.exists()) {
        return createEmptyFile(path(), error);
    }
    if (!current.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read known_hosts file %1: %2")
                            .arg(path(), current.errorString()));
        return false;
    }

    bool hasLegacyRecord = false;
    bool hasUnknownRecord = false;
    while (!current.atEnd()) {
        const QByteArray line = current.readLine();
        if (isIgnoredLegacyLine(line)) {
            continue;
        }
        if (parseLegacyRow(line).valid) {
            hasLegacyRecord = true;
        } else {
            hasUnknownRecord = true;
            break;
        }
    }
    current.close();

    if (!hasLegacyRecord || hasUnknownRecord) {
        return true;
    }
    if (QFile::exists(legacyPath())) {
        setError(error,
                 QStringLiteral("Cannot migrate %1 because %2 already exists")
                     .arg(path(), legacyPath()));
        return false;
    }
    if (!QFile::rename(path(), legacyPath())) {
        setError(error, QStringLiteral("Cannot rename %1 to %2")
                            .arg(path(), legacyPath()));
        return false;
    }
    return createEmptyFile(path(), error);
}

QString KnownHostsStore::legacyFingerprint(const QString &host,
                                           quint16 port) const
{
    QFile legacy(legacyPath());
    if (!legacy.open(QIODevice::ReadOnly)) {
        return {};
    }

    while (!legacy.atEnd()) {
        const LegacyRow row = parseLegacyRow(legacy.readLine());
        if (row.valid && row.host == host && row.port == port) {
            return row.fingerprint;
        }
    }
    return {};
}

bool KnownHostsStore::rememberLegacyMatch(const QString &host, quint16 port,
                                          const QString &fingerprint,
                                          QString *error)
{
    setError(error, {});

    QFile legacy(legacyPath());
    if (!legacy.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read legacy known_hosts file %1: %2")
                            .arg(legacyPath(), legacy.errorString()));
        return false;
    }
    const QByteArray contents = legacy.readAll();
    if (legacy.error() != QFileDevice::NoError) {
        setError(error, QStringLiteral("Cannot read legacy known_hosts file %1: %2")
                            .arg(legacyPath(), legacy.errorString()));
        return false;
    }
    legacy.close();

    QByteArray rewritten;
    rewritten.reserve(contents.size());
    bool removed = false;
    qsizetype position = 0;
    while (position < contents.size()) {
        const qsizetype newline = contents.indexOf('\n', position);
        const qsizetype end = newline < 0 ? contents.size() : newline + 1;
        const QByteArray completeLine = contents.mid(position, end - position);
        const LegacyRow row = parseLegacyRow(completeLine);
        if (!removed && row.valid && row.host == host && row.port == port
            && row.fingerprint.compare(fingerprint, Qt::CaseInsensitive) == 0) {
            removed = true;
        } else {
            rewritten.append(completeLine);
        }
        position = end;
    }

    if (!removed) {
        setError(error,
                 QStringLiteral("No matching legacy fingerprint for %1:%2")
                     .arg(host)
                     .arg(port));
        return false;
    }

    QSaveFile output(legacyPath());
    if (!output.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Cannot rewrite legacy known_hosts file %1: %2")
                            .arg(legacyPath(), output.errorString()));
        return false;
    }
    if (output.write(rewritten) != rewritten.size()) {
        setError(error, QStringLiteral("Cannot rewrite legacy known_hosts file %1: %2")
                            .arg(legacyPath(), output.errorString()));
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) {
        setError(error, QStringLiteral("Cannot commit legacy known_hosts file %1: %2")
                            .arg(legacyPath(), output.errorString()));
        return false;
    }
    return true;
}
