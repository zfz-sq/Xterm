#ifndef AUDITLOG_H
#define AUDITLOG_H

#include <QFile>
#include <QJsonObject>
#include <QString>

/*
 * AuditLog: MCP 审计日志（M2.e 可观测）。一行一条 JSON（jsonl），
 * 追加写、句柄常开、每条 flush——崩溃也只丢最后一条。
 * 超过 rollBytes 时轮转：<path> → <path>.old（旧 .old 直接删，
 * 只保两代，长跑磁盘封顶）。写失败静默：审计不能阻塞请求路径。
 *
 * 非 QObject 值语义，AgentBridge 持有；时间戳由 append 统一补
 * （ISO 本地时间 + 毫秒），调用方只管装业务字段。
 */
class AuditLog
{
public:
    // rollBytes 可注入：单测用小阈值验证轮转；默认 4 MiB
    explicit AuditLog(const QString &path, qint64 rollBytes = 4 * 1024 * 1024);
    ~AuditLog();

    AuditLog(const AuditLog &) = delete;
    AuditLog &operator=(const AuditLog &) = delete;

    // 补 "ts" 后序列化追加一行。目录不存在自动建；打开失败丢弃本条
    void append(const QJsonObject &entry);

    QString path() const { return m_path; }
    // 当前文件字节数（未开时 0；测试观察轮转用）
    qint64 sizeBytes() const { return m_file.isOpen() ? m_file.size() : 0; }

private:
    bool ensureOpen();
    void roll();

    QString m_path;
    qint64 m_rollBytes;
    QFile m_file;
};

#endif // AUDITLOG_H
