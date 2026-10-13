#include "termscreen.h"

#include <QRegularExpression>
#include <QVector>
#include <QtGlobal>

#include <utility>

namespace {

struct Palette
{
    QColor normal[8];
    QColor bright[8];
    QColor fg;
    QColor bg;
    QColor selection;
    QColor info;
};

const Palette kDark = {
    {QColor(0x4E, 0x4E, 0x4E), QColor(0xCD, 0x31, 0x31), QColor(0x0D, 0xBC, 0x79),
     QColor(0xE5, 0xE5, 0x11), QColor(0x24, 0x72, 0xC8), QColor(0xBC, 0x3F, 0xBC),
     QColor(0x11, 0xA8, 0xCD), QColor(0xE5, 0xE5, 0xE5)},
    {QColor(0x76, 0x76, 0x76), QColor(0xF1, 0x4C, 0x4C), QColor(0x23, 0xD1, 0x8B),
     QColor(0xF5, 0xF5, 0x43), QColor(0x3B, 0x8E, 0xEA), QColor(0xD6, 0x70, 0xD6),
     QColor(0x29, 0xB8, 0xDB), QColor(0xFF, 0xFF, 0xFF)},
    QColor(0xD4, 0xD4, 0xD4), QColor(0x1E, 0x1E, 0x1E), QColor(0x26, 0x4F, 0x78),
    QColor(0x9C, 0xDC, 0xFE)};

const Palette kLight = {
    {QColor(0x3E, 0x3E, 0x3E), QColor(0xC4, 0x2F, 0x2F), QColor(0x0E, 0x8A, 0x3E),
     QColor(0xB5, 0x89, 0x00), QColor(0x24, 0x72, 0xC8), QColor(0xB0, 0x3A, 0xB0),
     QColor(0x11, 0x8A, 0xB3), QColor(0x55, 0x55, 0x55)},
    {QColor(0x66, 0x66, 0x66), QColor(0xD9, 0x3C, 0x3C), QColor(0x1F, 0xA8, 0x55),
     QColor(0xC9, 0x9A, 0x0A), QColor(0x1F, 0x66, 0xC4), QColor(0xC5, 0x4E, 0xC5),
     QColor(0x14, 0x9C, 0xC8), QColor(0x99, 0x99, 0x99)},
    QColor(0x24, 0x29, 0x2E), QColor(0xFF, 0xFF, 0xFF), QColor(0xC8, 0xDD, 0xF5),
    QColor(0x0E, 0x63, 0x9C)};

const Palette &palette(bool dark)
{
    return dark ? kDark : kLight;
}

int glyphCols(char32_t cp)
{
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 0;
    if (cp >= 0x300 && cp <= 0x36F)
        return 0;
    if (cp < 0x1100)
        return 1;
    if (cp <= 0x115F)
        return 2;
    if (cp == 0x2329 || cp == 0x232A)
        return 2;
    if (cp >= 0x2E80 && cp <= 0xA4CF)
        return 2;
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return 2;
    if (cp >= 0xF900 && cp <= 0xFAFF)
        return 2;
    if (cp >= 0xFE10 && cp <= 0xFE19)
        return 2;
    if (cp >= 0xFE30 && cp <= 0xFE6F)
        return 2;
    if (cp >= 0xFF00 && cp <= 0xFF60)
        return 2;
    if (cp >= 0xFFE0 && cp <= 0xFFE6)
        return 2;
    if (cp >= 0x1F300 && cp <= 0x1FAFF)
        return 2;
    if (cp >= 0x20000 && cp <= 0x3FFFD)
        return 2;
    return 1;
}

void set256(TermColor &dst, int index)
{
    index = qBound(0, index, 255);
    if (index < 16) {
        dst.kind = TermColor::Indexed;
        dst.index = quint8(index);
        return;
    }
    dst.kind = TermColor::Rgb;
    if (index >= 232) {
        const int gray = 8 + (index - 232) * 10;
        dst.rgb = qRgb(gray, gray, gray);
        return;
    }
    index -= 16;
    const int r = index / 36;
    const int g = (index / 6) % 6;
    const int b = index % 6;
    auto level = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
    dst.rgb = qRgb(level(r), level(g), level(b));
}

bool isIpv4Dotted(const QString &text)
{
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;
    for (const QString &part : parts) {
        if (part.isEmpty() || part.size() > 3)
            return false;
        bool ok = false;
        const int v = part.toInt(&ok);
        if (!ok || v < 0 || v > 255)
            return false;
    }
    return true;
}

// 校验去掉 zone/前缀后的地址。压缩形式必须含一处 "::"，且有效组数不超过 7。
bool isIpv6Address(QString token)
{
    const int slash = token.indexOf(QLatin1Char('/'));
    if (slash >= 0) {
        bool ok = false;
        const int prefix = token.mid(slash + 1).toInt(&ok);
        if (!ok || prefix < 0 || prefix > 128)
            return false;
        token = token.left(slash);
    }
    const int zone = token.indexOf(QLatin1Char('%'));
    if (zone >= 0) {
        if (zone == 0 || zone + 1 >= token.size())
            return false;
        token = token.left(zone);
    }
    if (token.count(QStringLiteral("::")) > 1 || token.contains(QStringLiteral(":::")))
        return false;
    if (token.startsWith(QLatin1Char(':')) && !token.startsWith(QStringLiteral("::")))
        return false;
    if (token.endsWith(QLatin1Char(':')) && !token.endsWith(QStringLiteral("::")))
        return false;

    const bool compressed = token.contains(QStringLiteral("::"));
    const QStringList parts = token.split(QLatin1Char(':'));
    int groups = 0;
    for (int i = 0; i < parts.size(); ++i) {
        const QString &part = parts.at(i);
        if (part.isEmpty())
            continue;
        if (part.contains(QLatin1Char('.'))) {
            if (i != parts.size() - 1 || !isIpv4Dotted(part))
                return false;
            groups += 2; // IPv4 映射地址占两个组
            continue;
        }
        if (part.size() > 4)
            return false;
        for (const QChar c : part) {
            if (!c.isDigit() && (c.toLower() < QLatin1Char('a') || c.toLower() > QLatin1Char('f')))
                return false;
        }
        ++groups;
    }
    if (!compressed)
        return groups == 8;
    return groups <= 7;
}

} // namespace

