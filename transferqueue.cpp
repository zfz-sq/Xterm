#include "transferqueue.h"

#include "filebrowsermodel.h"
#include "sftpclient.h"
#include "transferdialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

TransferQueue::TransferQueue(SftpClient *client, const QString &sessionKey,
                             QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_sessionKey(sessionKey)
{
    m_dialog = new TransferDialog(client, dialogParent);
    connect(m_dialog, &TransferDialog::jobFinished, this, &TransferQueue::onJobFinished);
    connect(m_client, &SftpClient::transferFinished, this, &TransferQueue::onTransferFinished);

    m_editWatcher = new QFileSystemWatcher(this);
    m_editTimer = new QTimer(this);
    m_editTimer->setSingleShot(true);
    m_editTimer->setInterval(800);
    connect(m_editTimer, &QTimer::timeout, this, &TransferQueue::commitEditUpload);
    connect(m_editWatcher, &QFileSystemWatcher::fileChanged, this, &TransferQueue::onEditFileChanged);
}

int TransferQueue::upload(const QString &local, const QString &remote, qint64 size)
{
    return m_dialog->enqueue(true, local, remote, size);
}

int TransferQueue::download(const QString &local, const QString &remote, qint64 size)
{
    return m_dialog->enqueue(false, local, remote, size);
}

int TransferQueue::uploadDir(const QString &localDir, const QString &remoteDir)
{
    return m_dialog->enqueueDir(localDir, remoteDir);
}

int TransferQueue::fetch(bool edit, const QString &remotePath, qint64 size)
{
    const QString local = cacheFile(edit, remotePath);
    const int id = m_dialog->enqueue(false, local, remotePath, size);
    PendingFetch job;
    job.edit = edit;
    job.local = local;
    job.remote = remotePath;
    m_pendingFetch.insert(id, job);
    return id;
}

void TransferQueue::watchEditFile(const QString &local, const QString &remote)
{
    m_editDigest.insert(local, fileDigest(local));
    m_watchedEdit.insert(local, remote);
    if (!m_editWatcher->files().contains(local))
        m_editWatcher->addPath(local);
}

void TransferQueue::noteFolderMkdir(const QString &remoteDir)
{
    m_folderMkdirs.insert(remoteDir);
}

void TransferQueue::trackFolderJob(int id)
{
    m_folderUploadIds.insert(id);
}

bool TransferQueue::consumeFolderMkdir(const QString &remoteDir)
{
    return m_folderMkdirs.remove(remoteDir);
}

void TransferQueue::onTransferFinished(int id, bool ok, const QString &error)
{
    if (!m_pendingFetch.contains(id))
        return;
    const PendingFetch job = m_pendingFetch.take(id);
    if (job.uploadBack) {
        if (!ok) {
            m_editDigest.insert(job.local, job.previousDigest);
            emit editUploadFinished(false, QFileInfo(job.local).fileName(), job.remote, error);
        } else {
            emit editUploadFinished(true, QFileInfo(job.local).fileName(), job.remote, QString());
        }
        return;
    }
    emit fetchFinished(job, ok, error);
}


void TransferQueue::onJobFinished(int id)
{
    if (!m_folderUploadIds.contains(id))
        return;
    m_folderUploadIds.remove(id);
    if (m_folderUploadIds.isEmpty()) { // 整批（建目录 + 文件）收尾
        m_folderMkdirs.clear();
        emit folderBatchFinished();
    }
}

void TransferQueue::onEditFileChanged(const QString &path)
{
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QString key;
    if (m_watchedEdit.contains(normalized)) {
        key = normalized;
    } else {
        for (auto it = m_watchedEdit.cbegin(); it != m_watchedEdit.cend(); ++it) {
            if (QDir::cleanPath(QFileInfo(it.key()).absoluteFilePath()) == normalized) {
                key = it.key();
                break;
            }
        }
    }
    if (key.isEmpty())
        return;
    if (QFileInfo::exists(key))
        m_editWatcher->addPath(key);
    m_editTimerPath = key;
    m_editTimer->start();
}

void TransferQueue::commitEditUpload()
{
    const QString local = m_editTimerPath;
    if (!m_watchedEdit.contains(local) || !QFileInfo::exists(local))
        return;
    const QByteArray digest = fileDigest(local);
    if (digest.isEmpty() || digest == m_editDigest.value(local))
        return;
    const QByteArray previous = m_editDigest.value(local);
    m_editDigest.insert(local, digest);
    const int id = m_dialog->enqueue(true, local, m_watchedEdit.value(local),
                                     QFileInfo(local).size());
    PendingFetch job;
    job.uploadBack = true;
    job.local = local;
    job.remote = m_watchedEdit.value(local);
    job.previousDigest = previous;
    m_pendingFetch.insert(id, job);
    emit editUploadStarted(QFileInfo(local).fileName());
}

QString TransferQueue::cacheFile(bool edit, const QString &remotePath) const
{
    QString key = m_sessionKey;
    key.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    const QString base = edit
                             ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                             : QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString relative = remotePath;
    while (relative.startsWith(QLatin1Char('/')))
        relative.remove(0, 1);
    const QString folder = edit ? QStringLiteral("sftp-edit") : QStringLiteral("sftp-open");
    const QString local = QDir(base).filePath(folder + QLatin1Char('/') + key + QLatin1Char('/') + relative);
    QDir().mkpath(QFileInfo(local).absolutePath());
    return QDir::cleanPath(local);
}
