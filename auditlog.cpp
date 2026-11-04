#include "auditlog.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

AuditLog::AuditLog(const QString &path, qint64 rollBytes)
    : m_path(path)
    , m_rollBytes(qMax<qint64>(1024, rollBytes)) // 再小也要能装下一行常规条目
{
}

AuditLog::~AuditLog() = default;

bool AuditLog::ensureOpen()
{
    if (m_file.isOpen())
        return true;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    m_file.setFileName(m_path);
    return m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

void AuditLog::roll()
{
    m_file.close();
    // 只保两代：旧 .old 让位，当前文件降级为 .old。删/改名失败也静默，
    // 下次 append 会重开（继续写超限文件，好过丢审计）
    QFile::remove(m_path + QStringLiteral(".old"));
    QFile::rename(m_path, m_path + QStringLiteral(".old"));
}

void AuditLog::append(const QJsonObject &entry)
{
    if (!ensureOpen())
        return;
    if (m_file.size() >= m_rollBytes) {
        roll();
        if (!ensureOpen())
            return;
    }
    QJsonObject full = entry;
    full.insert(QStringLiteral("ts"),
                QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    m_file.write(QJsonDocument(full).toJson(QJsonDocument::Compact));
    m_file.write("\n");
    m_file.flush();
}
