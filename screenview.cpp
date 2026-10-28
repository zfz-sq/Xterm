#include "screenview.h"

#include <QClipboard>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QVector>
#include <QWheelEvent>

#include <utility>

#include "completionengine.h"

namespace {

bool wordChar(char32_t cp)
{
    if (cp <= 0x20)
        return false;
    if (cp > 0xFFFF)
        return true;
    const QChar ch = QChar(ushort(cp));
    return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('-')
           || ch == QLatin1Char('.') || ch == QLatin1Char('/') || ch == QLatin1Char(':');
}

} // namespace

ScreenView::ScreenView(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAttribute(Qt::WA_KeyCompression, false);
    setCursor(Qt::IBeamCursor);
    setMinimumSize(160, 80);

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const int systemPt = m_font.pointSize();
    m_font.setPointSize(qMax(9, systemPt > 0 ? systemPt : 9) + 2);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    m_font.setKerning(false);
    m_boldFont = m_font;
    m_boldFont.setBold(true);
    setFont(m_font);
    recalcMetrics();

    m_screen.setReplyHandler([this](const QByteArray &bytes) { emit sendData(bytes); });
    m_screen.setTitleHandler([this](const QString &title) { emit shellTitleChanged(title); });
    m_screen.setDark(true);
    m_clicks.start();

    m_bar = new QScrollBar(Qt::Vertical, this);
    m_bar->setFocusPolicy(Qt::NoFocus);
    connect(m_bar, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_barLock)
            return;
        m_origin = value;
        m_follow = !m_screen.altScreen() && value >= m_screen.historySize();
        update();
    });

    m_sizeTimer = new QTimer(this);
    m_sizeTimer->setSingleShot(true);
    connect(m_sizeTimer, &QTimer::timeout, this, &ScreenView::emitPtySize);

    m_pump = new QTimer(this);
    m_pump->setSingleShot(true);
    connect(m_pump, &QTimer::timeout, this, &ScreenView::pumpRx);

    m_syncHold = new QTimer(this);
    m_syncHold->setSingleShot(true);
    connect(m_syncHold, &QTimer::timeout, this, [this] {
        syncBar();
        update();
    });

    m_bellTimer = new QTimer(this);
    m_bellTimer->setSingleShot(true);
    connect(m_bellTimer, &QTimer::timeout, this, [this] {
        m_bell = false;
        update();
    });

    m_find = new QLineEdit(this);
    m_find->setPlaceholderText(QStringLiteral("查找    Enter/F3 下一个   Shift+Enter 上一个   Esc 关闭"));
    m_find->hide();
    m_find->installEventFilter(this);
}

void ScreenView::applyTheme(bool dark)
{
    m_screen.setDark(dark);
    if (m_find) {
        const QString bg = dark ? QStringLiteral("#2C2C30") : QStringLiteral("#FFFFFF");
        const QString fg = dark ? QStringLiteral("#D4D4D4") : QStringLiteral("#24292E");
        const QString bd = dark ? QStringLiteral("#3B8EEA") : QStringLiteral("#2472C8");
        m_find->setStyleSheet(QStringLiteral(
                                  "QLineEdit { background:%1; color:%2; border:1px solid %3;"
                                  " border-radius:3px; padding:2px 8px; }")
                                  .arg(bg, fg, bd));
    }
    update();
}

void ScreenView::receiveData(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    if (m_rxUsed > 0 && m_rxUsed >= m_rx.size()) {
        m_rx.clear();
        m_rxUsed = 0;
    }
    m_rx += data;
    const int avail = m_rx.size() - m_rxUsed;
    // 键入回显这种小包当场解析。大块先让出事件循环，打字不会被 cat 堵住。
    if (avail <= 4096 && !m_pump->isActive())
        pumpRx();
    else if (!m_pump->isActive())
        m_pump->start(0);
}

void ScreenView::appendSystemMessage(const QString &text)
{
    flushRx();
    afterOutput(m_screen.appendNote(text));
}

void ScreenView::pumpRx()
{
    const int avail = m_rx.size() - m_rxUsed;
    if (avail <= 0) {
        m_rx.clear();
        m_rxUsed = 0;
        return;
    }
    int budget = avail <= 4096 ? avail : 48 * 1024;
    if (m_screen.syncing())
        budget = qMin(avail, 256 * 1024);
    const QByteArray slice = m_rx.mid(m_rxUsed, budget);
    m_rxUsed += slice.size();
    if (m_rxUsed >= 256 * 1024 && m_rxUsed > (m_rx.size() / 2)) {
        m_rx.remove(0, m_rxUsed);
        m_rxUsed = 0;
    }
    const int dropped = m_screen.feed(slice);
    if (!m_editRedraw) // 本地行编辑重画自带光标定位，不能当远端画屏
        applyRemoteCtl();
    afterOutput(dropped);
    if (m_rxUsed < m_rx.size())
        m_pump->start(0);
}

void ScreenView::flushRx()
{
    if (m_pump->isActive())
        m_pump->stop();
    if (m_rxUsed >= m_rx.size()) {
        m_rx.clear();
        m_rxUsed = 0;
        return;
    }
    const QByteArray rest = m_rx.mid(m_rxUsed);
    m_rx.clear();
    m_rxUsed = 0;
    const int dropped = m_screen.feed(rest);
    if (!m_editRedraw)
        applyRemoteCtl();
    afterOutput(dropped);
}

void ScreenView::recalcMetrics()
{
    const QFontMetrics fm(m_font);
    m_cellW = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellH = qMax(1, fm.lineSpacing());
    m_ascent = fm.ascent();
}

void ScreenView::updateGrid()
{
    if (!m_bar || width() < m_cellW || height() < m_cellH)
        return;
    const int barW = qMax(14, m_bar->sizeHint().width());
    m_bar->setGeometry(width() - barW, 0, barW, height());
    const int cols = qMax(20, (width() - barW - m_pad * 2) / m_cellW);
    const int rows = qMax(4, (height() - m_pad * 2) / m_cellH);
    if (cols == m_screen.cols() && rows == m_screen.rows()) {
        if (m_sentCols != cols || m_sentRows != rows)
            m_sizeTimer->start(40);
        return;
    }
    const int dropped = m_screen.resize(cols, rows);
    afterOutput(dropped);
    m_sizeTimer->start(40);
}

void ScreenView::setPad(int pad)
{
    if (m_pad == pad)
        return;
    m_pad = pad;
    updateGrid(); // 内边距变化会改变可用列数/行数，需重算并通知 pty
    update();
}

void ScreenView::emitPtySize()
{
    const int cols = m_screen.cols();
    const int rows = m_screen.rows();
    if (cols == m_sentCols && rows == m_sentRows)
        return;
    m_sentCols = cols;
    m_sentRows = rows;
    emit terminalSizeChanged(cols, rows);
}