TermScreen::TermScreen()
{
    m_lines.assign(size_t(m_rows), blankLine());
}

void TermScreen::setDark(bool dark)
{
    m_dark = dark;
}

void TermScreen::setReplyHandler(std::function<void(const QByteArray &)> reply)
{
    m_reply = std::move(reply);
}

void TermScreen::setTitleHandler(std::function<void(const QString &)> title)
{
    m_onTitle = std::move(title);
}

bool TermScreen::takeBell()
{
    const bool rang = m_bell;
    m_bell = false;
    return rang;
}

int TermScreen::cursorCol() const
{
    if (m_lines.empty() || m_row < 0 || m_row >= int(m_lines.size()))
        return 0;
    int c = m_col;
    if (c >= m_cols)
        c = m_cols - 1;
    if (c < 0)
        c = 0;
    const auto &cells = m_lines[size_t(m_row)].cells;
    if (c > 0 && c < int(cells.size()) && (cells[size_t(c)].flags & FlagWideCont))
        --c;
    return c;
}

const TermLine *TermScreen::lineAbsolute(int absRow) const
{
    if (absRow < 0)
        return nullptr;
    if (m_alt) {
        if (absRow >= m_rows)
            return nullptr;
        return &m_lines[size_t(absRow)];
    }
    if (absRow < int(m_hist.size()))
        return &m_hist[size_t(absRow)];
    const int screenRow = absRow - int(m_hist.size());
    if (screenRow < 0 || screenRow >= int(m_lines.size()))
        return nullptr;
    return &m_lines[size_t(screenRow)];
}

QColor TermScreen::resolve(const TermColor &color, bool foreground) const
{
    const Palette &pal = palette(m_dark);
    switch (color.kind) {
    case TermColor::Info:
        return pal.info;
    case TermColor::Rgb:
        return QColor::fromRgb(color.rgb);
    case TermColor::Indexed: {
        const int idx = qBound(0, int(color.index), 15);
        return idx < 8 ? pal.normal[idx] : pal.bright[idx - 8];
    }
    case TermColor::Default:
        break;
    }
    return foreground ? pal.fg : pal.bg;
}

QColor TermScreen::foreground(const TermCell &cell) const
{
    return resolve(cell.fg, true);
}

QColor TermScreen::background(const TermCell &cell) const
{
    return resolve(cell.bg, false);
}

QColor TermScreen::defaultForeground() const
{
    return palette(m_dark).fg;
}

QColor TermScreen::defaultBackground() const
{
    return palette(m_dark).bg;
}

QColor TermScreen::selectionBackground() const
{
    return palette(m_dark).selection;
}

TermCell TermScreen::blankCell() const
{
    return TermCell();
}

TermCell TermScreen::erasedCell() const
{
    TermCell cell;
    cell.bg = m_pen.bg;
    return cell;
}

TermLine TermScreen::blankLine() const
{
    TermLine line;
    line.cells.assign(size_t(qMax(1, m_cols)), blankCell());
    return line;
}

int TermScreen::feed(const QByteArray &data)
{
    m_histCleared = false;
    for (int i = 0; i < data.size(); ++i)
        feedByte(static_cast<unsigned char>(data.at(i)));
    return finishFeed();
}

int TermScreen::appendNote(const QString &text)
{
    m_histCleared = false;
    if (m_alt)
        leaveAlt();
    m_state = Normal;
    m_utfLeft = 0;

    const bool dirty = m_col != 0 || m_wrapPending || lineHasText(m_row);
    if (dirty) {
        putControl('\r');
        putControl('\n');
    }

    const Pen saved = m_pen;
    m_pen = Pen();
    m_pen.fg.kind = TermColor::Info;

    const QList<uint> cps = text.toUcs4();
    for (uint cp : cps) {
        if (cp == '\n' || cp == '\r') {
            putControl('\r');
            putControl('\n');
        } else if (cp >= 0x20) {
            putCp(char32_t(cp));
        }
    }
    m_pen = saved;
    putControl('\r');
    putControl('\n');
    return finishFeed();
}

int TermScreen::resize(int cols, int rows)
{
    cols = qBound(20, cols, 500);
    rows = qBound(4, rows, 200);
    if (cols == m_cols && rows == m_rows)
        return 0;
    m_cols = cols;
    m_rows = rows;
    if (m_alt) {
        refit(m_lines, false);
        if (!m_primary.empty())
            refit(m_primary, m_used);
    } else {
        refit(m_lines, m_used);
        if (!m_primary.empty())
            refit(m_primary, false);
    }
    m_row = qBound(0, m_row, m_rows - 1);
    if (m_col > m_cols)
        m_col = m_cols;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_wrapPending = false;
    return trim();
}

int TermScreen::finishFeed()
{
    paintTouchedLines();
    if (m_histCleared) {
        m_histCleared = false;
        return -1;
    }
    return trim();
}

