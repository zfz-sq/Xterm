#include "sshview.h"

#include <QClipboard>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
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

SshView::SshView(QWidget *parent)
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
    connect(m_sizeTimer, &QTimer::timeout, this, &SshView::emitPtySize);

    m_pump = new QTimer(this);
    m_pump->setSingleShot(true);
    connect(m_pump, &QTimer::timeout, this, &SshView::pumpRx);

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
    m_find->setPlaceholderText(QStringLiteral("查找    Enter 下一个   Shift+Enter 上一个   Esc 关闭"));
    m_find->hide();
    m_find->installEventFilter(this);
}

void SshView::applyTheme(bool dark)
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

void SshView::receiveData(const QByteArray &data)
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

void SshView::appendSystemMessage(const QString &text)
{
    flushRx();
    afterOutput(m_screen.appendNote(text));
}

void SshView::pumpRx()
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
    afterOutput(m_screen.feed(slice));
    if (m_rxUsed < m_rx.size())
        m_pump->start(0);
}

void SshView::flushRx()
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
    afterOutput(m_screen.feed(rest));
}

void SshView::recalcMetrics()
{
    const QFontMetrics fm(m_font);
    m_cellW = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellH = qMax(1, fm.lineSpacing());
    m_ascent = fm.ascent();
}

void SshView::updateGrid()
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

void SshView::emitPtySize()
{
    const int cols = m_screen.cols();
    const int rows = m_screen.rows();
    if (cols == m_sentCols && rows == m_sentRows)
        return;
    m_sentCols = cols;
    m_sentRows = rows;
    emit terminalSizeChanged(cols, rows);
}

void SshView::afterOutput(int dropped)
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
    if (m_screen.takeBell()) {
        m_bell = true;
        m_bellTimer->start(140);
    }
    // 同步更新期间先攒着，帧结束或超时再画，避免 htop 闪半帧。
    if (m_screen.syncing()) {
        if (!m_syncHold->isActive())
            m_syncHold->start(80);
        return;
    }
    if (m_syncHold->isActive())
        m_syncHold->stop();
    syncBar();
    update();
}

void SshView::syncBar()
{
    const int max = m_screen.altScreen() ? 0 : m_screen.historySize();
    m_barLock = true;
    m_bar->setRange(0, max);
    m_bar->setPageStep(qMax(1, m_screen.rows()));
    m_bar->setSingleStep(1);
    m_bar->setValue(qBound(0, m_origin, max));
    m_barLock = false;
}

void SshView::scrollLocal(int deltaRows)
{
    if (m_screen.altScreen())
        return;
    const int max = m_screen.historySize();
    m_origin = qBound(0, m_origin + deltaRows, max);
    m_follow = m_origin >= max;
    syncBar();
    update();
}

void SshView::zoom(int steps)
{
    const int next = qBound(8, m_font.pointSize() + steps, 36);
    if (next == m_font.pointSize())
        return;
    m_font.setPointSize(next);
    m_boldFont = m_font;
    m_boldFont.setBold(true);
    setFont(m_font);
    recalcMetrics();
    updateGrid();
    update();
}

void SshView::pasteClipboard()
{
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    if (text.size() > 1000000)
        text.truncate(1000000);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    QByteArray bytes = text.toUtf8();
    bytes.replace('\0', "");
    if (bytes.isEmpty())
        return;
    if (m_screen.bracketedPaste())
        bytes = QByteArray("\x1b[200~") + bytes + QByteArray("\x1b[201~");
    emit sendData(bytes);
}

void SshView::copySelection()
{
    const QString out = selectedText();
    if (!out.isEmpty())
        QGuiApplication::clipboard()->setText(out);
}

QString SshView::selectedText() const
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

void SshView::clearSelection()
{
    m_anchor = Pos();
    m_focus = Pos();
    m_selecting = false;
}

void SshView::nudgeSelection(int deltaAbs)
{
    if (!m_anchor.valid)
        return;
    m_anchor.abs += deltaAbs;
    m_focus.abs += deltaAbs;
    if (m_anchor.abs < 0 || m_focus.abs < 0)
        clearSelection();
}

bool SshView::selectionBounds(int &r0, int &c0, int &r1, int &c1) const
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

bool SshView::cellSelected(int abs, int col) const
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

