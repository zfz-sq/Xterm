#include "transferdialog.h"

#include "sftpclient.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
constexpr qint64 kRateIntervalMs = 400;   // 速率采样间隔
}

TransferDialog::TransferDialog(SftpClient *client, QWidget *parent)
    : QDialog(parent)
    , m_client(client)
{
    setWindowTitle(tr("传输队列"));
    resize(680, 320);

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({ tr("方向"), tr("文件"), tr("大小"),
                                         tr("进度"), tr("状态") });
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 44);
    m_table->setColumnWidth(2, 90);
    m_table->setColumnWidth(3, 60);
    m_table->setColumnWidth(4, 190);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);

    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setFormat(tr("空闲"));
    m_summary = new QLabel(tr("队列为空"), this);

    m_cancelBtn = new QPushButton(tr("取消当前"), this);
    m_cancelBtn->setEnabled(false);
    m_clearBtn = new QPushButton(tr("清除已完成"), this);
    m_closeBtn = new QPushButton(tr("隐藏"), this);
    connect(m_cancelBtn, &QPushButton::clicked, this, &TransferDialog::cancelCurrent);
    connect(m_clearBtn, &QPushButton::clicked, this, &TransferDialog::clearFinished);
    connect(m_closeBtn, &QPushButton::clicked, this, &TransferDialog::onCloseClicked);

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(m_summary, 1);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_clearBtn);
    btnRow->addWidget(m_closeBtn);

    auto *root = new QVBoxLayout(this);
    root->addWidget(m_table, 1);
    root->addWidget(m_bar);
    root->addLayout(btnRow);

    connect(m_client, &SftpClient::transferProgress, this, &TransferDialog::onProgress);
    connect(m_client, &SftpClient::transferFinished, this, &TransferDialog::onFinished);
}

void TransferDialog::enqueue(bool upload, const QString &localPath,
                             const QString &remotePath, qint64 size)
{
    Job job;
    job.id = m_nextId++;
    job.upload = upload;
    job.localPath = localPath;
    job.remotePath = remotePath;
    job.size = size;
    m_jobs.append(job);

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    for (int c = 0; c < 5; ++c)
        m_table->setItem(row, c, new QTableWidgetItem);
    updateRow(row);
    show();
    raise();
    activateWindow();
    pump();
}

void TransferDialog::pump()
{
    if (m_activeId != 0)
        return;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].state != 0)
            continue;
        Job &job = m_jobs[i];
        job.state = 1;
        job.lastMs = 0;
        job.lastBytes = 0;
        job.rateBps = 0;
        m_activeId = job.id;
        updateRow(i);
        m_cancelBtn->setEnabled(true);
        if (job.upload)
            m_client->uploadFile(job.id, job.localPath, job.remotePath);
        else
            m_client->downloadFile(job.id, job.remotePath, job.localPath);
        return;
    }
    m_bar->setValue(0);
    m_bar->setFormat(tr("空闲"));
    m_cancelBtn->setEnabled(false);
}

void TransferDialog::onProgress(int id, qint64 done, qint64 total)
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].id != id)
            continue;
        Job &job = m_jobs[i];
        job.done = done;
        if (total > job.size)
            job.size = total;

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (job.lastMs == 0) {
            job.lastMs = now;
            job.lastBytes = done;
        } else if (now - job.lastMs >= kRateIntervalMs) {
            const double inst = double(done - job.lastBytes) * 1000.0 / double(now - job.lastMs);
            // 指数平滑，避免速率数字狂跳
            job.rateBps = job.rateBps > 0 ? job.rateBps * 0.5 + inst * 0.5 : inst;
            job.lastMs = now;
            job.lastBytes = done;
        }
        updateRow(i);

        const int pct = job.size > 0 ? int(done * 100 / job.size) : 0;
        m_bar->setValue(pct);
        m_bar->setFormat(tr("%1% · %2").arg(pct).arg(humanRate(job.rateBps)));
        return;
    }
}

void TransferDialog::onFinished(int id, bool ok, const QString &error)
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].id != id)
            continue;
        Job &job = m_jobs[i];
        if (ok)
            job.state = 2;
        else if (error == QLatin1String("已取消"))
            job.state = 4;
        else
            job.state = 3;
        job.error = ok ? QString() : error;
        job.rateBps = 0;
        updateRow(i);
        break;
    }
    m_activeId = 0;

    int done = 0, failed = 0;
    for (const Job &j : m_jobs) {
        if (j.state == 2)
            ++done;
        else if (j.state == 3)
            ++failed;
    }
    m_summary->setText(tr("共 %1 项 · 成功 %2 · 失败 %3")
                           .arg(m_jobs.size()).arg(done).arg(failed));
    pump();
}

void TransferDialog::cancelCurrent()
{
    if (m_activeId != 0)
        m_client->cancelTransfer(m_activeId);
}

void TransferDialog::clearFinished()
{
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (m_jobs[i].state >= 2) {   // 完成/失败/取消
            m_jobs.removeAt(i);
            m_table->removeRow(i);
        }
    }
    if (m_jobs.isEmpty())
        m_summary->setText(tr("队列为空"));
}

void TransferDialog::onCloseClicked()
{
    hide();
}

TransferDialog::Job *TransferDialog::jobById(int id)
{
    for (Job &j : m_jobs) {
        if (j.id == id)
            return &j;
    }
    return nullptr;
}

void TransferDialog::updateRow(int row)
{
    if (row < 0 || row >= m_jobs.size())
        return;
    const Job &job = m_jobs[row];

    const QString filePath = job.upload ? job.remotePath : job.localPath;
    const QString name = QFileInfo(filePath).fileName();

    m_table->item(row, 0)->setText(job.upload ? QStringLiteral("↑") : QStringLiteral("↓"));
    m_table->item(row, 0)->setTextAlignment(Qt::AlignCenter);
    m_table->item(row, 1)->setText(name);
    m_table->item(row, 1)->setToolTip(QStringLiteral("%1 → %2").arg(
        QDir::toNativeSeparators(job.localPath), job.remotePath));
    m_table->item(row, 2)->setText(humanSize(job.size));
    m_table->item(row, 2)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    int pct = 0;
    QString stateText;
    switch (job.state) {
    case 0:
        stateText = tr("排队中");
        break;
    case 1:
        pct = job.size > 0 ? int(job.done * 100 / job.size) : 0;
        stateText = tr("传输中 %1").arg(humanRate(job.rateBps));
        if (job.rateBps > 1 && job.size > job.done) {
            const qint64 secs = qint64(double(job.size - job.done) / job.rateBps);
            stateText += tr(" · 剩 %1s").arg(qMax<qint64>(secs, 1));
        }
        break;
    case 2:
        pct = 100;
        stateText = tr("完成");
        break;
    case 3:
        stateText = tr("失败：%1").arg(job.error);
        break;
    case 4:
        stateText = tr("已取消");
        break;
    }
    m_table->item(row, 3)->setText(job.size > 0 ? QStringLiteral("%1%").arg(pct) : QStringLiteral("-"));
    m_table->item(row, 3)->setTextAlignment(Qt::AlignCenter);
    m_table->item(row, 4)->setText(stateText);
}

QString TransferDialog::humanSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("-");
    double v = double(bytes);
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    if (u == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(QString::number(v, 'f', 1), QLatin1String(units[u]));
}

QString TransferDialog::humanRate(double bps)
{
    if (bps <= 0)
        return QStringLiteral("--");
    return QStringLiteral("%1/s").arg(humanSize(qint64(bps)));
}