int TermScreen::trim()
{
    int dropped = 0;
    while (int(m_hist.size()) > kMaxHist) {
        m_hist.pop_front();
        ++dropped;
    }
    if (dropped > 0)
        m_unscannedHist = qMax(0, m_unscannedHist - dropped); // 头部丢弃后下标前移
    return dropped;
}

void TermScreen::touchRow(int row)
{
    if (m_touchMin < 0 || row < m_touchMin)
        m_touchMin = row;
    if (row > m_touchMax)
        m_touchMax = row;
}

void TermScreen::touchAll()
{
    m_touchMin = 0;
    m_touchMax = m_rows - 1;
}

void TermScreen::paintTouchedLines()
{
    if (!m_alt) {
        // 刚滚进历史的行内容已定稿（进历史后不会再改），补一次色；幂等，重扫无副作用
        while (m_unscannedHist < int(m_hist.size()))
            paintIpTokens(m_hist[size_t(m_unscannedHist++)]);
        // 光标行总是重扫：词可能被 IO 拆在两个 chunk 里分批拼出来
        touchRow(m_row);
        if (m_touchMin >= 0) {
            const int top = qMax(0, m_touchMin);
            const int bottom = qMin(m_rows - 1, m_touchMax);
            for (int r = top; r <= bottom; ++r)
                paintIpTokens(m_lines[size_t(r)]);
        }
    }
    m_touchMin = -1;
    m_touchMax = -1;
}

// 设备若已用 ANSI 上色则保持原样；否则按 Ubuntu ip -c 给地址、网卡和链路状态补色。
// 颜色存调色板下标，主题切换由 resolve() 解析，不用重写格子。
void TermScreen::paintIpTokens(TermLine &line)
{
    if (line.cells.empty())
        return;
    QString text;
    text.reserve(int(line.cells.size()));
    QVector<int> cellAt; // 文本位 → cell 下标（宽字符占 1 格多文本位，都指向首格）
    cellAt.reserve(int(line.cells.size()) + 1);
    for (size_t i = 0; i < line.cells.size(); ++i) {
        const TermCell &cell = line.cells[i];
        if (cell.flags & FlagWideCont)
            continue;
        if (cell.cp < 0x10000) {
            cellAt.append(int(i));
            text.append(QChar(char16_t(cell.cp)));
        } else {
            char32_t cp = cell.cp;
            const QString ch = QString::fromUcs4(&cp, 1);
            for (int k = 0; k < ch.size(); ++k)
                cellAt.append(int(i));
            text += ch;
        }
    }
    if (text.isEmpty() || text.size() > 8192)
        return;
    cellAt.append(int(line.cells.size())); // 尾哨兵

    struct TokenStyle
    {
        TermColor fg;
        quint8 attr = 0;
    };
    const bool dark = m_dark; // 暗色主题下蓝/黄对比不够，提亮加粗才看得清
    auto style = [dark](int ansi, bool bright, bool bold) {
        TokenStyle s;
        s.fg.kind = TermColor::Indexed;
        s.fg.index = quint8(bright ? ansi + 8 : ansi);
        s.attr = bold ? AttrBold : quint8(0);
        return s;
    };
    const TokenStyle ipv4Style = style(5, false, dark); // 洋红
    const TokenStyle ipv6Style = style(4, dark, dark);  // 蓝，暗色主题提亮加粗才看得清
    const TokenStyle macStyle = style(3, false, dark);  // 黄
    const TokenStyle ifStyle = style(6, false, dark);   // 青
    const TokenStyle upStyle = style(2, false, dark);   // 绿
    const TokenStyle downStyle = style(1, false, dark); // 红

    static const QRegularExpression ipv6Re(
        QStringLiteral("(?<![0-9A-Fa-f:.])(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}"
                       "(?:%[0-9A-Za-z._~-]+)?(?:/(?:12[0-8]|1[01][0-9]|[1-9]?[0-9]))?"
                       "(?![0-9A-Fa-f:.])"));
    static const QRegularExpression ipv4Re(
        QStringLiteral("(?<![\\d.])(?:(?:25[0-5]|2[0-4]\\d|1\\d\\d|0?\\d\\d?)\\.){3}"
                       "(?:25[0-5]|2[0-4]\\d|1\\d\\d|0?\\d\\d?)(?:/(?:3[0-2]|[12]?\\d))?(?![\\d.])"));
    static const QRegularExpression macRe(
        QStringLiteral("(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}"
                       "(?![0-9A-Fa-f:])"));
    static const QRegularExpression ifRe(
        QStringLiteral("(?<![A-Za-z0-9])(?:"
                       "enp\\d+s\\d+(?:f\\d+)?|wlp\\d+s\\d+|wlx[0-9A-Fa-f]+|"
                       "ens\\d+|eno\\d+|eth\\d+|wlan\\d+|usb\\d+|bond\\d+|"
                       "virbr\\d+|veth[0-9A-Fa-f]+|docker\\d*|br\\d+|tun\\d+|tap\\d+|"
                       "wg\\d+|can\\d+|tunl\\d+|sit\\d+|ip6tnl\\d+|lo"
                       ")(?![A-Za-z0-9])"));
    static const QRegularExpression stateRe(
        QStringLiteral("(?<![A-Za-z0-9_])(LOWER_UP|UP|DOWN)(?![A-Za-z0-9_])"));

    QVector<char> taken(text.size(), 0);
    auto take = [&taken](int start, int len) {
        for (int i = start; i < start + len && i < taken.size(); ++i) {
            if (taken[i])
                return false;
        }
        for (int i = start; i < start + len && i < taken.size(); ++i)
            taken[i] = 1;
        return true;
    };
    // 区间里只要有一格不是「素」的（设备上过色/系统提示），整段都不动
    auto paint = [&](int start, int len, const TokenStyle &s) {
        const int c0 = cellAt[start];
        const int c1 = cellAt[start + len];
        for (int c = c0; c < c1; ++c) {
            const TermCell &cell = line.cells[size_t(c)];
            if (cell.fg.kind != TermColor::Default || cell.bg.kind != TermColor::Default
                || cell.attr)
                return;
        }
        for (int c = c0; c < c1; ++c) {
            TermCell &cell = line.cells[size_t(c)];
            cell.fg = s.fg;
            cell.attr = s.attr;
        }
    };

    auto matches = ipv6Re.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!isIpv6Address(m.captured()) || !take(m.capturedStart(), m.capturedLength()))
            continue;
        paint(m.capturedStart(), m.capturedLength(), ipv6Style);
    }
    matches = ipv4Re.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!take(m.capturedStart(), m.capturedLength()))
            continue;
        paint(m.capturedStart(), m.capturedLength(), ipv4Style);
    }
    matches = macRe.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!take(m.capturedStart(), m.capturedLength()))
            continue;
        paint(m.capturedStart(), m.capturedLength(), macStyle);
    }
    matches = ifRe.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!take(m.capturedStart(), m.capturedLength()))
            continue;
        paint(m.capturedStart(), m.capturedLength(), ifStyle);
    }
    matches = stateRe.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!take(m.capturedStart(), m.capturedLength()))
            continue;
        const bool down = m.captured(1) == QLatin1String("DOWN");
        paint(m.capturedStart(), m.capturedLength(), down ? downStyle : upStyle);
    }
}

