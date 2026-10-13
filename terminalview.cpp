#include "terminalview.h"

#include "completionengine.h"

#include <QClipboard>
#include <QColor>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTimer>

namespace {
// 接收侧节奏参数：SSH/ConPTY 一次可能吐几十 KB，逐包渲染会把主循环占满。
constexpr int kFlushVisibleMs = 8;         // 可见时约一帧，手感上是"即时"
constexpr int kFlushHiddenMs = 120;        // 后台会话放宽节奏，省 CPU
constexpr int kFeedSliceBytes = 32 * 1024; // 单次最多渲染这么多字节
constexpr int kZeroMs = 0;                 // 积压很大时尽快续片，但仍回到事件循环
constexpr int kImmediateBytes = 256 * 1024; // 积压到这个量就不再叠加延迟
constexpr int kHiddenBacklogLimit = 1024 * 1024; // 后台最多缓存 1MB，超出保留尾部

// 零延迟通道：交互小包当场渲染，但按时间窗限制总量，突发输出照样走合并。
constexpr int kSyncChunkBytes = 4 * 1024;  // 单包上限
constexpr int kSyncWindowBytes = 16 * 1024; // 一个窗口内的同步渲染总量
constexpr qint64 kSyncWindowMs = 8;

// 深色 / 亮色两套终端配色
struct TerminalPalette
{
    QColor normal[8];
    QColor bright[8];
    QColor defaultFg;
    QColor background;
    QColor selectionBg;
    QColor echo;    // 本地回显
    QColor info;    // 系统提示
};

const TerminalPalette kDarkPalette = {
    {QColor(0x4E,0x4E,0x4E), QColor(0xCD,0x31,0x31), QColor(0x0D,0xBC,0x79),
     QColor(0xE5,0xE5,0x11), QColor(0x24,0x72,0xC8), QColor(0xBC,0x3F,0xBC),
     QColor(0x11,0xA8,0xCD), QColor(0xE5,0xE5,0xE5)},
    {QColor(0x76,0x76,0x76), QColor(0xF1,0x4C,0x4C), QColor(0x23,0xD1,0x8B),
     QColor(0xF5,0xF5,0x43), QColor(0x3B,0x8E,0xEA), QColor(0xD6,0x70,0xD6),
     QColor(0x29,0xB8,0xDB), QColor(0xFF,0xFF,0xFF)},
    QColor(0xD4,0xD4,0xD4), QColor(0x1E,0x1E,0x1E), QColor(0x26,0x4F,0x78),
    QColor(0x4E,0xC9,0xB0), QColor(0x9C,0xDC,0xFE)};

const TerminalPalette kLightPalette = {
    {QColor(0x3E,0x3E,0x3E), QColor(0xC4,0x2F,0x2F), QColor(0x0E,0x8A,0x3E),
     QColor(0xB5,0x89,0x00), QColor(0x24,0x72,0xC8), QColor(0xB0,0x3A,0xB0),
     QColor(0x11,0x8A,0xB3), QColor(0x55,0x55,0x55)},
    {QColor(0x66,0x66,0x66), QColor(0xD9,0x3C,0x3C), QColor(0x1F,0xA8,0x55),
     QColor(0xC9,0x9A,0x0A), QColor(0x1F,0x66,0xC4), QColor(0xC5,0x4E,0xC5),
     QColor(0x14,0x9C,0xC8), QColor(0x99,0x99,0x99)},
    QColor(0x24,0x29,0x2E), QColor(0xFF,0xFF,0xFF), QColor(0xC8,0xDD,0xF5),
    QColor(0x0E,0x7A,0x6E), QColor(0x0E,0x63,0x9C)};

const TerminalPalette *s_palette = &kDarkPalette;

QColor paletteColor(int idx, bool brightVariant)
{
    idx = qBound(0, idx, 7);
    return brightVariant ? s_palette->bright[idx] : s_palette->normal[idx];
}

bool sameRgb(const QColor &a, const QColor &b)
{
    return a.isValid() && b.isValid() && a.red() == b.red() && a.green() == b.green()
           && a.blue() == b.blue();
}

// 字符格式里存的是插入时的具体颜色。主题切换只改样式表，盖不住这些格式，
// 所以按旧调色板的角色（默认色/ANSI/回显/系统消息/背景）映射到新调色板。
QColor mappedPaletteColor(const QColor &color, const TerminalPalette *from,
                          const TerminalPalette *to)
{
    const QColor fromColors[] = {
        from->defaultFg, from->background, from->echo, from->info,
        from->normal[0], from->normal[1], from->normal[2], from->normal[3],
        from->normal[4], from->normal[5], from->normal[6], from->normal[7],
        from->bright[0], from->bright[1], from->bright[2], from->bright[3],
        from->bright[4], from->bright[5], from->bright[6], from->bright[7]};
    const QColor toColors[] = {
        to->defaultFg, to->background, to->echo, to->info,
        to->normal[0], to->normal[1], to->normal[2], to->normal[3],
        to->normal[4], to->normal[5], to->normal[6], to->normal[7],
        to->bright[0], to->bright[1], to->bright[2], to->bright[3],
        to->bright[4], to->bright[5], to->bright[6], to->bright[7]};
    for (int i = 0; i < 20; ++i) {
        if (sameRgb(color, fromColors[i]))
            return toColors[i];
    }
    return color;
}

void recolorDocument(QTextDocument *doc, const TerminalPalette *from,
                     const TerminalPalette *to)
{
    struct Edit
    {
        int pos;
        int length;
        QTextCharFormat fmt;
    };
    QList<Edit> edits;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.length() <= 0)
                continue;
            QTextCharFormat fmt = frag.charFormat();
            bool changed = false;
            if (fmt.foreground().style() != Qt::NoBrush) {
                const QColor mapped = mappedPaletteColor(fmt.foreground().color(), from, to);
                if (!sameRgb(mapped, fmt.foreground().color())) {
                    fmt.setForeground(mapped);
                    changed = true;
                }
            }
            if (fmt.background().style() != Qt::NoBrush) {
                const QColor mapped = mappedPaletteColor(fmt.background().color(), from, to);
                if (!sameRgb(mapped, fmt.background().color())) {
                    fmt.setBackground(mapped);
                    changed = true;
                }
            }
            if (changed)
                edits.append({frag.position(), frag.length(), fmt});
        }
    }
    if (edits.isEmpty())
        return;

    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (const Edit &e : edits) {
        cursor.setPosition(e.pos);
        cursor.setPosition(e.pos + e.length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(e.fmt);
    }
    cursor.endEditBlock();
}

