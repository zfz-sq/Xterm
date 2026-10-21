#ifndef TRANSFERDIALOG_H
#define TRANSFERDIALOG_H

#include <QDialog>
#include <QElapsedTimer>
#include <QQueue>
#include <QVector>

class QLabel;
class QProgressBar;
class QTableWidget;
class SftpClient;

/*
 * TransferDialog: SFTP 传输队列窗口（非模态，随 FileBrowserDialog 存在）。
 *
 * 同一时刻只跑一个传输（SFTP 单通道内串行最稳），完成的任务自动让位
 * 给下一个排队任务。行状态: 排队 → 传输中 → 完成 / 失败 / 已取消。
 */
class TransferDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TransferDialog(SftpClient *client, QWidget *parent = nullptr);

    // 加入队列并尝试启动。upload=true 表示本地→远程。
    // reveal=false 时只入队，不把队列窗口抢到前台（编辑回传用）。
    // 返回任务 id，调用方据此认出自己的传输。
    int enqueue(bool upload, const QString &localPath, const QString &remotePath,
                qint64 size, bool reveal = true);

private slots:
    void onProgress(int id, qint64 done, qint64 total);
    void onFinished(int id, bool ok, const QString &error);
    void cancelCurrent();
    void clearFinished();
    void onCloseClicked();

private:
    struct Job
    {
        int id = 0;
        bool upload = false;
        QString localPath;
        QString remotePath;
        qint64 size = 0;
        qint64 done = 0;
        int state = 0;              // 0=排队 1=传输中 2=完成 3=失败 4=取消
        QString error;
        // 速率统计（仅传输中有效）
        qint64 lastBytes = 0;
        qint64 lastMs = 0;
        double rateBps = 0.0;
    };
    Job *jobById(int id);
    void updateRow(int row);
    void pump();                    // 无活动任务时启动下一个
    static QString humanSize(qint64 bytes);
    static QString humanRate(double bps);

    SftpClient *m_client = nullptr; // 不拥有
    QTableWidget *m_table = nullptr;
    QProgressBar *m_bar = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QVector<Job> m_jobs;            // 行号 = 下标
    int m_nextId = 1;
    int m_activeId = 0;             // 0 = 空闲
};

#endif // TRANSFERDIALOG_H
