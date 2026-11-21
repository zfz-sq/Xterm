#include "transferqueue.h"

#include "filebrowsermodel.h"
#include "sftpclient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

TransferQueue::TransferQueue(SftpClient *client, const QString &sessionKey,
                             QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_sessionKey(sessionKey)
{
    connect(m_client, &SftpClient::transferProgress, this,
            &TransferQueue::onTransferProgress);
    connect(m_client, &SftpClient::transferFinished, this,
            &TransferQueue::onTransferFinished);

    m_editWatcher = new QFileSystemWatcher(this);
    m_editTimer = new QTimer(this);
    m_editTimer->setSingleShot(true);
    m_editTimer->setInterval(800);
    connect(m_editTimer, &QTimer::timeout, this, &TransferQueue::commitEditUpload);
    connect(m_editWatcher, &QFileSystemWatcher::fileChanged, this, &TransferQueue::onEditFileChanged);
}

int TransferQueue::upload(const QString &local, const QString &remote, qint64 size)
{
    Job job;
    job.upload = true;
    job.localPath = local;
    job.remotePath = remote;
    job.size = size;
    job.total = size;
    return appendJob(job);
}

int TransferQueue::download(const QString &local, const QString &remote, qint64 size)
{
    Job job;
    job.localPath = local;
    job.remotePath = remote;
    job.size = size;
    job.total = size;
    return appendJob(job);
}

int TransferQueue::uploadDir(const QString &localDir, const QString &remoteDir)
{
    Q_UNUSED(localDir);
    Job job;
    job.makeDir = true;
    job.remotePath = remoteDir;
    return appendJob(job);
}

int TransferQueue::removeEntry(const QString &remotePath, bool isDir)
{
    Job job;
    job.removeTask = true;
    job.removeIsDir = isDir;
    job.remotePath = remotePath;
    return appendJob(job);
}

int TransferQueue::fetch(bool edit, const QString &remotePath, qint64 size)
{
    const QString local = cacheFile(edit, remotePath);
    const int id = download(local, remotePath, size);
    PendingFetch job;
    job.edit = edit;
    job.local = local;
    job.remote = remotePath;
    m_pendingFetch.insert(id, job);
    return id;
}

void TransferQueue::cancelAll()
{
    QVector<int> queued;
    int activeId = 0;
    for (const Job &job : m_jobs) {
        if (job.state == 0)
            queued.append(job.id);
        else if (job.state == 1)
            activeId = job.id;
    }
    if (queued.isEmpty() && activeId == 0)
        return;
    // 先把排队任务全部本地置终态，再取消活动任务：泵中途不会再拉起下一个任务
    for (int id : queued)
        finishJob(id, false, tr("已取消"));
    if (activeId != 0)
        m_client->cancelTransfer(activeId);
    emitProgress();
}

void TransferQueue::watchEditFile(const QString &local, const QString &remote)
{
    m_editDigest.insert(local, fileDigest(local));
    m_watchedEdit.insert(local, remote);
    if (!m_editWatcher->files().contains(local))
        m_editWatcher->addPath(local);
}

void TransferQueue::trackFolderJob(int id)
{
    m_folderUploadIds.insert(id);
}

TransferQueue::Job *TransferQueue::jobById(int id)
{
    for (Job &job : m_jobs) {
        if (job.id == id)
            return &job;
    }
    return nullptr;
}

int TransferQueue::appendJob(const Job &job)
{
    Job entry = job;
    entry.id = m_nextId++;
    m_jobs.append(entry);
    pump();
    return entry.id;
}

void TransferQueue::pump()
{
    if (m_activeId != 0)
        return;
    for (Job &job : m_jobs) {
        if (job.state != 0)
            continue;
        job.state = 1;
        m_activeId = job.id;
        job.done = 0;
        if (job.makeDir)
            m_client->makeDirTracked(job.id, job.remotePath);
        else if (job.removeTask)
            m_client->removeEntryTracked(job.id, job.remotePath, job.removeIsDir);
        else if (job.upload)
            m_client->uploadFile(job.id, job.localPath, job.remotePath);
        else
            m_client->downloadFile(job.id, job.remotePath, job.localPath);
        emitProgress();
        return;
    }
}

void TransferQueue::onTransferProgress(int id, qint64 done, qint64 total)
{
    Job *job = jobById(id);
    if (job == nullptr || job->state != 1)
        return;
    if (total > 0)
        job->total = total;
    job->done = total > 0 ? done : 0;
    emitProgress();
}

void TransferQueue::finishJob(int id, bool ok, const QString &error)
{
    Job *job = jobById(id);
    if (job == nullptr)
        return;
    if (job->state != 0 && job->state != 1)
        return; // 已终态
    job->error = ok ? QString() : error;
    job->state = ok ? 2 : (error == tr("已取消") ? 4 : 3);
    if (m_activeId == id)
        m_activeId = 0;
    if (m_folderUploadIds.contains(id)) {
        m_folderUploadIds.remove(id);
        if (m_folderUploadIds.isEmpty()) // 整批（建目录 + 文件）收尾
            emit folderBatchFinished();
    }
    if (m_removeIds.contains(id)) {
        m_removeIds.remove(id);
        if (m_removeIds.isEmpty())   // 本轮删除全部收尾：界面刷新一次
            emit removeFinished();
    }
    pump();
    emitProgress();
}

void TransferQueue::onTransferFinished(int id, bool ok, const QString &error)
{
    if (m_pendingFetch.contains(id)) {
        const PendingFetch fetchJob = m_pendingFetch.take(id);
        if (fetchJob.uploadBack) {
            if (!ok) {
                m_editDigest.insert(fetchJob.local, fetchJob.previousDigest);
                emit editUploadFinished(false, QFileInfo(fetchJob.local).fileName(),
                                        fetchJob.remote, error);
            } else {
                emit editUploadFinished(true, QFileInfo(fetchJob.local).fileName(),
                                        fetchJob.remote, QString());
            }
        } else {
            emit fetchFinished(fetchJob, ok, error);
        }
    }
    finishJob(id, ok, error);
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
    const int id = upload(local, m_watchedEdit.value(local), QFileInfo(local).size());
    PendingFetch job;
    job.uploadBack = true;
    job.local = local;
    job.remote = m_watchedEdit.value(local);
    job.previousDigest = previous;
    m_pendingFetch.insert(id, job);
    emit editUploadStarted(QFileInfo(local).fileName());
}

void TransferQueue::emitProgress()
{
    Job *active = jobById(m_activeId);
    if (active == nullptr) {
        // 队列收尾：给一句汇总，下次入队或取消时被替换
        int doneCount = 0;
        int failedCount = 0;
        for (const Job &job : m_jobs) {
            if (job.state == 2)
                ++doneCount;
            else if (job.state == 3 || job.state == 4)
                ++failedCount;
        }
        if (doneCount + failedCount == 0) {
            emit progressChanged(QString(), 0, false);
            return;
        }
        QString text = tr("传输结束：成功 %1").arg(doneCount);
        if (failedCount > 0)
            text += tr(" · 失败 %1").arg(failedCount);
        emit progressChanged(text, 0, false);
        return;
    }

    QString text;
    int percent = 0;
    if (active->makeDir) {
        text = tr("建目录 %1").arg(active->remotePath);
    } else if (active->removeTask) {
        text = active->total > 0
                   ? tr("删除 %1 · %2 / %3 项")
                         .arg(active->remotePath, QString::number(active->done),
                              QString::number(active->total))
                   : tr("删除 %1...").arg(active->remotePath);
    } else {
        const QString direction = active->upload ? tr("上传") : tr("下载");
        const QString name = QFileInfo(active->upload ? active->localPath
                                                      : active->remotePath)
                                 .fileName();
        if (active->total > 0) {
            percent = int(active->done * 100 / active->total);
            text = tr("%1 %2 · %3%").arg(direction, name, QString::number(percent));
        } else {
            text = tr("%1 %2...").arg(direction, name);
        }
    }
    emit progressChanged(text, percent, true);
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