QColor color256(int index)
{
    index = qBound(0, index, 255);
    if (index < 8)
        return paletteColor(index, false);
    if (index < 16)
        return paletteColor(index - 8, true);
    if (index >= 232) {
        const int gray = 8 + (index - 232) * 10;
        return QColor(gray, gray, gray);
    }
    index -= 16;
    const int r = index / 36;
    const int g = (index / 6) % 6;
    const int b = index % 6;
    auto level = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
    return QColor(level(r), level(g), level(b));
}

bool formatIsPlain(const QTextCharFormat &fmt)
{
    if (fmt.fontItalic() || fmt.fontUnderline() || fmt.fontWeight() != QFont::Normal)
        return false;
    if (fmt.background().style() != Qt::NoBrush)
        return false;
    if (fmt.foreground().style() == Qt::NoBrush)
        return true;
    return sameRgb(fmt.foreground().color(), s_palette->defaultFg);
}

QTextCharFormat tokenFormat(int ansiIndex, bool bright, bool bold)
{
    QTextCharFormat fmt;
    fmt.setForeground(paletteColor(ansiIndex, bright));
    if (bold)
        fmt.setFontWeight(QFont::Bold);
    return fmt;
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

bool spanIsPlain(const QTextBlock &block, int startInBlock, int length)
{
    const int endInBlock = startInBlock + length;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment frag = it.fragment();
        const int from = frag.position() - block.position();
        const int to = from + frag.length();
        if (to <= startInBlock || from >= endInBlock)
            continue;
        if (!formatIsPlain(frag.charFormat()))
            return false;
    }
    return length > 0;
}

void paintToken(QTextCursor &cursor, const QTextBlock &block, int startInBlock, int length,
                const QTextCharFormat &fmt)
{
    if (!spanIsPlain(block, startInBlock, length))
        return;
    const int pos = block.position() + startInBlock;
    cursor.setPosition(pos);
    cursor.setPosition(pos + length, QTextCursor::KeepAnchor);
    cursor.setCharFormat(fmt);
}

// 设备若已用 ANSI 上色则保持原样；否则按 Ubuntu ip -c 给地址、网卡和链路状态补色。
void highlightBlock(QTextCursor &cursor, const QTextBlock &block)
{
    const QString text = block.text();
    if (text.isEmpty() || text.size() > 8192)
        return;

    const bool dark = s_palette == &kDarkPalette;
    const QTextCharFormat ipv4Fmt = tokenFormat(5, false, dark);
    const QTextCharFormat ipv6Fmt = tokenFormat(4, dark, dark);
    const QTextCharFormat macFmt = tokenFormat(3, false, dark);
    const QTextCharFormat ifFmt = tokenFormat(6, false, dark);
    const QTextCharFormat upFmt = tokenFormat(2, false, dark);
    const QTextCharFormat downFmt = tokenFormat(1, false, dark);

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
                       "virbr\\d+|veth[0-9A-Fa-f]+|docker\\d*|br\\d+|tun\\d+|tap\\d+|wg\\d+|lo"
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

    auto matches = ipv6Re.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!isIpv6Address(m.captured()) || !take(m.capturedStart(), m.capturedLength()))
            continue;
        paintToken(cursor, block, m.capturedStart(), m.capturedLength(), ipv6Fmt);
    }
    matches = ipv4Re.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!take(m.capturedStart(), m.capturedLength()))
            continue;
        paintToken(cursor, block, m.capturedStart(), m.capturedLength(), ipv4Fmt);
    }
    matches = macRe.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!take(m.capturedStart(), m.capturedLength()))
            continue;
        paintToken(cursor, block, m.capturedStart(), m.capturedLength(), macFmt);
    }
    matches = ifRe.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!take(m.capturedStart(), m.capturedLength()))
            continue;
        paintToken(cursor, block, m.capturedStart(), m.capturedLength(), ifFmt);
    }
    matches = stateRe.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch m = matches.next();
        if (!take(m.capturedStart(), m.capturedLength()))
            continue;
        const bool down = m.captured(1) == QLatin1String("DOWN");
        paintToken(cursor, block, m.capturedStart(), m.capturedLength(), down ? downFmt : upFmt);
    }
}

