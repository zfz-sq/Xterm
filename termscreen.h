#ifndef TERMSCREEN_H
#define TERMSCREEN_H

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

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
    // OSC 8 超链接的 URI 池下标（TermScreen::uriAt 取串）；NoUri = 非链接
    static constexpr quint32 NoUri = 0xFFFFFFFFu;
    char32_t cp = U' ';
    TermColor fg;
    TermColor bg;
    quint8 attr = 0;
    quint8 flags = 0;
    quint32 uri = NoUri;
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
    // 滚动历史行数上限（环形：超限丢最老，长跑内存封顶）。可配，默认 10000
    void setMaxHistory(int lines);
    int maxHistory() const { return m_maxHist; }
    // 环形丢掉的最老行累计（含清屏/复位丢弃）——绝对行号 = dropped +
    // historySize + rows，单调不回退，wait 锚点靠它不漂移
    qint64 droppedLines() const { return m_histDropped; }
    // 本次 feed 触到的屏幕行范围快照（含光标行；-1 = 无触达），供
    // ScreenView 脏矩形用。与补色（paintTouchedLines）同源，下块覆盖
    void feedTouchedRows(int *minRow, int *maxRow) const
    { *minRow = m_feedTouchMin; *maxRow = m_feedTouchMax; }
    bool altScreen() const { return m_alt; }
    bool appCursorKeys() const { return m_appCursor; }
    bool bracketedPaste() const { return m_bracketPaste; }
    bool cursorHidden() const { return m_cursorHidden; }
    bool syncing() const { return m_syncing; }
    // 鼠标上报（DECSET 1000/1002/1003，互斥，后设覆盖前设，DECRST 只关
    // 自己）：0 = 关闭；1000 = 按下/释放；1002 = 加按住拖动；1003 = 任意
    // 移动。1006 是 SGR 编码开关，与跟踪模式独立。
    int mouseTrackingMode() const { return m_mouseTrack; }
    bool mouseSgrEncoding() const { return m_mouseSgr; }
    // 本块是否出现光标定位（CUP/HVP）或整屏擦除（ED 2/3）：BusyBox top、
    // menuconfig 这类不切备用屏的全屏程序靠它认出来。每次 feed() 清零。
    bool screenCtlSeen() const { return m_feedCtl > 0; }
    // 本块写入的可见字符数（feed() 清零），区分「画屏」与「回显了几个字」
    int feedPrintable() const { return m_feedPrintable; }
    // 本块处理的换行数（feed() 清零）。提示符一定出现在新行上，vi 插入模式
    // 的逐字回显永远不带换行——恢复行编辑前必须见到换行。
    int feedNewlines() const { return m_feedNewlines; }
    // 本块最后一次换行之后写入的可见字符数（feed() 清零）。提示符的字一定
    // 写在换行之后；vi 插入模式「打字+回车」的回显字全在换行之前——用它
    // 把两者分开，防止 vi 里一按回车就掉回本地行编辑（按键被扣、vi 卡死）。
    int feedPrintableAfterLf() const { return m_feedPrintableAfterLf; }
    bool takeBell();
    void setTitleHandler(std::function<void(const QString &)> title);
    // OSC 52（远端写剪贴板）解码后的文本经此回调交上层（策略/弹窗归视图层）
    void setClipboardHandler(std::function<void(const QByteArray &)> handler);
    // URI 池取串（TermCell::uri 下标）；越界/NoUri 返回空串
    QString uriAt(quint32 index) const;
    // 清掉可见内容和滚动历史，光标回到左上角。返回 -1，视图据此跳到空白屏底。
    int clearDisplay();
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
        quint32 uri = TermCell::NoUri; // OSC 8：本段文字的链接（空 URI 关闭）
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
    void clearScrollback();
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
    // 本次 feed 触到的行收尾时补一次「ip -c」风格的网络词着色
    void touchRow(int row);
    void touchAll();
    void paintTouchedLines();
    void paintIpTokens(TermLine &line);
    void replyBytes(const QByteArray &data);
    void finishOsc();
    // OSC 8 链接池：同一 URL 只存一份，cell 存下标（长会话去重后可控）
    quint32 internUri(const QString &uri);
    QColor resolve(const TermColor &color, bool foreground) const;

    static constexpr int kDefaultMaxHist = 10000;

    int m_maxHist = kDefaultMaxHist;  // 环形历史上限（setMaxHistory 可调）
    qint64 m_histDropped = 0;         // 环形/清屏累计丢弃行数（绝对行号基数）

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
    int m_mouseTrack = 0;      // DEC 1000/1002/1003：鼠标上报模式，0 = 关
    bool m_mouseSgr = false;   // DEC 1006：SGR 编码（\e[<b;x;yM/m）
    bool m_originMode = false; // DEC 6：光标原点落在滚动区
    bool m_insertMode = false;
    bool m_bell = false;
    bool m_dark = true;
    bool m_histCleared = false;
    bool m_clearRequested = false; // 本块里出现过清屏（ED），收尾时再丢一次历史
    int m_clearFollowups = 0;      // 清屏后紧接着的几块若把旧屏滚进历史，也丢掉
    int m_feedScrolled = 0;        // 本块推进滚动历史的行数
    int m_feedPrintable = 0;       // 本块写入的非空格字符数
    int m_feedNewlines = 0;        // 本块处理的换行数（提示符行的判据）
    int m_feedPrintableAfterLf = 0; // 本块最后一次换行后写入的非空格字符数
    int m_rowsErased = 0;          // 本块被整行擦掉的屏幕行数
    int m_feedCtl = 0;             // 本块的光标定位/整屏擦除次数（全屏程序标志）
    quint8 m_rowErased[200] = {};
    bool m_used = false; // 还没写过内容时，改尺寸不要把空行推进滚动历史
    int m_touchMin = -1;       // 本次 feed 触到的屏幕行范围（收尾补色用），-1 表示无
    int m_touchMax = -1;
    int m_feedTouchMin = -1;   // 同范围快照（feed 后供 ScreenView 脏矩形读）
    int m_feedTouchMax = -1;
    int m_unscannedHist = 0;   // 滚动历史里还没补过色的首行下标

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
    std::function<void(const QByteArray &)> m_onClipboard;
    QVector<QString> m_uriPool;          // OSC 8 链接池（下标即 TermCell::uri）
    QHash<QString, quint32> m_uriIndex;  // 去重索引

    std::vector<TermLine> m_lines;
    std::vector<TermLine> m_primary;
    std::deque<TermLine> m_hist;
    std::function<void(const QByteArray &)> m_reply;
};

#endif // TERMSCREEN_H