void TermScreen::refit(std::vector<TermLine> &buf, bool toHistory)
{
    while (int(buf.size()) > m_rows) {
        if (toHistory)
            m_hist.push_back(std::move(buf.front()));
        buf.erase(buf.begin());
    }
    while (int(buf.size()) < m_rows)
        buf.push_back(blankLine());
    for (TermLine &line : buf) {
        if (int(line.cells.size()) < m_cols)
            line.cells.resize(size_t(m_cols), blankCell());
        else if (int(line.cells.size()) > m_cols)
            line.cells.resize(size_t(m_cols));
        if (!line.cells.empty() && (line.cells.back().flags & FlagWide))
            line.cells.back() = blankCell();
    }
}

bool TermScreen::lineHasText(int row) const
{
    if (row < 0 || row >= int(m_lines.size()))
        return false;
    for (const TermCell &cell : m_lines[size_t(row)].cells) {
        if (cell.cp != U' ' && cell.cp != 0)
            return true;
    }
    return false;
}

void TermScreen::feedByte(unsigned char b)
{
    if (m_utfLeft > 0) {
        if ((b & 0xC0) != 0x80) {
            m_utfLeft = 0;
            putCp(0xFFFD);
            feedByte(b);
            return;
        }
        m_utfAcc = (m_utfAcc << 6) | (b & 0x3F);
        if (--m_utfLeft == 0)
            putCp(m_utfAcc);
        return;
    }
    if (m_state == Normal && b >= 0x80) {
        if ((b & 0xE0) == 0xC0 && b >= 0xC2) {
            m_utfAcc = b & 0x1F;
            m_utfLeft = 1;
        } else if ((b & 0xF0) == 0xE0) {
            m_utfAcc = b & 0x0F;
            m_utfLeft = 2;
        } else if ((b & 0xF8) == 0xF0 && b <= 0xF4) {
            m_utfAcc = b & 0x07;
            m_utfLeft = 3;
        } else {
            putCp(0xFFFD);
        }
        return;
    }
    if (b >= 0x80 && m_state != Normal) {
        m_state = Normal;
        feedByte(b);
        return;
    }
    dispatch(b);
}

void TermScreen::dispatch(unsigned char b)
{
    switch (m_state) {
    case Normal:
        if (b == 0x1B) {
            m_state = Esc;
            return;
        }
        if (b == '\n' || b == '\r' || b == '\b' || b == '\t' || b == 0x0B || b == 0x0C) {
            putControl(b == 0x0B || b == 0x0C ? '\n' : b);
            return;
        }
        if (b == 0x07) {
            m_bell = true;
            return;
        }
        if (b == 0x00 || b == 0x7F)
            return;
        if (b < 0x20)
            return;
        putCp(b);
        return;
    case Esc:
        m_state = Normal;
        if (b == '[') {
            beginCsi();
            return;
        }
        if (b == ']') {
            m_state = Osc;
            m_osc.clear();
            return;
        }
        if (b == 'E') { // NEL
            putControl('\r');
            lineFeed();
            return;
        }
        if (b == '(' || b == ')' || b == '#') {
            m_state = Charset;
            return;
        }
        if (b == 'D') {
            lineFeed();
            return;
        }
        if (b == 'M') {
            reverseIndex();
            return;
        }
        if (b == '7') {
            m_scRow = m_row;
            m_scCol = qMin(m_col, m_cols - 1);
            m_scPen = m_pen;
            return;
        }
        if (b == '8') {
            m_row = qBound(0, m_scRow, m_rows - 1);
            m_col = qBound(0, m_scCol, m_cols - 1);
            m_pen = m_scPen;
            m_wrapPending = false;
            return;
        }
        if (b == 'c')
            reset();
        return;
    case Csi:
        if (b == 0x1B) {
            m_state = Esc;
            return;
        }
        if (b < 0x20) {
            m_state = Normal;
            dispatch(b);
            return;
        }
        if (b >= '0' && b <= '9') {
            const int digit = b - '0';
            m_acc = qMin(9999, (m_acc < 0 ? 0 : m_acc) * 10 + digit);
            return;
        }
        if (b == ';') {
            pushParam();
            return;
        }
        if (b == '?' || b == '>' || b == '<' || b == '=') {
            if (b == '?')
                m_priv = true;
            else
                m_mod = char(b);
            return;
        }
        if (b >= 0x40 && b <= 0x7E) {
            pushParam();
            handleCsi(char(b));
            m_state = Normal;
        }
        return;
    case Osc:
        if (b == 0x07) {
            finishOsc();
            m_state = Normal;
            return;
        }
        if (b == 0x1B) {
            m_state = OscEsc;
            return;
        }
        if (m_osc.size() < 512)
            m_osc.append(char(b));
        return;
    case OscEsc:
        if (b == '\\')
            finishOsc();
        else
            m_osc.clear();
        m_state = (b == 0x1B) ? Esc : Normal;
        return;
    case Charset:
        m_state = Normal;
        return;
    }
}