void highlightPlainText(QTextDocument *doc, int start, int end)
{
    if (!doc || start >= end)
        return;
    const QTextBlock first = doc->findBlock(start);
    const QTextBlock last = doc->findBlock(end - 1);
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (QTextBlock block = first; block.isValid(); block = block.next()) {
        highlightBlock(cursor, block);
        if (block == last)
            break;
    }
    cursor.endEditBlock();
}
} // namespace

TerminalView::TerminalView(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setMaximumBlockCount(5000); // 限制内存占用
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);

    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // Ctrl+滚轮每档 +1pt，行距随字号一起变。默认按放大两档，对齐手感合适的字号与行距。
    const int systemPt = mono.pointSize();
    const int basePt = qMax(9, systemPt > 0 ? systemPt : 9);
    mono.setPointSize(basePt + 2);
    setFont(mono);

    m_cursor = QTextCursor(document());
    applyTheme(true); // 默认深色

    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    connect(m_flushTimer, &QTimer::timeout, this, &TerminalView::feedIncoming);

    m_sizeTimer = new QTimer(this);
    m_sizeTimer->setSingleShot(true);
    connect(m_sizeTimer, &QTimer::timeout, this, &TerminalView::reportTerminalSize);

    m_syncClock.start(); // 同步渲染预算的时间基准
}

void TerminalView::applyTheme(bool dark)
{
    // 每个文档各自记住"我是按哪套配色渲染的"：串口与 SSH 两个视图同时存在，
    // 只靠全局 s_palette 会让第二个视图跳过重着色。
    const TerminalPalette *previous = m_docDark ? &kDarkPalette : &kLightPalette;
    const TerminalPalette *next = dark ? &kDarkPalette : &kLightPalette;
    s_palette = next;
    m_docDark = dark;

    if (previous != next) {
        const int cursorPos = m_cursor.position();
        recolorDocument(document(), previous, next);
        const int maxPos = qMax(0, document()->characterCount() - 1);
        m_cursor.setPosition(qBound(0, cursorPos, maxPos));
    }

    setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background-color: %1; color: %2;"
        " border: none; selection-background-color: %3; }")
                      .arg(s_palette->background.name(),
                           s_palette->defaultFg.name(),
                           s_palette->selectionBg.name()));

    m_fmt = QTextCharFormat();
    m_fmt.setForeground(s_palette->defaultFg);
    m_echoFmt = QTextCharFormat();
    m_echoFmt.setForeground(s_palette->echo);
    m_infoColor = s_palette->info;
}

// ---------------------------------------------------------------- 接收

void TerminalView::receiveData(const QByteArray &data)
{
    if (data.isEmpty())
        return;

    // 没有积压时的小包（键入回显、一条命令的输出）当场渲染：零延迟，
    // 调用完就能读到文档内容。超出时间窗预算后自动转入合并节奏。
    if (pendingBytes() == 0 && data.size() <= kSyncChunkBytes
        && takeSyncBudget(data.size())) {
        m_inBuffer.clear(); // 顺带回收已消费完的空壳
        m_inUsed = 0;
        renderBytes(data);
        return;
    }

    m_inBuffer += data;
    trimHiddenBacklog();
    scheduleFlush();
}

bool TerminalView::takeSyncBudget(int size)
{
    if (!m_syncClock.isValid())
        m_syncClock.start();
    const qint64 now = m_syncClock.elapsed();
    if (now - m_syncWindowStart >= kSyncWindowMs) {
        m_syncWindowStart = now; // 开新窗口
        m_syncBytes = 0;
    }
    if (m_syncBytes + size > kSyncWindowBytes)
        return false;
    m_syncBytes += size;
    return true;
}

void TerminalView::trimHiddenBacklog()
{
    // 后台（不可见）会话没必要无限制攒数据：文档本来就只留最后几千行，
    // 中间部分永远看不到，按行边界丢掉头部，内存与 CPU 都有上界。
    if (isVisible() || pendingBytes() <= kHiddenBacklogLimit)
        return;
    int keep = m_inBuffer.size() - kHiddenBacklogLimit / 2;
    if (keep <= m_inUsed)
        return;
    const int nl = m_inBuffer.indexOf('\n', keep);
    if (nl > m_inUsed)
        keep = nl + 1;
    m_inBuffer.remove(0, keep);
    m_inUsed = 0;
    // 截断点可能正好落在转义序列中间，状态机复位才不会把后续内容当残帧吞掉
    m_state = StateNormal;
    m_csiParams.clear();
}