void ScreenView::afterOutput(int dropped)
{
    const bool alt = m_screen.altScreen();
    if (alt != m_wasAlt)
        clearSelection();
    m_wasAlt = alt;

    if (alt) {
        m_origin = 0;
        m_follow = true;
    } else if (dropped < 0) {
        clearSelection();
        m_follow = true;
        m_origin = m_screen.historySize();
    } else {
        if (dropped > 0) {
            m_origin = qMax(0, m_origin - dropped);
            nudgeSelection(-dropped);
        }
        if (m_origin > m_screen.historySize())
            m_origin = m_screen.historySize();
        if (m_follow)
            m_origin = m_screen.historySize();
    }
    if (m_editOn && !m_editRedraw) {
        if (dropped < 0) {
            m_editAbs = m_screen.historySize() + m_screen.cursorRow();
            m_editCol = m_screen.cursorCol();
        }
        redrawLocalEdit();
    }
    if (m_screen.takeBell()) {
        m_bell = true;
        m_bellTimer->start(140);
    }
    // 输出是成块到达的（top 一轮刷新就是 3 块），逐块立绘会把半帧画出来。
    // 统一走短合并窗：8ms 内到达的块合成一次重绘（对键入回显的延迟不可感）；
    // 同步更新（BSA 2026）期间窗口放宽到 80ms 等帧尾，结束后回落 8ms 立即收帧。
    const int holdMs = m_screen.syncing() ? 80 : 8;
    if (!m_syncHold->isActive())
        m_syncHold->start(holdMs);
    else if (m_syncHold->remainingTime() > holdMs)
        m_syncHold->start(holdMs); // 窗内后续块不重置计时，只收紧不放宽
}

void ScreenView::syncBar()
{
    const int max = m_screen.altScreen() ? 0 : m_screen.historySize();
    m_barLock = true;
    m_bar->setRange(0, max);
    m_bar->setPageStep(qMax(1, m_screen.rows()));
    m_bar->setSingleStep(1);
    m_bar->setValue(qBound(0, m_origin, max));
    m_barLock = false;
}

void ScreenView::scrollLocal(int deltaRows)
{
    if (m_screen.altScreen())
        return;
    const int max = m_screen.historySize();
    m_origin = qBound(0, m_origin + deltaRows, max);
    m_follow = m_origin >= max;
    syncBar();
    update();
}

void ScreenView::setFontSize(int pt)
{
    const int next = qBound(8, pt, 36);
    if (next == m_font.pointSize())
        return;
    m_font.setPointSize(next);
    m_boldFont = m_font;
    m_boldFont.setBold(true);
    setFont(m_font);
    recalcMetrics();
    updateGrid();
    update();
    emit fontSizeChanged(next);
}

void ScreenView::setKeywords(const QList<KeywordRule> &rules)
{
    m_keywords = rules;
    update(); // 已在屏上的内容立即按新规则重画
}

void ScreenView::collectKeywordHits(const TermLine *line, int cols,
                                     QList<KeywordHit> *hits) const
{
    hits->clear();
    // 可见文本 + 每个字符的起始列与占格数。宽字符（CJK/emoji）占两列但只算首格，
    // 代理对的第二个 QChar 沿用同列号，命中区间按列展开才对得上画面。
    QString text;
    QVector<int> colOf;
    QVector<int> spanOf;
    for (int col = 0; col < cols; ++col) {
        const TermCell cell = cellOrBlank(line, col);
        if (cell.flags & TermScreen::FlagWideCont)
            continue;
        const char32_t cp = cell.cp ? cell.cp : U' ';
        const int before = text.size();
        text += QString::fromUcs4(&cp, 1);
        const int added = text.size() - before;
        const int span = (cell.flags & TermScreen::FlagWide) ? 2 : 1;
        colOf.append(col);
        spanOf.append(span);
        for (int e = 1; e < added; ++e) {
            colOf.append(col);
            spanOf.append(0);
        }
    }
    if (text.isEmpty())
        return;
    for (const KeywordRule &rule : m_keywords) {
        if (!rule.enabled || rule.text.isEmpty())
            continue;
        int from = 0;
        while (true) {
            const int idx = text.indexOf(rule.text, from, Qt::CaseInsensitive);
            if (idx < 0)
                break;
            const int last = idx + rule.text.size() - 1;
            const int start = colOf.at(idx);
            const int end = colOf.at(last) + qMax(spanOf.at(last), 1) - 1;
            hits->append({ start, end, rule.color });
            from = idx + rule.text.size();
        }
    }
}

void ScreenView::zoom(int steps)
{
    setFontSize(m_font.pointSize() + steps);
}

void ScreenView::pasteClipboard()
{
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    if (text.size() > 1000000)
        text.truncate(1000000);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (localLineMode()) {
        const QStringList parts = text.split(QLatin1Char('\n'));
        for (int i = 0; i < parts.size(); ++i) {
            if (!parts.at(i).isEmpty())
                localInsert(parts.at(i));
            if (i + 1 < parts.size())
                commitLocalLine();
        }
        return;
    }
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    QByteArray bytes = text.toUtf8();
    bytes.replace('\0', "");
    if (bytes.isEmpty())
        return;
    if (m_screen.bracketedPaste())
        bytes = QByteArray("\x1b[200~") + bytes + QByteArray("\x1b[201~");
    emitInput(bytes);
}

void ScreenView::copySelection()
{
    const QString out = selectedText();
    if (!out.isEmpty())
        QGuiApplication::clipboard()->setText(out);
}

QString ScreenView::selectedText() const
{
    int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
    if (!selectionBounds(r0, c0, r1, c1))
        return QString();
    QString out;
    for (int row = r0; row <= r1; ++row) {
        const TermLine *line = m_screen.lineAbsolute(row);
        const int from = (row == r0) ? c0 : 0;
        const int to = (row == r1) ? c1 : (line ? int(line->cells.size()) - 1 : 0);
        QString piece;
        if (line) {
            for (int col = from; col <= to && col < int(line->cells.size());) {
                const TermCell &cell = line->cells[size_t(col)];
                if (cell.flags & TermScreen::FlagWideCont) {
                    ++col;
                    continue;
                }
                const char32_t cp = cell.cp ? cell.cp : U' ';
                piece += QString::fromUcs4(&cp, 1);
                col += (cell.flags & TermScreen::FlagWide) ? 2 : 1;
            }
        }
        const bool soft = line && line->wrapped && row != r1;
        if (!soft) {
            while (piece.endsWith(QLatin1Char(' ')))
                piece.chop(1);
        }
        out += piece;
        if (row != r1 && !soft)
            out += QLatin1Char('\n');
    }
    return out;
}

void ScreenView::clearSelection()
{
    m_anchor = Pos();
    m_focus = Pos();
    m_selecting = false;
}

