#ifndef TERMINALVIEW_H
#define TERMINALVIEW_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QStringConverter>
#include <QStringDecoder>
#include <QStringList>
#include <QTextCharFormat>

class CompletionEngine;
class QTimer;

/*
 * 集成式终端视图（显示 + 输入在同一个页面）:
 *  - receiveData(): UTF-8 解码 + ANSI/VT100 转义序列解析（SGR 颜色含
 *    256 色/真彩色、光标移动、清行/清屏等），所见即设备输出。
 *  - 设备没上色的 IPv4/IPv6、MAC、常见网卡名、UP/DOWN，按 Ubuntu ip -c
 *    的配色在本地补上。
 *  - 键盘按键直通转发 sendData()（Enter、退格、方向键、Ctrl 组合键），
 *    行编辑 / Tab 补全 / 历史都由设备端 shell 完成。
 *  - setLocalEcho(): 远端未开启回显时可打开本地回显。
 *  - 客户端命令补全（setCompletionEngine() 后启用）：Tab 接管，按 CommandBar
 *    预设 + 内置常用 Linux 命令做前缀匹配；多候选项先补到最长公共前缀，
 *    再连续 Tab 在候选间循环选择。SSH 侧不挂引擎（远端 bash 自带补全）。
 *  - 接收侧：交互小包（键入回显、单条命令输出）同步渲染，零延迟；突发输出
 *    （cat 大文件、top 全量重绘）按时间窗合并投递 + 分片渲染，界面不可见时
 *    再放宽节奏并给积压设上限，既不卡也不无限吃内存。
 *  - terminalSizeChanged(): 视口换算出的行列尺寸，用于给伪终端设窗口大小。
 */
class TerminalView : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit TerminalView(QWidget *parent = nullptr);

    void applyTheme(bool dark); // 切换终端配色，并重映射已有文本的前景/背景
    void receiveData(const QByteArray &data);
    void appendSystemMessage(const QString &text);
    void setLocalEcho(bool on) { m_localEcho = on; }
    bool localEcho() const { return m_localEcho; }

    void setCompletionEngine(CompletionEngine *e) { m_completion = e; }

    int terminalColumns() const;
    int terminalRows() const;

signals:
    void sendData(const QByteArray &bytes); // 键盘字节 → Transport::send()
    void terminalSizeChanged(int cols, int rows); // 伪终端窗口尺寸

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool event(QEvent *event) override; // 捕获字号变化（Ctrl+滚轮）后重算尺寸

private:
    void scheduleFlush();
    void flushIncoming();   // 把所有已到达数据落地（按键 / 系统提示前保序用）
    void feedIncoming();    // 定时器驱动的增量渲染
    void renderBytes(const QByteArray &chunk);
    void renderNextSlice(); // 从缓冲区取一片渲染，单帧耗时可预算
    void trimHiddenBacklog();
    bool takeSyncBudget(int size);
    int pendingBytes() const { return m_inBuffer.size() - m_inUsed; }
    void reportTerminalSize();

    enum State
    {
        StateNormal,
        StateEsc,
        StateCsi,
        StateOsc,
        StateCharset
    };

    void feedChar(QChar c);
    void flushPending();
    void handleCsi(const QString &params, char finalByte);
    void applySgr(const QList<int> &params);
    void moveCursorOp(QTextCursor::MoveOperation op, int n);
    void setCursorRowCol(int row, int col);
    void setCursorColumn(int col);
    void eraseLine(int mode);
    void eraseDisplay(int mode);
    void resetScreen();      // RIS（ESC c）整屏复位：clear / reset 的另一条通路
    void lineFeed();         // LF：终端语义是「下移一行」，不是「插入一行」
    void eraseCharBeforeCursor();

    // Tab 补全
    void handleTabCompletion();
    void advanceCompletionCycle();
    void resetCompletionCycle();

    State m_state = StateNormal;
    QString m_csiParams;
    QStringDecoder m_decoder{QStringConverter::Utf8};
    QTextCursor m_cursor;      // 输出流光标（与用户选择光标独立）
    QTextCharFormat m_fmt;     // 当前 RX 字符属性（随 SGR 变化）
    QTextCharFormat m_echoFmt; // 本地回显颜色
    QColor m_infoColor;        // 系统提示颜色
    QString m_pending;         // 待插入的连续普通文本（批量插入提高性能）
    bool m_overwrite = false;  // 覆盖模式: \r/\b 之后的字符替换行内字符而非插入
    bool m_localEcho = false;
    bool m_docDark = true;     // 本文档当前是按哪套配色渲染的（主题切换时用来重映射）

    CompletionEngine *m_completion = nullptr;

    // 接收合并：远端突发输出先落进缓冲区，一次事件循环渲染一次
    QByteArray m_inBuffer;
    int m_inUsed = 0; // m_inBuffer 中已消费的前缀长度，避免反复 memmove
    QTimer *m_flushTimer = nullptr;
    QTimer *m_sizeTimer = nullptr; // 尺寸去抖，拖动窗口时不刷屏重绘
    int m_reportedCols = 0;
    int m_reportedRows = 0;
    // 零延迟通道：交互小包直接同步渲染，但按时间窗限制总量，突发输出仍走合并
    QElapsedTimer m_syncClock;
    qint64 m_syncWindowStart = 0; // 当前同步窗口的起始时刻
    int m_syncBytes = 0;          // 本窗口内已同步渲染的字节数

    // 补全 Cycle 状态：
    //   m_cycleStart/-End: 当前正在补全的词在文档中的列位置（按 m_cursor.block() 内的位置）
    //   m_cycleOriginal: 进入补全时用户输入的原始前缀（首次 Tab 时的前缀）
    //                    若 LCP 大于原前缀，则被替换为 LCP（成为 wrap 回退点）
    //   m_cycle: 候选项列表（按补全算法顺序，已排序去重）
    //   m_cycleState: -1 表示当前显示的是 m_cycleOriginal；
    //                 0..m_cycle.size()-1 表示当前显示的是 m_cycle[state]
    int m_cycleStart = -1;
    int m_cycleEnd = -1;
    QString m_cycleOriginal;
    QStringList m_cycle;
    int m_cycleState = -1;
};

#endif // TERMINALVIEW_H