void TerminalView::scheduleFlush()
{
    if (pendingBytes() <= 0)
        return;
    int interval = isVisible() ? kFlushVisibleMs : kFlushHiddenMs;
    if (pendingBytes() >= kImmediateBytes)
        interval = kZeroMs; // 尽快续上下一片，但每片之间都让事件循环喘气
    if (m_flushTimer->isActive()) {
        if (m_flushTimer->interval() <= interval)
            return; // 已经排在更及时的节奏上
        m_flushTimer->stop();
    }
    m_flushTimer->start(interval);
}

void TerminalView::flushIncoming()
{
    if (m_flushTimer->isActive())
        m_flushTimer->stop();
    // 显式同步点：已到达的数据全部落地，之后的插入才不会插到它们中间
    while (pendingBytes() > 0)
        renderNextSlice();
    m_inBuffer.clear();
    m_inUsed = 0;
}

void TerminalView::feedIncoming()
{
    if (pendingBytes() <= 0) {
        m_inBuffer.clear();
        m_inUsed = 0;
        return;
    }
    renderNextSlice();
    if (pendingBytes() > 0)
        scheduleFlush(); // 还有剩余，下一帧继续
}

void TerminalView::renderNextSlice()
{
    // 分片渲染：即便远端在 cat 大文件，单帧耗时也被限定住，输入照样响应
    const int n = qMin(pendingBytes(), kFeedSliceBytes);
    const QByteArray chunk = m_inBuffer.mid(m_inUsed, n);
    m_inUsed += n;
    if (m_inUsed >= kImmediateBytes) {
        // 头部已消费掉的部分及时回收，缓冲区不会只涨不消
        m_inBuffer.remove(0, m_inUsed);
        m_inUsed = 0;
    }
    renderBytes(chunk);
}

void TerminalView::renderBytes(const QByteArray &chunk)
{
    // UTF-8 解码，QStringDecoder 保留跨包的多字节中间状态
    const QString text = m_decoder.decode(chunk);
    for (const QChar c : text)
        feedChar(c);
    flushPending();
    ensureCursorVisible();
}

void TerminalView::appendSystemMessage(const QString &text)
{
    flushIncoming(); // 先把已到达的远端数据落到文档，提示才不会插到中间
    flushPending();
    QTextCharFormat fmt;
    fmt.setForeground(m_infoColor);
    // 远端最后一行常常不带换行（ssh 的 "password:" 提示就是如此），直接插会把
    // 系统提示拼在那一行后面，看着像被截断的乱码。
    // 判据是「光标在不在某个 block 开头」——不在开头说明上一行还欠一个换行。
    // 注意不能用 atBlockEnd()：末行没有换行符时光标恰好就在该 block 末尾，
    // 那个判据会把这种情况误认为"已经在新一行上"。
    if (!m_cursor.atBlockStart())
        m_cursor.insertText(QStringLiteral("\n"), fmt);
    m_cursor.insertText(text, fmt);
    m_cursor.insertText(QStringLiteral("\n"), fmt);
    ensureCursorVisible();
}

void TerminalView::feedChar(QChar c)
{
    const char ch = c.toLatin1();
    switch (m_state) {
    case StateNormal:
        if (c == QChar(0x1B)) {
            flushPending();
            m_state = StateEsc;
        } else if (c == QLatin1Char('\n')) {
            flushPending();
            lineFeed();
        } else if (c == QLatin1Char('\r')) {
            flushPending();
            m_cursor.movePosition(QTextCursor::StartOfBlock);
            m_overwrite = true; // 行首重绘走覆盖（进度条 / 提示符刷新）
        } else if (c == QChar(0x08)) { // backspace: 光标左移（设备行编辑配合）
            flushPending();
            m_cursor.movePosition(QTextCursor::Left);
            m_overwrite = true;
        } else if (c == QLatin1Char('\t')) {
            flushPending();
            m_cursor.insertText(QStringLiteral("\t"), m_fmt);
        } else if (ch == 0x07 || ch == 0x00) { // BEL / NUL
            // 忽略
        } else if (ch >= 0x20) {
            m_pending += c; // 连续普通文本攒起来批量插入
        } else if (c == QChar(0x0B) || c == QChar(0x0C)) { // VT / FF 当换行
            flushPending();
            lineFeed();
        }
        break;

    case StateEsc:
        if (c == QLatin1Char('[')) {
            m_csiParams.clear();
            m_state = StateCsi;
        } else if (c == QLatin1Char(']')) {
            m_state = StateOsc; // OSC: 吞到 BEL / ST
        } else if (c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('#')) {
            m_state = StateCharset; // 字符集选择: 吞掉下一个字符
        } else if (c == QLatin1Char('c')) {
            resetScreen(); // RIS 整屏复位（clear / reset）
            m_state = StateNormal;
        } else {
            m_state = StateNormal; // 两字符转义（ESC 7 等）忽略
        }
        break;

    case StateCsi:
        if (c.isDigit() || c == QLatin1Char(';') || c == QLatin1Char('?') ||
            c == QLatin1Char('<') || c == QLatin1Char('=') || c == QLatin1Char('>')) {
            m_csiParams += c;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            handleCsi(m_csiParams, ch);
            m_state = StateNormal;
        } // 0x20-0x2F 中间字节忽略
        break;

    case StateOsc:
        if (c == QChar(0x07)) {
            m_state = StateNormal;
        } else if (c == QChar(0x1B)) {
            m_state = StateEsc; // ST = ESC \，回到 Esc 状态吃掉 '\\'
        }
        break;

    case StateCharset:
        m_state = StateNormal;
        break;
    }
}

