#ifndef SCREENVIEW_H
#define SCREENVIEW_H

#include <QElapsedTimer>
#include <QWidget>

#include "commandstore.h"
#include "termscreen.h"

class CompletionEngine;
class QLineEdit;
class QScrollBar;
class QTimer;

/*
 * 屏幕式终端视图（SSH / 串口共用）。格子缓冲 + 自绘，键盘原样送给会话，
 * top / vim 这类全屏程序靠格子模型的光标定位 + 备用屏才能还原。
 * 滚轮看滚动历史；备用屏（vim/htop）里滚轮变成方向键。
 * 选中即复制，右键粘贴，Ctrl+滚轮改字号。
 * Ctrl+F / Ctrl+Shift+F 在滚动历史里查找（全屏程序里 Ctrl+F 直通给程序）。
 * 大块输出按片解析，界面保持能打字。
 * 主屏可接客户端 Tab 补全（嵌入式 shell 多数不吃 Tab，服务端补不了）。
 * 串口本地行编辑与全屏程序共存：远端一旦画屏（备用屏，或 BusyBox top
 * 这类只发光标定位不切备用屏的），行编辑自动让路变直通；输出回落到
 * 提示符样的纯文本（行中有字、停在行中）时再自动恢复行编辑。
 */
class ScreenView : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenView(QWidget *parent = nullptr);

    void applyTheme(bool dark);
    void receiveData(const QByteArray &data);
    void appendSystemMessage(const QString &text);
    void setCompletionEngine(CompletionEngine *engine);
    // 串口本地编辑：键入和粘贴只画在屏幕上，回车才把改完的整行发出去。
    // SSH 保持关闭，按键仍立刻送给远端。全屏程序（备用屏或远端画屏）期间
    // 自动让路，见 localLineMode()。
    void setLocalEcho(bool on) { m_localEcho = on; }
    // 当前视口的可见文本（逐行去尾空白），测试与诊断辅助
    QString screenText() const;
    // 滚动历史末尾加上当前屏，供本机控制口读取。maxLines 含当前屏。
    QString recentText(int maxLines) const;
    bool usingAltScreen() const { return m_screen.altScreen(); }
    // 本地回显打开时，把已经发出去的字节画到屏幕上（不再次发送）。
    void showOutbound(const QByteArray &bytes);
    // 只清本机画面和滚动历史，不向设备发送任何字节。
    void clearLocalDisplay();
    // 记入串口上下键可以翻回的已发送行（命令按钮、本机控制口也走这里）。
    void rememberSentLine(const QString &line);
    // 字号（pt）。设置成功（有变化）时发 fontSizeChanged；全局设置由主窗口联动两页
    int fontSize() const { return m_font.pointSize(); }
    void setFontSize(int pt);
    // 关键词高亮：命中的文本按规则颜色绘制（不区分大小写，含滚动历史），
    // 选中 / Ctrl+F 查找高亮优先级更高。空列表即关闭。
    void setKeywords(const QList<KeywordRule> &rules);
    // 绘制内边距（默认 6px）。最大化时主窗口会置 0，让终端网格贴满控件边缘
    void setPad(int pad);