void TermScreen::beginCsi()
{
    m_state = Csi;
    m_nparams = 0;
    m_acc = -1;
    m_priv = false;
    m_mod = 0;
}

void TermScreen::pushParam()
{
    if (m_nparams < 32)
        m_params[m_nparams++] = m_acc;
    m_acc = -1;
}

int TermScreen::arg(int index, int fallback) const
{
    if (index < 0 || index >= m_nparams || m_params[index] < 0)
        return fallback;
    return m_params[index];
}

void TermScreen::handleCsi(char finalByte)
{
    switch (finalByte) {
    case 'm':
        applySgr();
        return;
    case 'h':
    case 'l': {
        const bool enable = finalByte == 'h';
        if (m_priv) {
            for (int i = 0; i < m_nparams; ++i)
                setPrivateMode(m_params[i] < 0 ? 0 : m_params[i], enable);
        } else {
            for (int i = 0; i < m_nparams; ++i) {
                if ((m_params[i] < 0 ? 0 : m_params[i]) == 4)
                    m_insertMode = enable; // IRM
            }
        }
        return;
    }
    case 'H':
    case 'f': {
        const int rowArg = qMax(1, arg(0, 1));
        const int colArg = qMax(1, arg(1, 1));
        if (m_originMode)
            m_row = qBound(m_scrollTop, m_scrollTop + rowArg - 1, m_scrollBottom);
        else
            m_row = qBound(0, rowArg - 1, m_rows - 1);
        m_col = qBound(0, colArg - 1, m_cols - 1);
        m_wrapPending = false;
        return;
    }
    case 'A':
        m_row = qMax(m_originMode ? m_scrollTop : 0, m_row - qMax(1, arg(0, 1)));
        m_wrapPending = false;
        return;
    case 'B':
        m_row = qMin(m_originMode ? m_scrollBottom : m_rows - 1, m_row + qMax(1, arg(0, 1)));
        m_wrapPending = false;
        return;
    case 'E':
        for (int i = 0; i < qMax(1, arg(0, 1)); ++i)
            lineFeed();
        m_col = 0;
        m_wrapPending = false;
        return;
    case 'F':
        for (int i = 0; i < qMax(1, arg(0, 1)); ++i)
            reverseIndex();
        m_col = 0;
        m_wrapPending = false;
        return;
    case 'b':
        if (m_lastCp != 0) {
            const int n = qBound(1, arg(0, 1), 4096);
            const char32_t cp = m_lastCp;
            for (int i = 0; i < n; ++i)
                putCp(cp);
        }
        return;
    case 'C': {
        const int col = m_wrapPending ? m_cols - 1 : m_col;
        m_col = qMin(m_cols - 1, col + qMax(1, arg(0, 1)));
        m_wrapPending = false;
        return;
    }
    case 'D': {
        const int col = m_wrapPending ? m_cols - 1 : m_col;
        m_col = qMax(0, col - qMax(1, arg(0, 1)));
        m_wrapPending = false;
        return;
    }
    case 'G':
        m_col = qBound(0, arg(0, 1) - 1, m_cols - 1);
        m_wrapPending = false;
        return;
    case 'd':
        m_row = qBound(0, arg(0, 1) - 1, m_rows - 1);
        m_wrapPending = false;
        return;
    case 'K': {
        const int col = qMin(m_col, m_cols - 1);
        const int mode = arg(0, 0);
        if (mode == 1)
            eraseCells(m_row, 0, col + 1);
        else if (mode == 2)
            eraseCells(m_row, 0, m_cols);
        else
            eraseCells(m_row, col, m_cols);
        m_wrapPending = false;
        return;
    }
    case 'J': {
        const int col = qMin(m_col, m_cols - 1);
        const int mode = arg(0, 0);
        if (mode == 1) {
            for (int r = 0; r < m_row; ++r)
                eraseCells(r, 0, m_cols);
            eraseCells(m_row, 0, col + 1);
        } else if (mode == 2) {
            for (int r = 0; r < m_rows; ++r)
                m_lines[size_t(r)] = blankLine();
        } else if (mode == 3) {
            m_hist.clear();
            m_histCleared = true;
            m_unscannedHist = 0;
        } else {
            eraseCells(m_row, col, m_cols);
            for (int r = m_row + 1; r < m_rows; ++r)
                eraseCells(r, 0, m_cols);
        }
        m_wrapPending = false;
        return;
    }
    case 'L':
        insertLines(qMax(1, arg(0, 1)));
        return;
    case 'M':
        deleteLines(qMax(1, arg(0, 1)));
        return;
    case '@':
        insertChars(qMax(1, arg(0, 1)));
        return;
    case 'P':
        deleteChars(qMax(1, arg(0, 1)));
        return;
    case 'X':
        eraseCells(m_row, qMin(m_col, m_cols - 1),
                   qMin(m_col, m_cols - 1) + qMax(1, arg(0, 1)));
        return;
    case 'S':
        scrollUp(qMax(1, arg(0, 1)));
        return;
    case 'T':
        if (m_nparams <= 1)
            scrollDown(qMax(1, arg(0, 1)));
        return;
    case 'r': {
        int top = qBound(1, arg(0, 1), m_rows);
        int bot = qBound(1, arg(1, m_rows), m_rows);
        if (top >= bot) {
            m_scrollTop = 0;
            m_scrollBottom = m_rows - 1;
        } else {
            m_scrollTop = top - 1;
            m_scrollBottom = bot - 1;
        }
        m_row = 0;
        m_col = 0;
        m_wrapPending = false;
        return;
    }
    case 'n':
        if (!m_priv && m_mod == 0 && arg(0, 0) == 6) {
            replyBytes(QByteArray("\x1b[") + QByteArray::number(m_row + 1) + ';'
                       + QByteArray::number(cursorCol() + 1) + 'R');
        } else if (!m_priv && m_mod == 0 && arg(0, 0) == 5) {
            replyBytes(QByteArray("\x1b[0n"));
        }
        return;
    case 's':
        m_scRow = m_row;
        m_scCol = qMin(m_col, m_cols - 1);
        m_scPen = m_pen;
        return;
    case 'u':
        m_row = qBound(0, m_scRow, m_rows - 1);
        m_col = qBound(0, m_scCol, m_cols - 1);
        m_pen = m_scPen;
        m_wrapPending = false;
        return;
    default:
        return;
    }
}

