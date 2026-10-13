#ifndef TERMSCREEN_H
#define TERMSCREEN_H

#include <QByteArray>
#include <QColor>
#include <QString>

#include <deque>
#include <functional>
#include <vector>

// 一个格子。颜色用「调色板下标 / 真彩色 / 默认 / 系统提示」四种，
// 主题切换只改调色板，不用把历史格子重写一遍。
struct TermColor
{
    enum Kind : quint8 { Default, Indexed, Rgb, Info };
    Kind kind = Default;
    quint8 index = 0; // 0-15，仅 Indexed
    QRgb rgb = 0;
};

struct TermCell
{
    char32_t cp = U' ';
    TermColor fg;
    TermColor bg;
    quint8 attr = 0;
    quint8 flags = 0;
};

struct TermLine
{
    std::vector<TermCell> cells;
    bool wrapped = false; // 软换行：复制时不要在这里插入换行
};

/*
 * SSH 用的屏幕缓冲。不经过 QTextDocument：一格一格写，滚动进环形历史。
 * 解析 VT：光标、擦除、滚动区、备用屏、SGR（16/256/真彩色）、设备状态应答。
 * 应答（光标位置等）通过 setReplyHandler 交回去，vim / htop 才不会干等。
 */
class TermScreen
{
public:
    static constexpr quint8 AttrBold = 1;
    static constexpr quint8 AttrUnderline = 2;
    static constexpr quint8 AttrInverse = 4;
    static constexpr quint8 FlagWide = 1;
    static constexpr quint8 FlagWideCont = 2;

    TermScreen();

    void setDark(bool dark);
    bool isDark() const { return m_dark; }

    // 返回值：从滚动历史头部丢掉的行数；-1 表示历史被整段清掉。
    int feed(const QByteArray &data);
    int appendNote(const QString &text);
    int resize(int cols, int rows);

    void setReplyHandler(std::function<void(const QByteArray &)> reply);

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    int historySize() const { return int(m_hist.size()); }
    bool altScreen() const { return m_alt; }
    bool appCursorKeys() const { return m_appCursor; }
    bool bracketedPaste() const { return m_bracketPaste; }
    bool cursorHidden() const { return m_cursorHidden; }
    bool syncing() const { return m_syncing; }
    bool takeBell();
    void setTitleHandler(std::function<void(const QString &)> title);
    int cursorRow() const { return m_row; }
    int cursorCol() const;

    // 非备用屏：0 .. historySize()-1 是滚动历史，往后是当前屏幕。
    // 备用屏：行号就是屏幕行，历史不参与。
    const TermLine *lineAbsolute(int absRow) const;

    QColor foreground(const TermCell &cell) const;
    QColor background(const TermCell &cell) const;
    QColor defaultForeground() const;
    QColor defaultBackground() const;
    QColor selectionBackground() const;

private:
    enum State { Normal, Esc, Csi, Osc, OscEsc, Charset };

    struct Pen
    {
        TermColor fg;
        TermColor bg;
        quint8 attr = 0;
    };

    void feedByte(unsigned char b);
    void dispatch(unsigned char b);
    void beginCsi();
    void pushParam();
    int arg(int index, int fallback) const;
    void handleCsi(char finalByte);
    void applySgr();
    void setPrivateMode(int mode, bool enable);
    void putCp(char32_t cp);
    void putControl(unsigned char b);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int n);
    void scrollDown(int n);
    void insertLines(int n);
    void deleteLines(int n);
    void insertChars(int n);
    void deleteChars(int n);
    void eraseCells(int row, int c0, int c1);
    void writeAt(int row, int col, char32_t cp, int width);
    void clearWide(int row, int col);
    void repairWide(int row);
    void enterAlt();
    void leaveAlt();
    void reset();
    bool lineHasText(int row) const;
    TermCell blankCell() const;
    TermCell erasedCell() const;
    TermLine blankLine() const;
    void refit(std::vector<TermLine> &buf, bool toHistory);
    int trim();
    int finishFeed();
    void replyBytes(const QByteArray &data);
    void finishOsc();
    QColor resolve(const TermColor &color, bool foreground) const;

    static constexpr int kMaxHist = 8000;

    int m_cols = 80;
    int m_rows = 24;
    int m_row = 0;
    int m_col = 0;
    int m_scrollTop = 0;
    int m_scrollBottom = 23;
    bool m_wrapPending = false;
    bool m_autowrap = true;
    bool m_alt = false;
    bool m_appCursor = false;
    bool m_bracketPaste = false;
    bool m_cursorHidden = false;
    bool m_syncing = false;    // DEC 2026：一整帧到齐再画，htop 才不会闪
    bool m_originMode = false; // DEC 6：光标原点落在滚动区
    bool m_insertMode = false;
    bool m_bell = false;
    bool m_dark = true;
    bool m_histCleared = false;
    bool m_used = false; // 还没写过内容时，改尺寸不要把空行推进滚动历史

    Pen m_pen;
    int m_scRow = 0;
    int m_scCol = 0;
    Pen m_scPen;
    int m_saveRow = 0;
    int m_saveCol = 0;

    State m_state = Normal;
    bool m_priv = false;
    char m_mod = 0;
    int m_acc = -1;
    int m_nparams = 0;
    int m_params[32] = {};
    int m_utfLeft = 0;
    char32_t m_utfAcc = 0;
    char32_t m_lastCp = 0;
    QByteArray m_osc;
    QString m_title;
    std::function<void(const QString &)> m_onTitle;

    std::vector<TermLine> m_lines;
    std::vector<TermLine> m_primary;
    std::deque<TermLine> m_hist;
    std::function<void(const QByteArray &)> m_reply;
};

#endif // TERMSCREEN_H