void TerminalView::flushPending()
{
    if (m_pending.isEmpty())
        return;
    const int start = m_cursor.position();
    const bool plain = formatIsPlain(m_fmt);
    if (m_overwrite && !m_cursor.atBlockEnd()) {
        // 覆盖模式: 替换光标处已有字符（终端语义），超出行尾的部分追加
        const int remaining =
            qMax(0, m_cursor.block().length() - 1 - m_cursor.positionInBlock());
        const int n = qMin(m_pending.size(), remaining);
        if (n > 0) {
            QTextCursor c = m_cursor;
            c.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, n);
            c.insertText(m_pending.left(n), m_fmt);
            m_cursor = c;
        }
        if (n < m_pending.size())
            m_cursor.insertText(m_pending.mid(n), m_fmt);
    } else {
        m_cursor.insertText(m_pending, m_fmt);
    }
    const int end = m_cursor.position();
    m_pending.clear();
    if (plain && end > start) {
        highlightPlainText(document(), start, end);
        const int maxPos = qMax(0, document()->characterCount() - 1);
        m_cursor.setPosition(qBound(0, end, maxPos));
    }
}

// ---------------------------------------------------------------- CSI 处理

void TerminalView::handleCsi(const QString &params, char finalByte)
{
    if (params.contains(QLatin1Char('?')) || params.contains(QLatin1Char('<')) ||
        params.contains(QLatin1Char('=')) || params.contains(QLatin1Char('>')))
        return; // 私有模式（ESC[?25l 光标隐藏等）忽略

    const QStringList parts = params.split(QLatin1Char(';'));
    auto argAt = [&parts](int i, int def) {
        if (i >= parts.size())
            return def;
        const QString &s = parts.at(i);
        if (s.isEmpty())
            return def;
        bool ok = false;
        const int v = s.toInt(&ok);
        return ok ? v : def;
    };

    switch (finalByte) {
    case 'm': {
        QList<int> ps;
        for (const QString &s : parts)
            ps << (s.isEmpty() ? 0 : s.toInt());
        applySgr(ps);
        break;
    }
    case 'A': moveCursorOp(QTextCursor::Up, qMax(1, argAt(0, 1))); break;
    case 'B': moveCursorOp(QTextCursor::Down, qMax(1, argAt(0, 1))); break;
    case 'C':
        flushPending();
        m_overwrite = false;
        for (int i = 0; i < qMax(1, argAt(0, 1)); ++i)
            m_cursor.movePosition(QTextCursor::Right);
        break;
    case 'D': moveCursorOp(QTextCursor::Left, qMax(1, argAt(0, 1))); m_overwrite = true; break;
    case 'H':
    case 'f':
        setCursorRowCol(qMax(1, argAt(0, 1)), qMax(1, argAt(1, 1)));
        m_overwrite = false;
        break;
    case 'G':
        setCursorColumn(qMax(1, argAt(0, 1)));
        m_overwrite = false;
        break;
    case 'K': eraseLine(argAt(0, 0)); m_overwrite = false; break;
    case 'J': eraseDisplay(argAt(0, 0)); m_overwrite = false; break;
    default:
        break; // 滚动/插入/删除行等（L M P @ S T X）暂不支持
    }
}

