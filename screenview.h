#ifndef SCREENVIEW_H
#define SCREENVIEW_H

#include <QElapsedTimer>
#include <QWidget>

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
 * Ctrl+Shift+F 在滚动历史里查找。大块输出按片解析，界面保持能打字。
 * 主屏可接客户端 Tab 补全（嵌入式 shell 多数不吃 Tab，服务端补不了）。
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
    // 当前视口的可见文本（逐行去尾空白），测试与诊断辅助
    QString screenText() const;
    // 字号（pt）。设置成功（有变化）时发 fontSizeChanged；全局设置由主窗口联动两页
    int fontSize() const { return m_font.pointSize(); }
    void setFontSize(int pt);

signals:
    void sendData(const QByteArray &data);
    void terminalSizeChanged(int cols, int rows);
    void shellTitleChanged(const QString &title);
    void fontSizeChanged(int pt);

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
    QByteArray arrowKey(char letter) const;
    void paintCursor(QPainter &painter);
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
    bool m_follow = true;
    bool m_barLock = false;
    bool m_selecting = false;
    bool m_wasAlt = false;
    Pos m_anchor;
    Pos m_focus;

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