void ScreenView::nudgeSelection(int deltaAbs)
{
    if (!m_anchor.valid)
        return;
    m_anchor.abs += deltaAbs;
    m_focus.abs += deltaAbs;
    if (m_anchor.abs < 0 || m_focus.abs < 0)
        clearSelection();
}

bool ScreenView::selectionBounds(int &r0, int &c0, int &r1, int &c1) const
{
    if (!m_anchor.valid || !m_focus.valid)
        return false;
    const qint64 a = qint64(m_anchor.abs) * 100000 + m_anchor.col;
    const qint64 b = qint64(m_focus.abs) * 100000 + m_focus.col;
    if (a == b)
        return false;
    const qint64 lo = qMin(a, b);
    const qint64 hi = qMax(a, b);
    r0 = int(lo / 100000);
    c0 = int(lo % 100000);
    r1 = int(hi / 100000);
    c1 = int(hi % 100000);
    return true;
}

bool ScreenView::cellSelected(int abs, int col) const
{
    int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
    if (!selectionBounds(r0, c0, r1, c1))
        return false;
    if (abs < r0 || abs > r1)
        return false;
    if (abs == r0 && col < c0)
        return false;
    if (abs == r1 && col > c1)
        return false;
    return true;
}

ScreenView::Pos ScreenView::cellAt(QPoint pt) const
{
    Pos pos;
    if (m_cellW <= 0 || m_cellH <= 0)
        return pos;
    const int row = (pt.y() - m_pad) / m_cellH;
    const int col = (pt.x() - m_pad) / m_cellW;
    if (row < 0 || col < 0 || row >= m_screen.rows() || col >= m_screen.cols())
        return pos;
    pos.valid = true;
    pos.col = col;
    pos.abs = m_screen.altScreen() ? row : m_origin + row;
    return pos;
}

TermCell ScreenView::cellOrBlank(const TermLine *line, int col) const
{
    if (!line || col < 0 || col >= int(line->cells.size()))
        return TermCell();
    return line->cells[size_t(col)];
}

QByteArray ScreenView::arrowKey(char letter) const
{
    QByteArray seq = m_screen.appCursorKeys() ? QByteArray("\x1bO") : QByteArray("\x1b[");
    seq.append(letter);
    return seq;
}

