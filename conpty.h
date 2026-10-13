#ifndef CONPTY_H
#define CONPTY_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class PtyTask;

/*
 * ConPty: Windows 伪终端（ConPTY，Win10 1809+ 内建于 kernel32）。
 *
 * 在伪终端里拉起一个控制台程序（本项目是系统自带的 ssh.exe），该程序面对的
 * 就是真正的 tty：远端分配 PTY、开启回显、输出 ANSI 彩色，top/vim 一类全屏
 * 程序也照常可用 —— 交互语义与自研 SSH shell 通道等价，但不引入任何第三方库。
 *
 * 线程模型（这是"不卡 UI"的关键）：
 *   CreateProcess / ReadFile / WriteFile / TerminateProcess 全部在 PtyTask
 *   工作线程里做；GUI 线程只通过 queued 信号收数据、通过线程安全的方法投递
 *   输入与尺寸。因此远端阻塞、输出风暴、子进程不退出，都拖不住界面。
 *
 * 句柄生命周期全部由 PtyTask 持有，析构 ConPty 前必须先 stop()+waitForFinished()。
 */
class ConPty : public QObject
{
    Q_OBJECT

public:
    explicit ConPty(QObject *parent = nullptr);
    ~ConPty() override;

    // 系统是否具备 ConPTY API。false 时 SSH 功能应给出明确提示而不是崩。
    static bool isSupported();

    // 异步拉起子进程。返回 false 仅表示本对象已在运行（明显不对的调用），
    // 真正的启动结果通过 launched() / finished() 通知。
    bool start(const QString &program, const QStringList &args, int cols, int rows);

    void write(const QByteArray &data);         // 投递到 IO 线程，永不阻塞
    void resize(int cols, int rows);            // 合并到 IO 线程下一次循环处理
    void stop();                                // 优雅 → 超时强杀，不阻塞调用线程
    void waitForFinished(int msecs);            // 仅程序退出路径使用
    bool isRunning() const;
    void reset();                               // 回收已结束的任务对象，之后才能再次 start()

signals:
    void launched(const QString &errorText);    // 空 = 子进程已起来
    void outputReceived(const QByteArray &chunk);
    void finished(int exitCode, bool terminatedByUs);

private:
    PtyTask *m_task = nullptr;
};

#endif // CONPTY_H