void TerminalView::applySgr(const QList<int> &params)
{
    QList<int> ps = params;
    if (ps.isEmpty())
        ps << 0;
    for (int i = 0; i < ps.size(); ++i) {
        const int p = ps.at(i);
        if (p == 38 || p == 48) { // 256 色 / 真彩色: 38;5;n 或 38;2;r;g;b
            const bool foreground = p == 38;
            if (i + 1 < ps.size()) {
                const int mode = ps.at(i + 1);
                if (mode == 5 && i + 2 < ps.size()) {
                    const QColor c = color256(ps.at(i + 2));
                    if (foreground)
                        m_fmt.setForeground(c);
                    else
                        m_fmt.setBackground(c);
                    i += 2;
                } else if (mode == 2 && i + 4 < ps.size()) {
                    const QColor c(qBound(0, ps.at(i + 2), 255),
                                   qBound(0, ps.at(i + 3), 255),
                                   qBound(0, ps.at(i + 4), 255));
                    if (foreground)
                        m_fmt.setForeground(c);
                    else
                        m_fmt.setBackground(c);
                    i += 4;
                } else if (mode == 2 || mode == 5) {
                    ++i; // 参数不完整，避免把 2/5 当成淡色或闪烁
                }
            }
            continue;
        }
        if (p == 0) {
            m_fmt = QTextCharFormat();
            m_fmt.setForeground(s_palette->defaultFg);
        } else if (p == 1) {
            m_fmt.setFontWeight(QFont::Bold);
        } else if (p == 2) {
            m_fmt.setFontWeight(QFont::Normal);
        } else if (p == 3) {
            m_fmt.setFontItalic(true);
        } else if (p == 4) {
            m_fmt.setFontUnderline(true);
        } else if (p == 7 || p == 27) { // 反显: 前景/背景互换
            const QBrush fg = m_fmt.foreground().style() != Qt::NoBrush
                                  ? m_fmt.foreground() : QBrush(s_palette->defaultFg);
            const QBrush bg = m_fmt.background().style() != Qt::NoBrush
                                  ? m_fmt.background() : QBrush(s_palette->background);
            m_fmt.setForeground(bg);
            m_fmt.setBackground(fg);
        } else if (p == 22) {
            m_fmt.setFontWeight(QFont::Normal);
        } else if (p == 23) {
            m_fmt.setFontItalic(false);
        } else if (p == 24) {
            m_fmt.setFontUnderline(false);
        } else if (p >= 30 && p <= 37) {
            m_fmt.setForeground(paletteColor(p - 30, false));
        } else if (p == 39) {
            m_fmt.setForeground(s_palette->defaultFg);
        } else if (p >= 40 && p <= 47) {
            m_fmt.setBackground(paletteColor(p - 40, false));
        } else if (p == 49) {
            m_fmt.clearBackground();
        } else if (p >= 90 && p <= 97) {
            m_fmt.setForeground(paletteColor(p - 90, true));
        } else if (p >= 100 && p <= 107) {
            m_fmt.setBackground(paletteColor(p - 100, true));
        }
    }
}

void TerminalView::moveCursorOp(QTextCursor::MoveOperation op, int n)
{
    flushPending();
    for (int i = 0; i < n; ++i)
        m_cursor.movePosition(op);
}

void TerminalView::setCursorRowCol(int row, int col)
{
    flushPending();
    QTextBlock b = document()->firstBlock();
    for (int i = 1; i < row && b.isValid(); ++i)
        b = b.next();
    if (!b.isValid())
        b = document()->lastBlock();
    if (!b.isValid())
        return;
    const int pos = b.position() + qMin(col - 1, qMax(0, b.length() - 1));
    m_cursor.setPosition(pos);
}

void TerminalView::setCursorColumn(int col)
{
    flushPending();
    const QTextBlock b = m_cursor.block();
    if (!b.isValid())
        return;
    m_cursor.setPosition(b.position() + qMin(qMax(1, col) - 1, qMax(0, b.length() - 1)));
}

// LF（\n）在终端里是「光标下移一行」，列位置保留并夹到目标行行尾；只有下面确实
// 没有行了才新起一行（对应真实终端在屏幕底边滚动）。
// 之前按「插入换行符」实现：conhost 的整屏重绘发的是 ESC[H 之后每行 EL+CR+LF
// （cls、以及远端 clear 触发重绘时都是这套），于是一整屏空行被插进文档中间，
// 旧内容被顶到空行下面 —— 用户看到的就是「clear 之后留下一大片空白没清掉」。
void TerminalView::lineFeed()
{
    const QTextBlock next = m_cursor.block().next();
    if (next.isValid()) {
        const int col = qMin(m_cursor.positionInBlock(), qMax(0, next.length() - 1));
        m_cursor.setPosition(next.position() + col);
        m_overwrite = true; // 落在已有行上：后续字符替换该行内容，与终端一致
        return;
    }
    if (!m_cursor.atBlockEnd())
        m_cursor.movePosition(QTextCursor::EndOfBlock); // \r\n 不该多出空行
    m_overwrite = false;
    m_cursor.insertText(QStringLiteral("\n"), m_fmt);
}