void ScreenView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QColor bg0 = m_screen.defaultBackground();
    painter.fillRect(rect(), bg0);
    painter.setFont(m_font);

    const int rows = m_screen.rows();
    const int cols = m_screen.cols();
    const bool alt = m_screen.altScreen();
    const int origin = alt ? 0 : m_origin;
    QList<KeywordHit> kwHits; // 当前行的关键词命中，逐行重算

    auto colorsOf = [&](const TermCell &cell, int rowAbs, int column, QColor &fg, QColor &bg) {
        fg = m_screen.foreground(cell);
        bg = m_screen.background(cell);
        if (cell.attr & TermScreen::AttrInverse)
            std::swap(fg, bg);
        // 关键词高亮只改文字颜色；选中 / 查找高亮是主动状态，优先级更高
        for (const KeywordHit &hit : kwHits) {
            if (column >= hit.start && column <= hit.end) {
                fg = hit.color;
                break;
            }
        }
        if (cellSelected(rowAbs, column)) {
            bg = m_screen.selectionBackground();
            fg = m_screen.defaultForeground();
        }
        if (m_hitAbs == rowAbs && column >= m_hitCol && column <= m_hitEnd) {
            bg = m_screen.isDark() ? QColor(0x8A, 0x6A, 0x00) : QColor(0xF5, 0xD7, 0x6E);
            fg = m_screen.isDark() ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x1E, 0x1E, 0x1E);
        }
    };

    for (int row = 0; row < rows; ++row) {
        const int abs = alt ? row : origin + row;
        const TermLine *line = m_screen.lineAbsolute(abs);
        if (!m_keywords.isEmpty())
            collectKeywordHits(line, cols, &kwHits);
        else if (!kwHits.isEmpty())
            kwHits.clear();
        const int y = m_pad + row * m_cellH;
        int col = 0;
        while (col < cols) {
            const TermCell first = cellOrBlank(line, col);
            if (first.flags & TermScreen::FlagWideCont) {
                ++col;
                continue;
            }
            const int span = (first.flags & TermScreen::FlagWide) ? 2 : 1;
            QColor fg;
            QColor bg;
            colorsOf(first, abs, col, fg, bg);
            const bool styled = span == 2 || (first.attr & (TermScreen::AttrBold | TermScreen::AttrUnderline));

            int end = col + span;
            QString text;
            if (!styled) {
                while (end < cols) {
                    const TermCell next = cellOrBlank(line, end);
                    if (next.flags & (TermScreen::FlagWide | TermScreen::FlagWideCont))
                        break;
                    if (next.attr & (TermScreen::AttrBold | TermScreen::AttrUnderline))
                        break;
                    QColor nfg;
                    QColor nbg;
                    colorsOf(next, abs, end, nfg, nbg);
                    if (nfg.rgb() != fg.rgb() || nbg.rgb() != bg.rgb())
                        break;
                    ++end;
                }
            }
            for (int c = col; c < end;) {
                const TermCell cell = cellOrBlank(line, c);
                if (cell.flags & TermScreen::FlagWideCont) {
                    ++c;
                    continue;
                }
                const char32_t cp = cell.cp ? cell.cp : U' ';
                text += QString::fromUcs4(&cp, 1);
                c += (cell.flags & TermScreen::FlagWide) ? 2 : 1;
            }

            const int x = m_pad + col * m_cellW;
            const int w = (end - col) * m_cellW;
            if (bg.rgb() != bg0.rgb())
                painter.fillRect(x, y, w, m_cellH, bg);
            bool blank = true;
            for (const QChar ch : text) {
                if (ch != QLatin1Char(' ')) {
                    blank = false;
                    break;
                }
            }
            if (!blank) {
                QFont font = (first.attr & TermScreen::AttrBold) ? m_boldFont : m_font;
                font.setUnderline(first.attr & TermScreen::AttrUnderline);
                painter.setFont(font);
                painter.setPen(fg);
                painter.drawText(x, y + m_ascent, text);
            }
            col = end;
        }
    }
    paintCursor(painter);
    if (m_bell) {
        painter.setPen(QPen(QColor(0xE5, 0xC0, 0x07), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

void ScreenView::paintCursor(QPainter &painter)
{
    if (m_screen.cursorHidden())
        return;
    const bool alt = m_screen.altScreen();
    const int abs = alt ? m_screen.cursorRow() : m_screen.historySize() + m_screen.cursorRow();
    const int origin = alt ? 0 : m_origin;
    const int viewRow = abs - origin;
    if (viewRow < 0 || viewRow >= m_screen.rows())
        return;
    const int col = m_screen.cursorCol();
    const TermLine *line = m_screen.lineAbsolute(abs);
    const TermCell cell = cellOrBlank(line, col);
    const int span = (cell.flags & TermScreen::FlagWide) ? 2 : 1;
    const int x = m_pad + col * m_cellW;
    const int y = m_pad + viewRow * m_cellH;
    if (!hasFocus()) {
        painter.setPen(m_screen.defaultForeground());
        painter.drawRect(x, y, span * m_cellW - 1, m_cellH - 1);
        return;
    }
    QColor fg = m_screen.foreground(cell);
    QColor bg = m_screen.background(cell);
    if (cell.attr & TermScreen::AttrInverse)
        std::swap(fg, bg);
    painter.fillRect(x, y, span * m_cellW, m_cellH, fg);
    if (cell.cp && cell.cp != U' ') {
        painter.setPen(bg);
        painter.setFont((cell.attr & TermScreen::AttrBold) ? m_boldFont : m_font);
        const char32_t cp = cell.cp;
        painter.drawText(x, y + m_ascent, QString::fromUcs4(&cp, 1));
    }
}

void ScreenView::resizeEvent(QResizeEvent *)
{
    updateGrid();
    placeFind();
}

void ScreenView::showEvent(QShowEvent *)
{
    updateGrid();
}

void ScreenView::echoLocal(const QByteArray &bytes)
{
    // 行编辑没在管屏幕（备用屏 / 远端画屏）时远端自己会画，本地代画只会
    // 在 vi/top 的画面上留下残影
    if (!localLineMode() || bytes.isEmpty() || bytes.contains('\x1b'))
        return;
    QByteArray shown;
    shown.reserve(bytes.size() * 3);
    for (unsigned char b : bytes) {
        if (b == '\n' || b == '\r')
            shown += "\r\n";
        else if (b == 0x7f || b == '\b')
            shown += "\b \b";
        else if (b == '\t' || b >= 0x20)
            shown += char(b);
    }
    if (!shown.isEmpty())
        receiveData(shown);
}

void ScreenView::showOutbound(const QByteArray &bytes)
{
    echoLocal(bytes);
}

void ScreenView::emitInput(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    emit sendData(bytes);
    echoLocal(bytes);
}

bool ScreenView::localLineMode() const
{
    return m_localEcho && !m_screen.altScreen() && !m_remoteRaw;
}

// ---------------------------------------------------------------------------
// 远端画屏检测。串口行编辑默认替设备代画代编辑；但 BusyBox top、
// menuconfig 这类全屏程序不切备用屏（BusyBox top 每轮只发 ESC[H ESC[J），
// 行编辑会把它们的按键全部扣到回车为止，vi 备用屏之外的一切也同理。
// 所以远端一旦出现光标定位/整屏擦除，就判定「屏幕归远端管」：
// 丢弃未发出的编辑草稿，按键、回显、粘贴全部直通。
// 退出判定要保守，三样缺一不可：整块无画屏序列、写了可见字符且换过行、
// 光标停在行中（提示符形状，如 "root@board:~# "）。必须见到换行是关键——
// shell 提示符一定出现在新行上，而 vi 插入模式的逐字回显（纯文本、无控制
// 序列、行中光标）形状几乎相同，只差没有换行；少这条判据会让 vi 里打一个
// 字就退出直通，方向键被劫持成本地翻历史。误判的代价只是暂时像普通终端
// 一样靠设备回显，不会丢键。
void ScreenView::applyRemoteCtl()
{
    if (m_screen.screenCtlSeen()) {
        if (!m_remoteRaw && localLineMode())
            enterRemoteRaw();
        return;
    }
    if (m_remoteRaw && !m_screen.altScreen() && m_screen.feedPrintable() > 0
        && m_screen.feedNewlines() > 0 && m_screen.cursorCol() > 0) {
        exitRemoteRaw();
    }
}

void ScreenView::enterRemoteRaw()
{
    m_remoteRaw = true;
    // 画屏程序已把屏幕整个重画，本地编辑行没了落脚处；草稿丢弃
    // （此时屏幕内容也被远端替换，留着也看不见）。
    if (m_editOn) {
        m_editOn = false;
        m_edit.clear();
        m_editPos = 0;
    }
    m_histBrowse = -1;
    m_histDraft.clear();
    resetCompletionCycle();
}

void ScreenView::exitRemoteRaw()
{
    // 回到提示符；下一个键入从当前光标处重新起一行编辑（beginLocalEdit）
    m_remoteRaw = false;
}

static int stepCursor(const QString &text, int pos, int dir)
{
    if (dir < 0) {
        if (pos <= 0)
            return 0;
        int prev = pos - 1;
        if (prev > 0 && text.at(prev).isLowSurrogate())
            --prev;
        return prev;
    }
    if (pos >= text.size())
        return text.size();
    int next = pos + 1;
    if (text.at(pos).isHighSurrogate() && next < text.size())
        ++next;
    return next;
}

static int displayCols(const QString &text)
{
    int cols = 0;
    const QList<uint> cps = text.toUcs4();
    for (uint cp : cps) {
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
            continue;
        cols += (cp >= 0x1100) ? 2 : 1;
    }
    return cols;
}

void ScreenView::beginLocalEdit()
{
    if (m_editOn)
        return;
    m_editOn = true;
    m_editAbs = m_screen.historySize() + m_screen.cursorRow();
    m_editCol = m_screen.cursorCol();
    m_edit.clear();
    m_editPos = 0;
}

void ScreenView::redrawLocalEdit()
{
    if (!m_editOn || m_editRedraw)
        return;
    const int screenRow = m_editAbs - m_screen.historySize();
    if (screenRow < 0 || screenRow >= m_screen.rows())
        return;
    const int prefix = displayCols(m_edit.left(m_editPos));
    const int cursorCol = qBound(1, m_editCol + prefix + 1, m_screen.cols());
    QByteArray seq;
    seq += "\x1b[" + QByteArray::number(screenRow + 1) + ";"
           + QByteArray::number(m_editCol + 1) + "H";
    seq += m_edit.toUtf8();
    seq += "\x1b[K";
    seq += "\x1b[" + QByteArray::number(screenRow + 1) + ";"
           + QByteArray::number(cursorCol) + "H";
    m_editRedraw = true;
    receiveData(seq);
    m_editRedraw = false;
}

void ScreenView::localInsert(const QString &text)
{
    if (text.isEmpty())
        return;
    beginLocalEdit();
    m_edit.insert(m_editPos, text);
    m_editPos += text.size();
    resetCompletionCycle();
    redrawLocalEdit();
}

void ScreenView::rememberSentLine(const QString &line)
{
    if (line.isEmpty())
        return;
    if (!m_sentHist.isEmpty() && m_sentHist.last() == line)
        return;
    m_sentHist.append(line);
    if (m_sentHist.size() > 200)
        m_sentHist.removeFirst();
    m_histBrowse = -1;
}

void ScreenView::recallSent(int direction)
{
    if (m_sentHist.isEmpty())
        return;
    beginLocalEdit();
    if (direction < 0) {
        if (m_histBrowse < 0) {
            m_histDraft = m_edit;
            m_histBrowse = m_sentHist.size() - 1;
        } else if (m_histBrowse > 0) {
            --m_histBrowse;
        }
        m_edit = m_sentHist.at(m_histBrowse);
    } else {
        if (m_histBrowse < 0)
            return;
        if (m_histBrowse + 1 >= m_sentHist.size()) {
            m_histBrowse = -1;
            m_edit = m_histDraft;
        } else {
            ++m_histBrowse;
            m_edit = m_sentHist.at(m_histBrowse);
        }
    }
    m_editPos = m_edit.size();
    resetCompletionCycle();
    redrawLocalEdit();
}

void ScreenView::clearLocalDisplay()
{
    m_editOn = false;
    m_remoteRaw = false; // 手动清屏后按提示符处理，恢复行编辑
    m_edit.clear();
    m_editPos = 0;
    m_histBrowse = -1;
    m_histDraft.clear();
    if (m_pump->isActive())
        m_pump->stop();
    m_rx.clear();
    m_rxUsed = 0;
    clearSelection();
    m_hitAbs = -1;
    afterOutput(m_screen.clearDisplay());
}

void ScreenView::commitLocalLine()
{
    const QString line = m_edit;
    m_edit.clear();
    m_editPos = 0;
    m_editOn = false;
    resetCompletionCycle();
    if (line.trimmed() == QLatin1String("clear -a")) {
        rememberSentLine(line.trimmed());
        clearLocalDisplay();
        return;
    }
    rememberSentLine(line);
    const QByteArray payload = line.toUtf8() + '\n';
    emit sendData(payload);
    receiveData(QByteArray("\r\n"));
}

void ScreenView::localTab()
{
    if (!m_completion)
        return;
    beginLocalEdit();
    int start = m_editPos;
    while (start > 0 && !m_edit.at(start - 1).isSpace())
        --start;
    if (m_cycle.isEmpty() || start != m_cycleStart) {
        const QString prefix = m_edit.mid(start, m_editPos - start);
        m_cycle = m_completion->complete(prefix);
        m_cycleStart = start;
        m_cycleOriginal = prefix;
        m_cycleState = -1;
        m_cycleEnd = m_editPos;
        if (m_cycle.isEmpty())
            return;
    }
    int nextState = 0;
    if (m_cycleState >= 0)
        nextState = (m_cycleState + 1 >= m_cycle.size()) ? -1 : m_cycleState + 1;
    const QString target = (nextState < 0) ? m_cycleOriginal : m_cycle.at(nextState);
    m_edit.replace(start, m_editPos - start, target);
    m_editPos = start + target.size();
    m_cycleEnd = m_editPos;
    m_cycleState = nextState;
    redrawLocalEdit();
}

bool ScreenView::handleLocalLineKey(QKeyEvent *event)
{
    if (!localLineMode())
        return false;
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (mods & (Qt::ControlModifier | Qt::AltModifier))
        return false;

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        commitLocalLine();
        event->accept();
        return true;
    case Qt::Key_Backspace:
        if (m_editOn && m_editPos > 0) {
            const int prev = stepCursor(m_edit, m_editPos, -1);
            m_edit.remove(prev, m_editPos - prev);
            m_editPos = prev;
            resetCompletionCycle();
            redrawLocalEdit();
        }
        event->accept();
        return true;
    case Qt::Key_Delete:
        if (m_editOn && m_editPos < m_edit.size()) {
            const int next = stepCursor(m_edit, m_editPos, +1);
            m_edit.remove(m_editPos, next - m_editPos);
            resetCompletionCycle();
            redrawLocalEdit();
        }
        event->accept();
        return true;
    case Qt::Key_Left:
        if (m_editOn)
            m_editPos = stepCursor(m_edit, m_editPos, -1);
        redrawLocalEdit();
        event->accept();
        return true;
    case Qt::Key_Right:
        if (m_editOn)
            m_editPos = stepCursor(m_edit, m_editPos, +1);
        redrawLocalEdit();
        event->accept();
        return true;
    case Qt::Key_Home:
        m_editPos = 0;
        redrawLocalEdit();
        event->accept();
        return true;
    case Qt::Key_End:
        m_editPos = m_edit.size();
        redrawLocalEdit();
        event->accept();
        return true;
    case Qt::Key_Up:
        recallSent(-1);
        event->accept();
        return true;
    case Qt::Key_Down:
        recallSent(+1);
        event->accept();
        return true;
    case Qt::Key_Tab:
        localTab();
        event->accept();
        return true;
    default:
        break;
    }
    if (!event->text().isEmpty() && event->text().at(0).unicode() >= 0x20) {
        localInsert(event->text());
        event->accept();
        return true;
    }
    return false;
}

void ScreenView::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const int key = event->key();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    const bool shift = mods & Qt::ShiftModifier;

    // Tab 之外任何键都打断补全 Cycle（继续打字、回车、方向键……）
    if (key != Qt::Key_Tab)
        resetCompletionCycle();

    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt
        || key == Qt::Key_Meta) {
        event->accept();
        return;
    }

    if (ctrl && shift && key == Qt::Key_F) {
        openFind();
        event->accept();
        return;
    }
    // Ctrl+F 打开/聚焦查找（与浏览器等一致）。全屏程序（备用屏/远端画屏）
    // 期间它是程序自己的按键——vi 的下翻页——不抢，走下面的直通分支。
    if (ctrl && !shift && !alt && key == Qt::Key_F
        && !m_screen.altScreen() && !m_remoteRaw) {
        if (m_find->isVisible()) {
            m_find->setFocus();
            m_find->selectAll();
        } else {
            openFind();
        }
        event->accept();
        return;
    }
    if (m_find && m_find->isVisible() && key == Qt::Key_F3) {
        runFind(!shift);
        event->accept();
        return;
    }

    if (ctrl && shift && key == Qt::Key_C) {
        copySelection();
        event->accept();
        return;
    }
    if (ctrl && shift && (key == Qt::Key_V || key == Qt::Key_Insert)) {
        pasteClipboard();
        event->accept();
        return;
    }

    if (shift && key == Qt::Key_PageUp) {
        scrollLocal(-(m_screen.rows() - 1));
        event->accept();
        return;
    }
    if (shift && key == Qt::Key_PageDown) {
        scrollLocal(m_screen.rows() - 1);
        event->accept();
        return;
    }

    if (ctrl && !alt) {
        const bool selected = m_anchor.valid && m_focus.valid
                              && (m_anchor.abs != m_focus.abs || m_anchor.col != m_focus.col);
        if (key == Qt::Key_C && selected) {
            copySelection();
            event->accept();
            return;
        }
        if (key == Qt::Key_V || key == Qt::Key_Y) {
            pasteClipboard();
            event->accept();
            return;
        }
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            emitInput(QByteArray(1, char(1 + key - Qt::Key_A)));
            event->accept();
            return;
        }
        if (key == Qt::Key_Space) {
            emitInput(QByteArray(1, '\0'));
            event->accept();
            return;
        }
    }

    if (shift && key == Qt::Key_Insert) {
        pasteClipboard();
        event->accept();
        return;
    }

    if (handleLocalLineKey(event))
        return;

    QByteArray seq;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        seq = QByteArray(1, '\r');
        break;
    case Qt::Key_Backspace:
        seq = QByteArray(1, '\x7f');
        break;
    case Qt::Key_Tab:
        if (m_completion && !m_screen.altScreen() && !m_remoteRaw && !ctrl && !alt) {
            handleTabCompletion();
            event->accept();
            return;
        }
        seq = QByteArray(1, '\t');
        break;
    case Qt::Key_Backtab:
        seq = QByteArray("\x1b[Z");
        break;
    case Qt::Key_Escape:
        seq = QByteArray(1, '\x1b');
        break;
    case Qt::Key_Up:
        seq = arrowKey('A');
        break;
    case Qt::Key_Down:
        seq = arrowKey('B');
        break;
    case Qt::Key_Right:
        seq = arrowKey('C');
        break;
    case Qt::Key_Left:
        seq = arrowKey('D');
        break;
    case Qt::Key_Home:
        seq = arrowKey('H');
        break;
    case Qt::Key_End:
        seq = arrowKey('F');
        break;
    case Qt::Key_Delete:
        seq = QByteArray("\x1b[3~");
        break;
    case Qt::Key_Insert:
        seq = QByteArray("\x1b[2~");
        break;
    case Qt::Key_PageUp:
        seq = QByteArray("\x1b[5~");
        break;
    case Qt::Key_PageDown:
        seq = QByteArray("\x1b[6~");
        break;
    case Qt::Key_F1:
        seq = QByteArray("\x1bOP");
        break;
    case Qt::Key_F2:
        seq = QByteArray("\x1bOQ");
        break;
    case Qt::Key_F3:
        seq = QByteArray("\x1bOR");
        break;
    case Qt::Key_F4:
        seq = QByteArray("\x1bOS");
        break;
    case Qt::Key_F5:
        seq = QByteArray("\x1b[15~");
        break;
    case Qt::Key_F6:
        seq = QByteArray("\x1b[17~");
        break;
    case Qt::Key_F7:
        seq = QByteArray("\x1b[18~");
        break;
    case Qt::Key_F8:
        seq = QByteArray("\x1b[19~");
        break;
    case Qt::Key_F9:
        seq = QByteArray("\x1b[20~");
        break;
    case Qt::Key_F10:
        seq = QByteArray("\x1b[21~");
        break;
    case Qt::Key_F11:
        seq = QByteArray("\x1b[23~");
        break;
    case Qt::Key_F12:
        seq = QByteArray("\x1b[24~");
        break;
    default:
        break;
    }
    if (!seq.isEmpty()) {
        emitInput(seq);
        event->accept();
        return;
    }

    if (alt && !ctrl && !event->text().isEmpty()) {
        QByteArray bytes(1, '\x1b');
        bytes += event->text().toUtf8();
        emitInput(bytes);
        event->accept();
        return;
    }
    if (!ctrl && !event->text().isEmpty()) {
        emitInput(event->text().toUtf8());
        event->accept();
        return;
    }
    event->ignore();
}