void TermScreen::applySgr()
{
    if (m_nparams <= 0) {
        m_pen = Pen();
        return;
    }
    for (int i = 0; i < m_nparams; ++i) {
        const int p = m_params[i] < 0 ? 0 : m_params[i];
        if (p == 0) {
            m_pen = Pen();
        } else if (p == 1) {
            m_pen.attr |= AttrBold;
        } else if (p == 4) {
            m_pen.attr |= AttrUnderline;
        } else if (p == 7) {
            m_pen.attr |= AttrInverse;
        } else if (p == 22) {
            m_pen.attr = quint8(m_pen.attr & ~AttrBold);
        } else if (p == 24) {
            m_pen.attr = quint8(m_pen.attr & ~AttrUnderline);
        } else if (p == 27) {
            m_pen.attr = quint8(m_pen.attr & ~AttrInverse);
        } else if (p >= 30 && p <= 37) {
            m_pen.fg.kind = TermColor::Indexed;
            m_pen.fg.index = quint8(p - 30);
        } else if (p == 39) {
            m_pen.fg = TermColor();
        } else if (p >= 40 && p <= 47) {
            m_pen.bg.kind = TermColor::Indexed;
            m_pen.bg.index = quint8(p - 40);
        } else if (p == 49) {
            m_pen.bg = TermColor();
        } else if (p >= 90 && p <= 97) {
            m_pen.fg.kind = TermColor::Indexed;
            m_pen.fg.index = quint8(8 + p - 90);
        } else if (p >= 100 && p <= 107) {
            m_pen.bg.kind = TermColor::Indexed;
            m_pen.bg.index = quint8(8 + p - 100);
        } else if (p == 38 || p == 48) {
            TermColor *dst = (p == 38) ? &m_pen.fg : &m_pen.bg;
            if (i + 1 >= m_nparams)
                break;
            const int mode = m_params[i + 1] < 0 ? 0 : m_params[i + 1];
            if (mode == 5 && i + 2 < m_nparams) {
                set256(*dst, m_params[i + 2] < 0 ? 0 : m_params[i + 2]);
                i += 2;
            } else if (mode == 2 && i + 4 < m_nparams) {
                const int r = qBound(0, m_params[i + 2] < 0 ? 0 : m_params[i + 2], 255);
                const int g = qBound(0, m_params[i + 3] < 0 ? 0 : m_params[i + 3], 255);
                const int b = qBound(0, m_params[i + 4] < 0 ? 0 : m_params[i + 4], 255);
                dst->kind = TermColor::Rgb;
                dst->rgb = qRgb(r, g, b);
                i += 4;
            }
        }
    }
}

void TermScreen::setPrivateMode(int mode, bool enable)
{
    switch (mode) {
    case 1:
        m_appCursor = enable;
        break;
    case 6:
        m_originMode = enable;
        if (enable) {
            m_row = m_scrollTop;
            m_col = 0;
            m_wrapPending = false;
        }
        break;
    case 7:
        m_autowrap = enable;
        break;
    case 25:
        m_cursorHidden = !enable;
        break;
    case 47:
    case 1047:
    case 1049:
        if (enable)
            enterAlt();
        else
            leaveAlt();
        break;
    case 2004:
        m_bracketPaste = enable;
        break;
    case 2026:
        m_syncing = enable;
        break;
    default:
        break;
    }
}