void TerminalView::eraseLine(int mode)
{
    flushPending();
    if (mode == 0) { // 光标到行尾
        QTextCursor c = m_cursor;
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    } else if (mode == 1) { // 行首到光标
        QTextCursor c = m_cursor;
        c.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    } else if (mode == 2) { // 整行
        QTextCursor c(m_cursor.block());
        c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
}

void TerminalView::eraseDisplay(int mode)
{
    flushPending();
    if (mode == 2) { // 清屏
        m_cursor.select(QTextCursor::Document);
        m_cursor.removeSelectedText();
    } else if (mode == 0) { // 光标到文档尾
        QTextCursor c = m_cursor;
        c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    } else if (mode == 1) { // 文档头到光标
        QTextCursor c = m_cursor;
        c.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        m_cursor = c;
        m_cursor.clearSelection();
    } else if (mode == 3) {
        // 清回滚区。本控件里文档同时兼作回滚区，分不清「屏幕」从哪一行开始，
        // 只能整体丢弃 —— 和 mode 2 一样。conhost 的 cls 重绘正是「逐行 EL+CR+LF
        // 刷出一屏空白，再补一个 ESC[3J」。不接住 3J 的话那一屏空白行就留在
        // 文档里，旧内容被顶在下面，看着像 clear 没清干净。
        m_cursor.select(QTextCursor::Document);
        m_cursor.removeSelectedText();
    }
}

// RIS（ESC c）整屏复位。BusyBox 的 clear 在认出 xterm 时发的是 ESC c 而不是
// CSI 2J，reset 更是只有这一种写法；不实现的话这两条命令按下去屏幕一动不动。
void TerminalView::resetScreen()
{
    flushPending();
    m_fmt = QTextCharFormat();
    m_fmt.setForeground(s_palette->defaultFg); // 回到初始配色，等价于一次 SGR 0
    m_overwrite = false;
    m_cursor.select(QTextCursor::Document);
    m_cursor.removeSelectedText();
    setCursorRowCol(1, 1);
}

// ---------------------------------------------------------------- 键盘

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    // 键盘处理会读当前行（补全取词）、也会往文档里插回显字符，
    // 必须先把攒着的远端数据落地，顺序才对。
    flushIncoming();

    // Tab 由补全引擎接管，其他键一律打断当前的补全 Cycle
    if (event->key() != Qt::Key_Tab)
        resetCompletionCycle();

    const QString text = event->text();

    // Ctrl+C: 有选区时复制，否则发 ETX
    if (event->modifiers() & Qt::ControlModifier) {
        const int key = event->key();
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            if (key == Qt::Key_C && textCursor().hasSelection()) {
                copy();
                return;
            }
            if (key == Qt::Key_V || key == Qt::Key_Y) { // 粘贴到串口
                const QString clip = QGuiApplication::clipboard()->text();
                if (!clip.isEmpty()) {
                    if (m_localEcho)
                        m_cursor.insertText(clip, m_echoFmt);
                    emit sendData(clip.toUtf8());
                }
                return;
            }
            emit sendData(QByteArray(1, char(1 + key - Qt::Key_A))); // ^C ^D ^L ^U ...
            return;
        }
        if (key == Qt::Key_Space) {
            emit sendData(QByteArray(1, '\0'));
            return;
        }
    }
    if (event->modifiers() & Qt::ShiftModifier && event->key() == Qt::Key_Insert) {
        const QString clip = QGuiApplication::clipboard()->text();
        if (!clip.isEmpty())
            emit sendData(clip.toUtf8());
        return;
    }

    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_localEcho)
            m_cursor.insertText(QStringLiteral("\n"), m_echoFmt);
        emit sendData(QByteArrayLiteral("\n"));
        return;
    case Qt::Key_Backspace:
        if (m_localEcho)
            eraseCharBeforeCursor();
        emit sendData(QByteArray(1, '\x7f')); // DEL，tty 默认擦除键
        return;
    case Qt::Key_Tab:
        handleTabCompletion();
        return;
    case Qt::Key_Escape:
        emit sendData(QByteArray(1, '\x1b'));
        return;
    case Qt::Key_Up:
        emit sendData(QByteArrayLiteral("\x1b[A"));
        return;
    case Qt::Key_Down:
        emit sendData(QByteArrayLiteral("\x1b[B"));
        return;
    case Qt::Key_Right:
        emit sendData(QByteArrayLiteral("\x1b[C"));
        return;
    case Qt::Key_Left:
        emit sendData(QByteArrayLiteral("\x1b[D"));
        return;
    case Qt::Key_Home:
        emit sendData(QByteArrayLiteral("\x1b[H"));
        return;
    case Qt::Key_End:
        emit sendData(QByteArrayLiteral("\x1b[F"));
        return;
    case Qt::Key_Delete:
        emit sendData(QByteArrayLiteral("\x1b[3~"));
        return;
    case Qt::Key_PageUp:
        emit sendData(QByteArrayLiteral("\x1b[5~"));
        return;
    case Qt::Key_PageDown:
        emit sendData(QByteArrayLiteral("\x1b[6~"));
        return;
    default:
        break;
    }

    if (!text.isEmpty() && text.at(0).isPrint()) {
        if (m_localEcho)
            m_cursor.insertText(text, m_echoFmt);
        emit sendData(text.toUtf8());
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void TerminalView::eraseCharBeforeCursor()
{
    if (m_cursor.position() <= 0)
        return;
    QTextCursor c = m_cursor;
    c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor);
    c.removeSelectedText();
}

// ---------------------------------------------------------------- Tab 补全