// ---------------------------------------------------------------- Tab 补全
//
// 嵌入式设备的 shell（BusyBox ash 等）常没编 Tab 补全，按下 Tab 只会响一声
// 或没动静，所以补全在客户端做：读主屏光标所在的输入行，取光标前的词去匹配，
// 把差量字符发给设备，由设备回显落到屏幕上。客户端不代画：设备回显开着时
// 不会出现双份，关着时（打字本就不可见）行为也自洽。备用屏里跑的是全屏
// 程序，Tab 是它的键，一律直通。

void ScreenView::setCompletionEngine(CompletionEngine *engine)
{
    m_completion = engine;
}

static QString lineToPlain(const TermLine *line)
{
    QString text;
    if (!line)
        return text;
    for (const TermCell &cell : line->cells) {
        if (cell.flags & TermScreen::FlagWideCont)
            continue;
        char32_t cp = cell.cp ? cell.cp : U' ';
        text.append(QString::fromUcs4(&cp, 1));
    }
    while (text.size() > 1 && text.endsWith(QLatin1Char(' ')))
        text.chop(1);
    return text;
}

QString ScreenView::screenText() const
{
    // 主屏视口从滚动历史之后算起，备用屏行号就是屏幕行
    const int base = m_screen.altScreen() ? 0 : m_screen.historySize();
    QStringList lines;
    for (int r = 0; r < m_screen.rows(); ++r)
        lines << lineToPlain(m_screen.lineAbsolute(base + r));
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

QString ScreenView::recentText(int maxLines) const
{
    maxLines = qBound(1, maxLines, 400);
    const bool alt = m_screen.altScreen();
    const int total = alt ? m_screen.rows() : m_screen.historySize() + m_screen.rows();
    const int begin = qMax(0, total - maxLines);
    QStringList lines;
    for (int abs = begin; abs < total; ++abs)
        lines << lineToPlain(m_screen.lineAbsolute(abs));
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

void ScreenView::resetCompletionCycle()
{
    m_cycle.clear();
    m_cycleStart = -1;
    m_cycleEnd = -1;
    m_cyclePending = 0;
    m_cycleState = -1;
    m_cycleOriginal.clear();
}

QString ScreenView::inputLineText(int *cursorColOut) const
{
    if (m_screen.altScreen())
        return QString();
    const TermLine *line = m_screen.lineAbsolute(m_screen.historySize() + m_screen.cursorRow());
    if (!line)
        return QString();
    const int col = m_screen.cursorCol();
    QString text;
    text.reserve(int(line->cells.size()));
    for (const TermCell &cell : line->cells) {
        if (cell.flags & TermScreen::FlagWideCont)
            continue; // 宽字符的右半格，不算独立字符
        if (text.size() >= col)
            break; // 光标列之后的内容不属于输入前缀
        char32_t cp = cell.cp;
        text.append(QString::fromUcs4(&cp, 1));
    }
    if (cursorColOut)
        *cursorColOut = qMin(col, text.size());
    return text;
}

void ScreenView::handleTabCompletion()
{
    if (!m_completion || m_screen.altScreen() || m_remoteRaw) {
        emitInput(QByteArray(1, '\t'));
        return;
    }

    // 先把攒着的设备输出落地，不然补全尾巴的回显还堵在缓冲里，取到的行是旧的
    flushRx();

    int cursorCol = 0;
    const QString lineText = inputLineText(&cursorCol);
    int wordStart = cursorCol;
    while (wordStart > 0 && !lineText.at(wordStart - 1).isSpace())
        --wordStart;

    // Cycle 中词首没变、词尾落在 [设备侧词尾 - 待回显量, 设备侧词尾] 内 →
    // 算边界没变，推进下一项。串口/网络有延迟，屏上词尾可能还差 m_cyclePending
    // 个字符没回显到；设备侧行缓冲是完整的，替换时 DEL 会一次删够。
    if (!m_cycle.isEmpty() && wordStart == m_cycleStart
        && cursorCol <= m_cycleEnd && cursorCol >= m_cycleEnd - m_cyclePending) {
        advanceCompletionCycle();
        return;
    }

    // 全新补全
    const QString prefix = lineText.mid(wordStart, cursorCol - wordStart);
    m_cycle = m_completion->complete(prefix);
    m_cycleStart = wordStart;
    m_cycleOriginal = prefix;
    m_cycleState = -1;
    m_cyclePending = 0;

    if (m_cycle.isEmpty()) {
        m_cycleEnd = cursorCol;
        // 客户端没有候选时把 Tab 直通给设备，别把服务端补全也一并拦了
        emitInput(QByteArray(1, '\t'));
        return;
    }
    if (m_cycle.size() == 1) {
        // 唯一匹配：直接补齐（一次到位，不进 Cycle）
        const QString &only = m_cycle.first();
        const QString tail = only.mid(prefix.size());
        if (!tail.isEmpty()) {
            m_cyclePending = m_localEcho ? 0 : tail.size();
            emitInput(tail.toUtf8());
        }
        m_cycleEnd = wordStart + only.size();
        m_cycle.clear();
        return;
    }

    // 多匹配：先补到最长公共前缀
    const QString lcp = CompletionEngine::longestCommonPrefix(m_cycle);
    if (lcp.size() > prefix.size()) {
        const QString tail = lcp.mid(prefix.size());
        m_cyclePending = m_localEcho ? 0 : tail.size();
        emitInput(tail.toUtf8());
        m_cycleEnd = wordStart + lcp.size();
        m_cycleOriginal = lcp; // LCP 成为新的回绕基准
        return;
    }
    // LCP == prefix，立即推进到 cycle[0]
    m_cycleEnd = wordStart + prefix.size();
    advanceCompletionCycle();
}

void ScreenView::advanceCompletionCycle()
{
    const int n = m_cycle.size();
    if (n == 0)
        return;

    // state: -1 (原始) → 0 (cycle[0]) → 1 → ... → n-1 → 回到 -1 (原始)
    int nextState;
    if (m_cycleState < 0)
        nextState = 0;
    else if (m_cycleState + 1 >= n)
        nextState = -1;
    else
        nextState = m_cycleState + 1;

    const QString target = (nextState < 0) ? m_cycleOriginal : m_cycle.at(nextState);
    // 设备侧行缓冲长度含待回显部分，按它整体替换：DEL 删够旧词，再发新词。
    // 回显流在设备输出队列里是有序的，屏上中间态可能乱一阵，终态必正确。
    const int oldLen = m_cycleEnd - m_cycleStart;
    const int seenOld = qMax(0, oldLen - m_cyclePending); // 屏上已见旧词长
    if (oldLen > 0)
        emitInput(QByteArray(oldLen, m_localEcho ? '\b' : '\x7f'));
    if (!target.isEmpty())
        emitInput(target.toUtf8());
    m_cycleEnd = m_cycleStart + target.size();
    m_cyclePending = m_localEcho ? 0 : qAbs(target.size() - seenOld);
    m_cycleState = nextState;
}

void ScreenView::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    if (event->button() == Qt::LeftButton && m_lastDoubleMs > 0
        && m_clicks.elapsed() - m_lastDoubleMs < 500) {
        const Pos pos = cellAt(event->position().toPoint());
        m_lastDoubleMs = 0;
        if (pos.valid)
            selectLine(pos.abs);
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        // 有选区时给菜单（高亮/复制/粘贴），没选区保持右键直接粘贴的快路径
        QString word = selectedText().trimmed();
        const int nl = word.indexOf(QLatin1Char('\n'));
        if (nl >= 0)
            word = word.left(nl).trimmed(); // 多行选区只用第一行当关键词
        if (word.isEmpty()) {
            pasteClipboard();
        } else {
            QMenu menu(this);
            menu.addAction(tr("高亮「%1」").arg(word), this, [this, word] {
                emit highlightRequested(word);
            });
            menu.addAction(tr("复制"), this, [this] { copySelection(); });
            menu.addAction(tr("粘贴"), this, [this] { pasteClipboard(); });
            menu.exec(event->globalPosition().toPoint());
        }
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    const Pos pos = cellAt(event->position().toPoint());
    if (!pos.valid) {
        clearSelection();
        update();
        return;
    }
    m_selecting = true;
    m_anchor = pos;
    m_focus = pos;
    update();
}

void ScreenView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_selecting || !(event->buttons() & Qt::LeftButton))
        return;
    const Pos pos = cellAt(event->position().toPoint());
    if (!pos.valid || (pos.abs == m_focus.abs && pos.col == m_focus.col))
        return;
    m_focus = pos;
    update();
}

