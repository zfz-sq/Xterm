#ifndef TRANSFERDIALOG_H
#define TRANSFERDIALOG_H

#include <QElapsedTimer>
#include <QQueue>
#include <QVector>

#include "framelessdialog.h"

class QLabel;
class QProgressBar;
class QTableWidget;
class SftpClient;

/*
 * TransferDialog: SFTP 传输队列窗口（非模态，随 FileBrowserDialog 存在）。
 *
 * 同一时刻只跑一个传输（SFTP 单通道内串行最稳），完成的任务自动让位
 * 给下一个排队任务。行状态: 排队 → 传输中 → 完成 / 失败 / 已取消。
 * 顶层是公共无边框对话框（FramelessDialog），随全局 QSS 换肤。
 */
class TransferDialog : public FramelessDialog
{
    Q_OBJECT

signals:
    // 任一任务到达终态（完成/失败/取消，含建目录任务），调用方据此跟踪批次
    void jobFinished(int id, bool ok);
    // 队列状态快照：active=有任务在跑（percent 0-100），text 为展示文案；
    // 队列收尾时 active=false、text 为汇总。供状态栏进度条消费。
    void progressChanged(const QString &text, int percent, bool active);

public:
    explicit TransferDialog(SftpClient *client, QWidget *parent = nullptr);

    // 加入队列并尝试启动。upload=true 表示本地→远程。
    // 返回任务 id，调用方据此认出自己的传输。
    int enqueue(bool upload, const QString &localPath, const QString &remotePath,
                qint64 size);
    // 文件夹上传的建目录任务：与文件传输同一队列、同一套 id，
    // 严格排队保证「先建目录后传里面的文件」。localDir 仅作提示展示。
    int enqueueDir(const QString &localDir, const QString &remoteDir);

private slots:
    void onProgress(int id, qint64 done, qint64 total);
    void onFinished(int id, bool ok, const QString &error);
    void onOpFinished(const QString &op, const QString &arg, bool ok, const QString &error);
    void cancelCurrent();
    void clearFinished();
    void onCloseClicked();

private:
    struct Job
    {
        int id = 0;
        bool upload = false;
        bool makeDir = false;      // 建目录任务（无传输、无进度，瞬时完成）
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
    int appendJob(const Job &job);  // 公共入队尾巴：加行、启动
    void finishJob(int id, bool ok, const QString &error); // 置终态 + 汇总 + 接力
    void emitProgress();            // 按当前队列状态发一次 progressChanged
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