void TermScreen::putCp(char32_t cp)
{
    if (m_row < 0 || m_row >= m_rows || m_lines.empty())
        return;
    m_used = true;
    int width = glyphCols(cp);
    if (width <= 0)
        return;

    if (m_wrapPending || m_col >= m_cols) {
        if (m_autowrap) {
            m_lines[size_t(m_row)].wrapped = true;
            m_wrapPending = false;
            lineFeed();
            m_col = 0;
        } else {
            m_col = m_cols - 1;
            m_wrapPending = false;
            width = 1;
        }
    }
    if (m_col + width > m_cols) {
        if (m_autowrap) {
            m_lines[size_t(m_row)].wrapped = true;
            lineFeed();
            m_col = 0;
        } else {
            width = 1;
            m_col = m_cols - 1;
        }
    }
    if (m_row < 0 || m_row >= m_rows)
        return;
    if (m_insertMode)
        insertChars(width);
    writeAt(m_row, m_col, cp, width);
    m_lastCp = cp;
    m_col += width;
    m_wrapPending = m_col >= m_cols;
    if (m_wrapPending)
        m_col = m_cols;
}

void TermScreen::putControl(unsigned char b)
{
    if (b == '\r') {
        m_col = 0;
        m_wrapPending = false;
        return;
    }
    if (b == '\n') {
        m_wrapPending = false;
        lineFeed();
        return;
    }
    if (b == '\b') {
        if (m_wrapPending) {
            m_wrapPending = false;
            m_col = qMax(0, m_cols - 1);
        } else if (m_col > 0) {
            --m_col;
        }
        return;
    }
    if (b == '\t') {
        if (m_wrapPending) {
            m_wrapPending = false;
            m_col = qMax(0, m_cols - 1);
            return;
        }
        const int next = (m_col / 8 + 1) * 8;
        m_col = qMin(next, qMax(0, m_cols - 1));
    }
}

void TermScreen::lineFeed()
{
    m_wrapPending = false;
    if (m_row == m_scrollBottom)
        scrollUp(1);
    else if (m_row < m_rows - 1)
        ++m_row;
}

void TermScreen::reverseIndex()
{
    m_wrapPending = false;
    if (m_row == m_scrollTop)
        scrollDown(1);
    else if (m_row > 0)
        --m_row;
}

void TermScreen::scrollUp(int n)
{
    if (m_rows <= 0 || m_scrollBottom < m_scrollTop)
        return;
    touchAll();
    n = qBound(1, n, m_scrollBottom - m_scrollTop + 1);
    const bool toHist = !m_alt && m_scrollTop == 0 && m_scrollBottom == m_rows - 1;
    for (int i = 0; i < n; ++i) {
        TermLine leaving = std::move(m_lines[size_t(m_scrollTop)]);
        for (int r = m_scrollTop; r < m_scrollBottom; ++r)
            m_lines[size_t(r)] = std::move(m_lines[size_t(r + 1)]);
        m_lines[size_t(m_scrollBottom)] = blankLine();
        if (toHist)
            m_hist.push_back(std::move(leaving));
    }
}

void TermScreen::scrollDown(int n)
{
    if (m_rows <= 0 || m_scrollBottom < m_scrollTop)
        return;
    touchAll();
    n = qBound(1, n, m_scrollBottom - m_scrollTop + 1);
    for (int i = 0; i < n; ++i) {
        for (int r = m_scrollBottom; r > m_scrollTop; --r)
            m_lines[size_t(r)] = std::move(m_lines[size_t(r - 1)]);
        m_lines[size_t(m_scrollTop)] = blankLine();
    }
}

void TermScreen::insertLines(int n)
{
    if (m_row < m_scrollTop || m_row > m_scrollBottom)
        return;
    touchAll();
    n = qBound(1, n, m_scrollBottom - m_row + 1);
    for (int i = 0; i < n; ++i) {
        for (int r = m_scrollBottom; r > m_row; --r)
            m_lines[size_t(r)] = std::move(m_lines[size_t(r - 1)]);
        m_lines[size_t(m_row)] = blankLine();
    }
    m_wrapPending = false;
}

void TermScreen::deleteLines(int n)
{
    if (m_row < m_scrollTop || m_row > m_scrollBottom)
        return;
    touchAll();
    n = qBound(1, n, m_scrollBottom - m_row + 1);
    for (int i = 0; i < n; ++i) {
        for (int r = m_row; r < m_scrollBottom; ++r)
            m_lines[size_t(r)] = std::move(m_lines[size_t(r + 1)]);
        m_lines[size_t(m_scrollBottom)] = blankLine();
    }
    m_wrapPending = false;
}

void TermScreen::insertChars(int n)
{
    if (m_row < 0 || m_row >= m_rows)
        return;
    touchRow(m_row);
    n = qBound(1, n, m_cols);
    const int col = qMin(m_col, m_cols - 1);
    auto &cells = m_lines[size_t(m_row)].cells;
    if (int(cells.size()) < m_cols)
        cells.resize(size_t(m_cols), blankCell());
    for (int c = m_cols - 1; c >= col + n; --c)
        cells[size_t(c)] = cells[size_t(c - n)];
    for (int c = col; c < col + n && c < m_cols; ++c)
        cells[size_t(c)] = erasedCell();
    repairWide(m_row);
    m_wrapPending = false;
}

void TermScreen::deleteChars(int n)
{
    if (m_row < 0 || m_row >= m_rows)
        return;
    touchRow(m_row);
    n = qBound(1, n, m_cols);
    const int col = qMin(m_col, m_cols - 1);
    auto &cells = m_lines[size_t(m_row)].cells;
    if (int(cells.size()) < m_cols)
        cells.resize(size_t(m_cols), blankCell());
    for (int c = col; c + n < m_cols; ++c)
        cells[size_t(c)] = cells[size_t(c + n)];
    for (int c = qMax(col, m_cols - n); c < m_cols; ++c)
        cells[size_t(c)] = erasedCell();
    repairWide(m_row);
    m_wrapPending = false;
}