void ScreenView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_selecting = false;
    if (m_anchor.valid && (m_anchor.abs != m_focus.abs || m_anchor.col != m_focus.col))
        copySelection();
}

void ScreenView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const Pos pos = cellAt(event->position().toPoint());
    if (!pos.valid)
        return;
    const TermLine *line = m_screen.lineAbsolute(pos.abs);
    if (!line || line->cells.empty())
        return;
    int left = qBound(0, pos.col, int(line->cells.size()) - 1);
    int right = left;
    if (left < int(line->cells.size()) && (line->cells[size_t(left)].flags & TermScreen::FlagWideCont)
        && left > 0)
        --left;
    if (!wordChar(line->cells[size_t(left)].cp))
        return;
    while (left > 0 && wordChar(line->cells[size_t(left - 1)].cp))
        --left;
    while (right + 1 < int(line->cells.size()) && wordChar(line->cells[size_t(right + 1)].cp))
        ++right;
    m_anchor = Pos{pos.abs, left, true};
    m_focus = Pos{pos.abs, right, true};
    m_selecting = false;
    m_lastDoubleMs = m_clicks.elapsed();
    copySelection();
    update();
}

void ScreenView::wheelEvent(QWheelEvent *event)
{
    const int dy = event->angleDelta().y();
    if (dy == 0) {
        event->ignore();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        zoom(dy > 0 ? 1 : -1);
        event->accept();
        return;
    }
    const int steps = qMax(1, qAbs(dy) / 120);
    const int dir = dy > 0 ? -1 : 1;
    // 备用屏 / 远端画屏（top 列表滚动）：滚轮当方向键交给程序
    if ((m_screen.altScreen() || m_remoteRaw)
        && !(event->modifiers() & Qt::ShiftModifier)) {
        const QByteArray seq = dir < 0 ? arrowKey('A') : arrowKey('B');
        for (int i = 0; i < steps * 3; ++i)
            emit sendData(seq);
        event->accept();
        return;
    }
    scrollLocal(dir * steps * 3);
    event->accept();
}

