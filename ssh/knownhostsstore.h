#pragma once

#include <QString>
#include <QtGlobal>

class KnownHostsStore
{
public:
    explicit KnownHostsStore(QString rootDirectory = QString());

    QString path() const;
    QString legacyPath() const;
    bool prepareMigration(QString *error = nullptr);
    QString legacyFingerprint(const QString &host, quint16 port) const;
    bool rememberLegacyMatch(const QString &host, quint16 port,
                             const QString &fingerprint,
                             QString *error = nullptr);

private:
    QString m_rootDirectory;
};