SshView::Pos SshView::cellAt(QPoint pt) const
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

TermCell SshView::cellOrBlank(const TermLine *line, int col) const
{
    if (!line || col < 0 || col >= int(line->cells.size()))
        return TermCell();
    return line->cells[size_t(col)];
}

QByteArray SshView::arrowKey(char letter) const
{
    QByteArray seq = m_screen.appCursorKeys() ? QByteArray("\x1bO") : QByteArray("\x1b[");
    seq.append(letter);
    return seq;
}

void SshView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const QColor bg0 = m_screen.defaultBackground();
    painter.fillRect(rect(), bg0);
    painter.setFont(m_font);

    const int rows = m_screen.rows();
    const int cols = m_screen.cols();
    const bool alt = m_screen.altScreen();
    const int origin = alt ? 0 : m_origin;

    auto colorsOf = [&](const TermCell &cell, int rowAbs, int column, QColor &fg, QColor &bg) {
        fg = m_screen.foreground(cell);
        bg = m_screen.background(cell);
        if (cell.attr & TermScreen::AttrInverse)
            std::swap(fg, bg);
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

void SshView::paintCursor(QPainter &painter)
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

void SshView::resizeEvent(QResizeEvent *)
{
    updateGrid();
    placeFind();
}

void SshView::showEvent(QShowEvent *)
{
    updateGrid();
}

void SshView::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const int key = event->key();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    const bool shift = mods & Qt::ShiftModifier;

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
            emit sendData(QByteArray(1, char(1 + key - Qt::Key_A)));
            event->accept();
            return;
        }
        if (key == Qt::Key_Space) {
            emit sendData(QByteArray(1, '\0'));
            event->accept();
            return;
        }
    }

    if (shift && key == Qt::Key_Insert) {
        pasteClipboard();
        event->accept();
        return;
    }

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
        emit sendData(seq);
        event->accept();
        return;
    }

    if (alt && !ctrl && !event->text().isEmpty()) {
        QByteArray bytes(1, '\x1b');
        bytes += event->text().toUtf8();
        emit sendData(bytes);
        event->accept();
        return;
    }
    if (!ctrl && !event->text().isEmpty()) {
        emit sendData(event->text().toUtf8());
        event->accept();
        return;
    }
    event->ignore();
}

void SshView::mousePressEvent(QMouseEvent *event)
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
        pasteClipboard();
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

void SshView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_selecting || !(event->buttons() & Qt::LeftButton))
        return;
    const Pos pos = cellAt(event->position().toPoint());
    if (!pos.valid || (pos.abs == m_focus.abs && pos.col == m_focus.col))
        return;
    m_focus = pos;
    update();
}

void SshView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_selecting = false;
    if (m_anchor.valid && (m_anchor.abs != m_focus.abs || m_anchor.col != m_focus.col))
        copySelection();
}

void SshView::mouseDoubleClickEvent(QMouseEvent *event)
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

void SshView::wheelEvent(QWheelEvent *event)
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
    if (m_screen.altScreen() && !(event->modifiers() & Qt::ShiftModifier)) {
        const QByteArray seq = dir < 0 ? arrowKey('A') : arrowKey('B');
        for (int i = 0; i < steps * 3; ++i)
            emit sendData(seq);
        event->accept();
        return;
    }
    scrollLocal(dir * steps * 3);
    event->accept();
}

void SshView::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    update();
}

void SshView::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    update();
}

void SshView::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty())
        emit sendData(event->commitString().toUtf8());
    event->accept();
}

QVariant SshView::inputMethodQuery(Qt::InputMethodQuery query) const
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

void SshView::placeFind()
{
    if (!m_find || m_find->isHidden() || !m_bar)
        return;
    const int w = qMin(360, qMax(160, width() / 3));
    m_find->setGeometry(width() - m_bar->width() - w - 8, 8, w, 28);
    m_find->raise();
}

void SshView::openFind()
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

void SshView::closeFind()
{
    m_find->hide();
    m_hitAbs = -1;
    setFocus();
    update();
}

bool SshView::eventFilter(QObject *watched, QEvent *event)
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
    }
    return QWidget::eventFilter(watched, event);
}

void SshView::selectLine(int absRow)
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

bool SshView::runFind(bool forward)
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