void ScreenView::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    update();
}

void ScreenView::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    update();
}

void ScreenView::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty()) {
        if (localLineMode())
            localInsert(event->commitString());
        else
            emitInput(event->commitString().toUtf8());
    }
    event->accept();
}

QVariant ScreenView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return true;
    if (query == Qt::ImCursorRectangle) {
        const bool alt = m_screen.altScreen();
        const int abs = alt ? m_screen.cursorRow() : m_screen.historySize() + m_screen.cursorRow();
        const int origin = alt ? 0 : m_origin;
        const int viewRow = abs - origin;
        const int col = m_screen.cursorCol();
        return QRect(m_pad + col * m_cellW, m_pad + qMax(0, viewRow) * m_cellH, m_cellW, m_cellH);
    }
    return QWidget::inputMethodQuery(query);
}

void ScreenView::placeFind()
{
    if (!m_find || m_find->isHidden() || !m_bar)
        return;
    const int w = qMin(360, qMax(160, width() / 3));
    m_find->setGeometry(width() - m_bar->width() - w - 8, 8, w, 28);
    m_find->raise();
}

void ScreenView::openFind()
{
    const QString sel = selectedText();
    if (!sel.isEmpty() && !sel.contains(QLatin1Char('\n')) && sel.size() <= 200)
        m_find->setText(sel);
    m_find->show();
    placeFind();
    m_find->setFocus();
    m_find->selectAll();
    if (!m_find->text().isEmpty())
        runFind(true);
}