void TermScreen::eraseCells(int row, int c0, int c1)
{
    if (row < 0 || row >= m_rows)
        return;
    touchRow(row);
    auto &cells = m_lines[size_t(row)].cells;
    if (int(cells.size()) < m_cols)
        cells.resize(size_t(m_cols), blankCell());
    c0 = qBound(0, c0, m_cols);
    c1 = qBound(0, c1, m_cols);
    if (c0 > 0 && (cells[size_t(c0)].flags & FlagWideCont))
        cells[size_t(c0 - 1)] = erasedCell();
    for (int c = c0; c < c1; ++c)
        cells[size_t(c)] = erasedCell();
    if (c1 < m_cols && (cells[size_t(c1)].flags & FlagWideCont))
        cells[size_t(c1)] = erasedCell();
}

void TermScreen::writeAt(int row, int col, char32_t cp, int width)
{
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols)
        return;
    touchRow(row);
    auto &cells = m_lines[size_t(row)].cells;
    if (int(cells.size()) < m_cols)
        cells.resize(size_t(m_cols), blankCell());
    clearWide(row, col);
    if (width == 2 && col + 1 < m_cols)
        clearWide(row, col + 1);

    TermCell &cell = cells[size_t(col)];
    cell.cp = cp;
    cell.fg = m_pen.fg;
    cell.bg = m_pen.bg;
    cell.attr = m_pen.attr;
    cell.flags = (width == 2) ? FlagWide : 0;
    if (width == 2 && col + 1 < m_cols) {
        TermCell &next = cells[size_t(col + 1)];
        next = erasedCell();
        next.flags = FlagWideCont;
        next.bg = m_pen.bg;
    }
}

void TermScreen::clearWide(int row, int col)
{
    if (row < 0 || col < 0 || row >= m_rows || col >= m_cols)
        return;
    auto &cells = m_lines[size_t(row)].cells;
    if (col >= int(cells.size()))
        return;
    if (cells[size_t(col)].flags & FlagWideCont) {
        cells[size_t(col)] = blankCell();
        if (col > 0)
            cells[size_t(col - 1)] = blankCell();
    } else if (cells[size_t(col)].flags & FlagWide) {
        cells[size_t(col)] = blankCell();
        if (col + 1 < int(cells.size()))
            cells[size_t(col + 1)] = blankCell();
    }
}

void TermScreen::repairWide(int row)
{
    if (row < 0 || row >= m_rows)
        return;
    auto &cells = m_lines[size_t(row)].cells;
    const int n = qMin(m_cols, int(cells.size()));
    for (int c = 0; c < n; ++c) {
        if (cells[size_t(c)].flags & FlagWide) {
            if (c + 1 >= n || !(cells[size_t(c + 1)].flags & FlagWideCont))
                cells[size_t(c)].flags = 0;
        }
        if ((cells[size_t(c)].flags & FlagWideCont)
            && (c == 0 || !(cells[size_t(c - 1)].flags & FlagWide))) {
            cells[size_t(c)] = erasedCell();
        }
    }
}

void TermScreen::enterAlt()
{
    if (m_alt)
        return;
    m_touchMin = -1; // 备用屏内容不补色，脏标记别带进主屏
    m_touchMax = -1;
    m_primary = std::move(m_lines);
    m_saveRow = m_row;
    m_saveCol = qMin(m_col, qMax(0, m_cols - 1));
    m_lines.assign(size_t(m_rows), blankLine());
    m_alt = true;
    m_row = 0;
    m_col = 0;
    m_wrapPending = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
}

void TermScreen::leaveAlt()
{
    if (!m_alt)
        return;
    m_touchMin = -1;
    m_touchMax = -1;
    if (!m_primary.empty())
        m_lines = std::move(m_primary);
    m_primary.clear();
    m_alt = false;
    m_row = qBound(0, m_saveRow, qMax(0, m_rows - 1));
    m_col = qBound(0, m_saveCol, qMax(0, m_cols - 1));
    m_wrapPending = false;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    if (int(m_lines.size()) != m_rows)
        refit(m_lines, false);
}

void TermScreen::reset()
{
    m_hist.clear();
    m_histCleared = true;
    m_unscannedHist = 0;
    m_touchMin = -1;
    m_touchMax = -1;
    m_primary.clear();
    m_alt = false;
    m_lines.assign(size_t(m_rows), blankLine());
    m_row = 0;
    m_col = 0;
    m_pen = Pen();
    m_wrapPending = false;
    m_autowrap = true;
    m_cursorHidden = false;
    m_appCursor = false;
    m_bracketPaste = false;
    m_syncing = false;
    m_originMode = false;
    m_insertMode = false;
    m_bell = false;
    m_lastCp = 0;
    m_osc.clear();
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_state = Normal;
    m_utfLeft = 0;
}

void TermScreen::replyBytes(const QByteArray &data)
{
    if (m_reply && !data.isEmpty())
        m_reply(data);
}

void TermScreen::finishOsc()
{
    const int semi = m_osc.indexOf(';');
    if (semi > 0) {
        bool ok = false;
        const int cmd = m_osc.left(semi).toInt(&ok);
        if (ok && (cmd == 0 || cmd == 2)) {
            const QString title = QString::fromUtf8(m_osc.mid(semi + 1));
            if (title != m_title) {
                m_title = title;
                if (m_onTitle)
                    m_onTitle(title);
            }
        }
    }
    m_osc.clear();
}
