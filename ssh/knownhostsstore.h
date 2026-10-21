#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

class KnownHostKeyValidator
{
public:
    virtual ~KnownHostKeyValidator() = default;
    virtual bool validateKnownHostKey(
        const QByteArray &algorithm,
        const QByteArray &encodedKey) const = 0;
};

class KnownHostsStore
{
public:
    explicit KnownHostsStore(
        KnownHostKeyValidator &keyValidator,
        QString rootDirectory = QString());

    QString path() const;
    QString legacyPath() const;
    QString globalPath() const;
    bool prepareMigration(QString *error = nullptr);
    QString legacyFingerprint(const QString &host, quint16 port) const;
    QString standardFingerprint(const QString &host, quint16 port,
                                QString *error = nullptr) const;
    bool rememberLegacyMatch(const QString &host, quint16 port,
                             const QString &fingerprint,
                             QString *error = nullptr);

private:
    KnownHostKeyValidator &m_keyValidator;
    QString m_rootDirectory;
};
