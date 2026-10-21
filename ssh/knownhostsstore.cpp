#include "knownhostsstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

bool wireBlobAlgorithmMatches(const QByteArray &blob,
                              const QByteArray &declaredType)
{
    constexpr qsizetype lengthFieldSize = 4;
    if (blob.size() < lengthFieldSize) {
        return false;
    }

    const auto *bytes =
        reinterpret_cast<const unsigned char *>(blob.constData());
    const quint32 algorithmLength =
        (static_cast<quint32>(bytes[0]) << 24)
        | (static_cast<quint32>(bytes[1]) << 16)
        | (static_cast<quint32>(bytes[2]) << 8)
        | static_cast<quint32>(bytes[3]);
    const quint64 available =
        static_cast<quint64>(blob.size() - lengthFieldSize);
    if (static_cast<quint64>(algorithmLength) > available
        || static_cast<quint64>(algorithmLength)
               != static_cast<quint64>(declaredType.size())) {
        return false;
    }
    return QByteArrayView(blob).sliced(lengthFieldSize, algorithmLength)
        == QByteArrayView(declaredType);
}

bool hostPatternsMatch(const QString &patternsField,
                       const QString &expectedPattern)
{
    bool hasPositiveMatch = false;
    const QStringList patterns = patternsField.split(QLatin1Char(','));
    for (const QString &pattern : patterns) {
        if (!pattern.isEmpty() && pattern.startsWith(QLatin1Char('!'))) {
            return false;
        }
        if (pattern == expectedPattern) {
            hasPositiveMatch = true;
        }
    }
    return hasPositiveMatch;
}

QString standardFileFingerprint(const QString &path,
                                const KnownHostKeyValidator &keyValidator,
                                const QString &host, quint16 port,
                                QString *error)
{
    QByteArray contents;
    if (!readFileContents(path, QStringLiteral("known_hosts"), &contents,
                          error)) {
        return {};
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
            || !hostPatternsMatch(fields.at(0), expectedPattern)) {
            continue;
        }

        const QByteArray declaredType = fields.at(1).toLatin1();
        const QByteArray encodedBlob = fields.at(2).toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(
            encodedBlob, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded
            || !wireBlobAlgorithmMatches(decoded.decoded, declaredType)
            || !keyValidator.validateKnownHostKey(declaredType,
                                                  encodedBlob)) {
            continue;
        }

        const QByteArray hash =
            QCryptographicHash::hash(decoded.decoded,
                                     QCryptographicHash::Sha256)
                .toHex();
        setError(error, {});
        return QString::fromLatin1(hash);
    }

    setError(error,
             QStringLiteral("No valid OpenSSH host key for %1:%2 in %3")
                 .arg(host)
                 .arg(port)
                 .arg(path));
    return {};
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

KnownHostsStore::KnownHostsStore(KnownHostKeyValidator &keyValidator,
                                 QString rootDirectory)
    : m_keyValidator(keyValidator),
      m_rootDirectory(rootDirectory.isEmpty()
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

QString KnownHostsStore::globalPath() const
{
    return QDir(m_rootDirectory).filePath(QStringLiteral("known_hosts.global"));
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

    QFile global(globalPath());
    const QFileInfo globalInfo(globalPath());
    if (!globalInfo.isFile() || globalInfo.size() != 0) {
        if (!global.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            setError(error,
                     QStringLiteral(
                         "Cannot maintain empty global known_hosts file %1: %2")
                         .arg(globalPath(), global.errorString()));
            return false;
        }
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

QString KnownHostsStore::standardFingerprint(const QString &host,
                                             quint16 port,
                                             QString *error) const
{
    setError(error, {});
    return standardFileFingerprint(path(), m_keyValidator, host, port,
                                   error);
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

    const QString standard =
        standardFingerprint(host, port, error);
    if (standard.isEmpty()
        || standard.compare(fingerprint, Qt::CaseInsensitive) != 0) {
        if (!standard.isEmpty()) {
            setError(error,
                     QStringLiteral(
                         "OpenSSH host key fingerprint mismatch for %1:%2")
                         .arg(host)
                         .arg(port));
        }
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
