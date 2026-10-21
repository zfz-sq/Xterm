#include "knownhostsstore.h"

#include <QCryptographicHash>
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

bool readFileContents(const QString &path, const QString &description,
                      QByteArray *contents, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot read %1 file %2: %3")
                            .arg(description, path, file.errorString()));
        return false;
    }

    *contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        setError(error, QStringLiteral("Cannot read %1 file %2: %3")
                            .arg(description, path, file.errorString()));
        contents->clear();
        return false;
    }
    return true;
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

bool standardFileHasMatchingKey(const QString &path, const QString &host,
                                quint16 port, const QString &fingerprint,
                                QString *error)
{
    QByteArray contents;
    if (!readFileContents(path, QStringLiteral("known_hosts"), &contents,
                          error)) {
        return false;
    }

    const QString expectedPattern =
        port == 22
            ? host
            : QStringLiteral("[%1]:%2").arg(host).arg(port);
    const QList<QByteArray> lines = contents.split('\n');
    for (const QByteArray &line : lines) {
        const QString trimmed = QString::fromUtf8(line).trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }

        const QStringList fields =
            trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                          Qt::SkipEmptyParts);
        if (fields.size() < 3
            || !fields.at(0).split(QLatin1Char(',')).contains(expectedPattern)) {
            continue;
        }

        const auto decoded = QByteArray::fromBase64Encoding(
            fields.at(2).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            continue;
        }
        const QByteArray hash =
            QCryptographicHash::hash(decoded.decoded,
                                     QCryptographicHash::Sha256)
                .toHex();
        if (QString::fromLatin1(hash).compare(fingerprint,
                                              Qt::CaseInsensitive)
            == 0) {
            return true;
        }
    }

    setError(error,
             QStringLiteral("No matching OpenSSH host key for %1:%2 in %3")
                 .arg(host)
                 .arg(port)
                 .arg(path));
    return false;
}

} // namespace

namespace KnownHostsStoreDetail {

bool rollbackMigrationRename(const QString &currentPath,
                             const QString &legacyPath,
                             const QString &creationError, QString *error)
{
    if (QFile::rename(legacyPath, currentPath)) {
        setError(error,
                 QStringLiteral("%1; migration rolled back from %2 to %3")
                     .arg(creationError, legacyPath, currentPath));
        return false;
    }

    setError(error,
             QStringLiteral(
                 "%1; rollback from %2 to %3 failed "
                 "(current exists=%4, legacy exists=%5)")
                 .arg(creationError, legacyPath, currentPath,
                      QFile::exists(currentPath) ? QStringLiteral("true")
                                                 : QStringLiteral("false"),
                      QFile::exists(legacyPath) ? QStringLiteral("true")
                                                : QStringLiteral("false")));
    return false;
}

} // namespace KnownHostsStoreDetail

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

    if (!QFile::exists(path())) {
        return createEmptyFile(path(), error);
    }

    QByteArray currentContents;
    if (!readFileContents(path(), QStringLiteral("known_hosts"),
                          &currentContents, error)) {
        return false;
    }

    bool hasLegacyRecord = false;
    bool hasUnknownRecord = false;
    const QList<QByteArray> lines = currentContents.split('\n');
    for (const QByteArray &line : lines) {
        if (isIgnoredLegacyLine(line)) {
            continue;
        }
        if (parseLegacyRow(line).valid) {
            hasLegacyRecord = true;
        } else {
            hasUnknownRecord = true;
        }
    }

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
    QString creationError;
    if (createEmptyFile(path(), &creationError)) {
        return true;
    }
    return KnownHostsStoreDetail::rollbackMigrationRename(
        path(), legacyPath(), creationError, error);
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

    QByteArray contents;
    if (!readFileContents(legacyPath(), QStringLiteral("legacy known_hosts"),
                          &contents, error)) {
        return false;
    }

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

    if (!standardFileHasMatchingKey(path(), host, port, fingerprint, error)) {
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