void TerminalView::resetCompletionCycle()
{
    m_cycle.clear();
    m_cycleStart = -1;
    m_cycleEnd = -1;
    m_cycleOriginal.clear();
    m_cycleState = -1;
}

void TerminalView::handleTabCompletion()
{
    if (!m_completion) {
        emit sendData(QByteArray(1, '\t'));
        return;
    }

    // 当前词：从 m_cursor 位置向前找到第一个空白字符
    const QTextBlock block = m_cursor.block();
    const QString lineText = block.text();
    const int cursorInBlock = m_cursor.positionInBlock();
    int wordStart = cursorInBlock;
    while (wordStart > 0 && !lineText.at(wordStart - 1).isSpace())
        --wordStart;

    // 已经在 Cycle 过程中、且词边界没变 → 推进到下一项
    if (!m_cycle.isEmpty() && wordStart == m_cycleStart && cursorInBlock == m_cycleEnd) {
        advanceCompletionCycle();
        return;
    }

    // 全新补全
    const QString prefix = lineText.mid(wordStart, cursorInBlock - wordStart);
    m_cycle = m_completion->complete(prefix);
    m_cycleStart = wordStart;
    m_cycleOriginal = prefix;
    m_cycleState = -1;

    if (m_cycle.isEmpty()) {
        m_cycleEnd = cursorInBlock;
        return;
    }
    if (m_cycle.size() == 1) {
        // 唯一匹配：直接补齐即可（一次到位，不进 Cycle）
        const QString &only = m_cycle.first();
        const QString tail = only.mid(prefix.size());
        if (!tail.isEmpty()) {
            m_cursor.insertText(tail, m_echoFmt);
            emit sendData(tail.toUtf8());
        }
        m_cycleEnd = wordStart + only.size();
        m_cycle.clear();
        return;
    }

    // 多匹配：先尝试补到 LCP
    const QString lcp = CompletionEngine::longestCommonPrefix(m_cycle);
    if (lcp.size() > prefix.size()) {
        const QString tail = lcp.mid(prefix.size());
        m_cursor.insertText(tail, m_echoFmt);
        emit sendData(tail.toUtf8());
        m_cycleEnd = wordStart + lcp.size();
        m_cycleOriginal = lcp; // LCP 成为新的 wrap 回退点
        return;
    }
    // LCP == prefix，立即推进到 cycle[0]
    m_cycleEnd = wordStart + prefix.size();
    advanceCompletionCycle();
}

void TerminalView::advanceCompletionCycle()
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
    const int oldLen = m_cycleEnd - m_cycleStart;
    const int newLen = target.size();

    // 1) 删除尾部多余字符（按字符数整体替换）
    if (oldLen > 0) {
        QTextCursor c = m_cursor;
        c.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, oldLen);
        c.removeSelectedText();
        m_cursor = c;
        emit sendData(QString(oldLen, QChar(0x7f)).toUtf8());
    }
    // 2) 写入新内容
    if (newLen > 0) {
        m_cursor.insertText(target, m_echoFmt);
        emit sendData(target.toUtf8());
    }
    m_cycleEnd = m_cycleStart + newLen;
    m_cycleState = nextState;
}

// ---------------------------------------------------------------- 窗口尺寸
//
// 等宽字体下由视口换算行列数，供伪终端设置窗口大小：远端 shell 按这个宽度
// 折行、全屏程序按这个高度重绘，报少了会串行，报多了右边留白。

int TerminalView::terminalColumns() const
{
    const QFontMetricsF fm(font());
    const double cell = fm.horizontalAdvance(QLatin1Char('M'));
    if (cell <= 0.0)
        return 80;
    const double usable = viewport()->width() - 2.0 * document()->documentMargin();
    return qMax(20, int(usable / cell));
}

int TerminalView::terminalRows() const
{
    const QFontMetricsF fm(font());
    const double line = fm.lineSpacing();
    if (line <= 0.0)
        return 24;
    const double usable = viewport()->height() - 2.0 * document()->documentMargin();
    return qMax(4, int(usable / line));
}

void TerminalView::reportTerminalSize()
{
    const int cols = terminalColumns();
    const int rows = terminalRows();
    if (cols == m_reportedCols && rows == m_reportedRows)
        return;
    m_reportedCols = cols;
    m_reportedRows = rows;
    emit terminalSizeChanged(cols, rows);
}

void TerminalView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    // 去抖：拖动窗口时每像素都重设远端尺寸会让它反复全量重绘
    if (m_sizeTimer)
        m_sizeTimer->start(120);
}

void TerminalView::showEvent(QShowEvent *event)
{
    QPlainTextEdit::showEvent(event);
    reportTerminalSize();
    scheduleFlush(); // 后台期间攒下的输出，切回来时立刻补上
}

bool TerminalView::event(QEvent *event)
{
    if (event->type() == QEvent::FontChange && m_sizeTimer)
        m_sizeTimer->start(120); // Ctrl+滚轮改字号后行列数会变
    return QPlainTextEdit::event(event);
}