void ScreenView::closeFind()
{
    m_find->hide();
    m_hitAbs = -1;
    setFocus();
    update();
}

bool ScreenView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_find && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            closeFind();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            runFind(!(key->modifiers() & Qt::ShiftModifier));
            return true;
        }
        if (key->key() == Qt::Key_F3) {
            runFind(!(key->modifiers() & Qt::ShiftModifier));
            return true;
        }
        if (key->key() == Qt::Key_F
            && key->modifiers() == Qt::ControlModifier) {
            m_find->selectAll(); // 查找框已聚焦时再按 Ctrl+F：全选关键词
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenView::selectLine(int absRow)
{
    const TermLine *line = m_screen.lineAbsolute(absRow);
    int end = 0;
    if (line) {
        for (int col = 0; col < int(line->cells.size()); ++col) {
            const char32_t cp = line->cells[size_t(col)].cp;
            if (cp && cp != U' ')
                end = col;
        }
    }
    m_anchor = Pos{absRow, 0, true};
    m_focus = Pos{absRow, end, true};
    m_selecting = false;
    if (m_anchor.col != m_focus.col)
        copySelection();
    update();
}

bool ScreenView::runFind(bool forward)
{
    const QString query = m_find ? m_find->text() : QString();
    if (query.isEmpty()) {
        m_hitAbs = -1;
        update();
        return false;
    }
    const bool alt = m_screen.altScreen();
    const int total = alt ? m_screen.rows() : m_screen.historySize() + m_screen.rows();
    if (total <= 0)
        return false;

    int row = m_hitAbs >= 0 ? m_hitAbs
                            : (alt ? m_screen.cursorRow()
                                   : m_screen.historySize() + m_screen.cursorRow());
    int fromCol = 0;
    if (m_hitAbs >= 0)
        fromCol = forward ? m_hitEnd + 1 : m_hitCol - 1;
    else if (!forward)
        fromCol = m_screen.cols();
    row = qBound(0, row, total - 1);

    auto scan = [&](int absRow, int startCol, int &hitCol, int &hitEnd) -> bool {
        const TermLine *line = m_screen.lineAbsolute(absRow);
        if (!line)
            return false;
        QString text;
        QVector<int> cols;
        text.reserve(int(line->cells.size()));
        for (int col = 0; col < int(line->cells.size());) {
            const TermCell &cell = line->cells[size_t(col)];
            if (cell.flags & TermScreen::FlagWideCont) {
                ++col;
                continue;
            }
            cols.append(col);
            const char32_t cp = cell.cp ? cell.cp : U' ';
            text += QString::fromUcs4(&cp, 1);
            col += (cell.flags & TermScreen::FlagWide) ? 2 : 1;
        }
        if (text.isEmpty() || cols.isEmpty())
            return false;
        int found = -1;
        if (forward) {
            int index = 0;
            for (int i = 0; i < cols.size(); ++i) {
                if (cols.at(i) >= startCol) {
                    index = i;
                    break;
                }
                index = i + 1;
            }
            if (index >= text.size())
                return false;
            found = text.indexOf(query, index, Qt::CaseInsensitive);
        } else {
            int limit = 0;
            for (int i = 0; i < cols.size(); ++i) {
                if (cols.at(i) > startCol)
                    break;
                limit = i + 1;
            }
            if (limit <= 0)
                return false;
            found = text.lastIndexOf(query, limit - 1, Qt::CaseInsensitive);
            if (found < 0 || found + query.size() > limit)
                return false;
        }
        if (found < 0 || found >= cols.size())
            return false;
        hitCol = cols.at(found);
        const int after = found + query.size();
        hitEnd = after < cols.size() ? cols.at(after) - 1 : m_screen.cols() - 1;
        if (hitEnd < hitCol)
            hitEnd = hitCol;
        return true;
    };

    for (int n = 0; n < total; ++n) {
        int hitCol = 0;
        int hitEnd = 0;
        if (scan(row, fromCol, hitCol, hitEnd)) {
            m_hitAbs = row;
            m_hitCol = hitCol;
            m_hitEnd = hitEnd;
            if (!alt) {
                const int max = m_screen.historySize();
                m_origin = qBound(0, row - m_screen.rows() / 3, max);
                m_follow = m_origin >= max;
                syncBar();
            }
            update();
            return true;
        }
        if (forward) {
            row = (row + 1) % total;
            fromCol = 0;
        } else {
            row = (row + total - 1) % total;
            fromCol = m_screen.cols();
        }
    }
    return false;
}