signals:
    void sendData(const QByteArray &data);
    void terminalSizeChanged(int cols, int rows);
    void shellTitleChanged(const QString &title);
    void fontSizeChanged(int pt);
    // 选中词右键「高亮此词」：把选中文本交给主窗口建关键词高亮规则
    void highlightRequested(const QString &word);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Pos
    {
        int abs = 0;
        int col = 0;
        bool valid = false;
    };
    // 一条关键词命中：列区间 [start, end]（含端点）与绘制颜色
    struct KeywordHit
    {
        int start = 0;
        int end = 0;
        QColor color;
    };

    void recalcMetrics();
    void updateGrid();
    void emitPtySize();
    void afterOutput(int dropped);
    void syncBar();
    void scrollLocal(int deltaRows);
    void zoom(int steps);
    void pumpRx();
    void flushRx();
    void placeFind();
    void openFind();
    void closeFind();
    bool runFind(bool forward);
    QString selectedText() const;
    void selectLine(int absRow);
    void pasteClipboard();
    void copySelection();
    void clearSelection();
    void nudgeSelection(int deltaAbs);
    bool selectionBounds(int &r0, int &c0, int &r1, int &c1) const;
    bool cellSelected(int abs, int col) const;
    Pos cellAt(QPoint pt) const;
    TermCell cellOrBlank(const TermLine *line, int col) const;
    // 一行里所有关键词命中的列区间（绘制时调用，规则空则清空不扫）
    void collectKeywordHits(const TermLine *line, int cols, QList<KeywordHit> *hits) const;
    QByteArray arrowKey(char letter) const;
    void paintCursor(QPainter &painter);
    void emitInput(const QByteArray &bytes);
    void echoLocal(const QByteArray &bytes);
    bool localLineMode() const;
    // 远端画屏检测：进入（丢弃未发出的编辑草稿）/退出（回到行编辑）
    void applyRemoteCtl();
    void enterRemoteRaw();
    void exitRemoteRaw();
    bool handleLocalLineKey(QKeyEvent *event);
    void beginLocalEdit();
    void redrawLocalEdit();
    void localInsert(const QString &text);
    void commitLocalLine();
    void localTab();
    void recallSent(int direction);
    void handleTabCompletion();
    void advanceCompletionCycle();
    void resetCompletionCycle();
    // 主屏光标所在行的可见文本 + 光标列，补全取词用；备用屏返回空
    QString inputLineText(int *cursorColOut) const;

    TermScreen m_screen;
    QScrollBar *m_bar = nullptr;
    QTimer *m_sizeTimer = nullptr;
    QTimer *m_pump = nullptr;
    QTimer *m_syncHold = nullptr;
    QTimer *m_bellTimer = nullptr;
    QLineEdit *m_find = nullptr;
    QByteArray m_rx;
    int m_rxUsed = 0;
    bool m_bell = false;
    int m_hitAbs = -1;
    int m_hitCol = 0;
    int m_hitEnd = -1;
    QElapsedTimer m_clicks;
    qint64 m_lastDoubleMs = 0;
    QFont m_font;
    QFont m_boldFont;

    int m_cellW = 8;
    int m_cellH = 16;
    int m_ascent = 12;
    int m_pad = 6;
    int m_origin = 0;
    int m_sentCols = -1;
    int m_sentRows = -1;
    bool m_localEcho = false;
    bool m_remoteRaw = false;  // 远端画屏中（BusyBox top 等），行编辑让路、按键直通
    bool m_editOn = false;     // 串口行编辑：未回车的内容还没发给设备
    bool m_editRedraw = false;
    int m_editAbs = 0;         // 这一行的绝对行号
    int m_editCol = 0;         // 行内起始列
    QString m_edit;
    int m_editPos = 0;
    QStringList m_sentHist;
    int m_histBrowse = -1; // -1 表示正在编辑新行，不是在翻历史
    QString m_histDraft;
    bool m_follow = true;
    bool m_barLock = false;
    bool m_selecting = false;
    bool m_wasAlt = false;
    Pos m_anchor;
    Pos m_focus;
    QList<KeywordRule> m_keywords; // 关键词高亮规则（全局设置同步而来）

    // ---- 客户端 Tab 补全（主屏 + 串口/SSH shell 行编辑场景）----
    CompletionEngine *m_completion = nullptr;
    QStringList m_cycle;        // 候选列表，空表示不在 Cycle 中
    int m_cycleStart = -1;      // 词首列
    int m_cycleEnd = -1;        // 词尾列（含已发出但回显未到的字符）
    int m_cyclePending = 0;     // 已发送、设备回显还没到的字符数
    int m_cycleState = -1;      // -1=原始词，0..n-1=正在展示的候选
    QString m_cycleOriginal;    // 回绕时恢复的原始词
};

#endif // SCREENVIEW_H
