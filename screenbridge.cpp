#include "screenbridge.h"

#include <QColor>
#include <QRegularExpression>

namespace {

constexpr int kDefaultFindWindow = 400; // find 扫描窗（与 read 上限一致）
constexpr int kDefaultFindHits = 50;
constexpr int kMaxFindHits = 200;

// 与 lineToPlain（screenview.cpp）同款的纯文本：跳过宽字符续格、去尾空白
QString lineText(const TermLine *line)
{
    QString text;
    if (!line)
        return text;
    for (const TermCell &cell : line->cells) {
        if (cell.flags & TermScreen::FlagWideCont)
            continue;
        const char32_t cp = cell.cp ? cell.cp : U' ';
        text.append(QString::fromUcs4(&cp, 1));
    }
    while (text.size() > 1 && text.endsWith(QLatin1Char(' ')))
        text.chop(1);
    return text;
}

bool sameColor(const TermColor &a, const TermColor &b)
{
    return a.kind == b.kind && a.index == b.index && a.rgb == b.rgb;
}

} // namespace

QJsonObject ScreenBridge::snapshot(const TermScreen &screen, int maxLines)
{
    maxLines = qBound(1, maxLines, 400);
    const bool alt = screen.altScreen();
    const qint64 dropped = alt ? 0 : screen.droppedLines();
    const qint64 total = alt ? screen.rows()
                             : dropped + screen.historySize() + screen.rows();
    const qint64 begin = qMax<qint64>(dropped, total - maxLines);

    QJsonArray lines;
    for (qint64 abs = begin; abs < total; ++abs) {
        const TermLine *line = screen.lineAbsolute(int(abs - dropped));
        QJsonObject row;
        row.insert(QStringLiteral("abs"), double(abs));
        row.insert(QStringLiteral("text"), lineText(line));
        if (line != nullptr && line->wrapped)
            row.insert(QStringLiteral("wrapped"), true);

        // 非默认样式段：fg/bg 任一非默认或 attr 非 0 才记录，颜色按当前
        // 主题解析成 #rrggbb（AI 不用懂调色板下标）。行末补一格哨兵（默认
        // 样式）收尾最后一段
        QJsonArray runs;
        int runStart = -1;
        TermColor runFg;
        TermColor runBg;
        quint8 runAttr = 0;
        const int cols = line != nullptr ? int(line->cells.size()) : 0;
        for (int col = 0; col <= cols; ++col) {
            const bool last = col == cols;
            const TermCell &cell = last ? TermCell{} : line->cells.at(size_t(col));
            const bool same = runStart >= 0 && !last
                              && sameColor(cell.fg, runFg)
                              && sameColor(cell.bg, runBg) && cell.attr == runAttr;
            if (same)
                continue;
            if (runStart >= 0 && (runFg.kind != TermColor::Default
                                  || runBg.kind != TermColor::Default
                                  || runAttr != 0)) {
                QJsonObject run;
                run.insert(QStringLiteral("start"), runStart);
                run.insert(QStringLiteral("end"), col - 1);
                TermCell probe;
                probe.fg = runFg;
                probe.bg = runBg;
                probe.attr = runAttr;
                if (runFg.kind != TermColor::Default)
                    run.insert(QStringLiteral("fg"), screen.foreground(probe).name());
                if (runBg.kind != TermColor::Default)
                    run.insert(QStringLiteral("bg"), screen.background(probe).name());
                if (runAttr & TermScreen::AttrBold)
                    run.insert(QStringLiteral("bold"), true);
                if (runAttr & TermScreen::AttrUnderline)
                    run.insert(QStringLiteral("underline"), true);
                if (runAttr & TermScreen::AttrInverse)
                    run.insert(QStringLiteral("inverse"), true);
                runs.append(run);
            }
            runStart = col;
            runFg = cell.fg;
            runBg = cell.bg;
            runAttr = cell.attr;
        }
        if (!runs.isEmpty())
            row.insert(QStringLiteral("runs"), runs);

        // OSC 8 链接段：连续同池下标的 cell 合并（M3.2 的 URI 池）
        QJsonArray links;
        int linkStart = -1;
        quint32 linkUri = TermCell::NoUri;
        for (int col = 0; col <= cols; ++col) {
            const bool last = col == cols;
            const quint32 uri = last ? TermCell::NoUri
                                     : line->cells.at(size_t(col)).uri;
            if (uri == linkUri && !last)
                continue;
            if (linkStart >= 0 && linkUri != TermCell::NoUri) {
                const QString target = screen.uriAt(linkUri);
                if (!target.isEmpty()) {
                    QJsonObject link;
                    link.insert(QStringLiteral("start"), linkStart);
                    link.insert(QStringLiteral("end"), col - 1);
                    link.insert(QStringLiteral("uri"), target);
                    links.append(link);
                }
            }
            linkStart = col;
            linkUri = uri;
        }
        if (!links.isEmpty())
            row.insert(QStringLiteral("links"), links);

        lines.append(row);
    }

    QJsonObject data;
    data.insert(QStringLiteral("cols"), screen.cols());
    data.insert(QStringLiteral("rows"), screen.rows());
    data.insert(QStringLiteral("alt_screen"), alt);
    data.insert(QStringLiteral("total_lines"), double(total));
    data.insert(QStringLiteral("start_abs"), double(begin));
    data.insert(QStringLiteral("lines"), lines);
    return data;
}

QJsonArray ScreenBridge::findMatches(const QString &text, qint64 startLine,
                                     const QString &pattern, bool regex,
                                     int maxHits, bool *badPattern)
{
    QJsonArray hits;
    if (badPattern != nullptr)
        *badPattern = false;
    if (pattern.isEmpty())
        return hits;

    QRegularExpression re;
    if (regex) {
        // 语义对齐 M3.3 的 Ctrl+F 查找：大小写不敏感
        re.setPattern(pattern);
        re.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) {
            if (badPattern != nullptr)
                *badPattern = true;
            return hits;
        }
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size() && hits.size() < maxHits; ++i) {
        const QString &line = lines.at(i);
        if (regex) {
            auto it = re.globalMatch(line);
            while (it.hasNext() && hits.size() < maxHits) {
                const QRegularExpressionMatch m = it.next();
                if (m.capturedLength() == 0)
                    continue; // 空匹配不算命中（M3.3 同款，防 "a*" 类误报）
                QJsonObject hit;
                hit.insert(QStringLiteral("abs"), double(startLine + i));
                hit.insert(QStringLiteral("col"), m.capturedStart());
                hit.insert(QStringLiteral("match"), m.captured());
                hit.insert(QStringLiteral("line"), line);
                hits.append(hit);
            }
        } else {
            int from = 0;
            while (hits.size() < maxHits) {
                const int col = line.indexOf(pattern, from, Qt::CaseInsensitive);
                if (col < 0)
                    break;
                QJsonObject hit;
                hit.insert(QStringLiteral("abs"), double(startLine + i));
                hit.insert(QStringLiteral("col"), col);
                hit.insert(QStringLiteral("match"), line.mid(col, pattern.size()));
                hit.insert(QStringLiteral("line"), line);
                hits.append(hit);
                from = col + 1;
            }
        }
    }
    return hits;
}

void ScreenBridge::registerTools(AgentBridge::Hooks *hooks)
{
    auto meta = [hooks](const char *name, const QString &desc, QJsonObject params) {
        const QString full = QString::fromLatin1(name);
        QJsonObject m;
        m.insert(QStringLiteral("name"), full);
        m.insert(QStringLiteral("description"), desc);
        m.insert(QStringLiteral("params"), params);
        hooks->toolMeta.insert(full, m);
    };

    // —— xterm.screen.snapshot：结构化快照（字符/FG/BG/URI，验收「URI 可见」）——
    hooks->tools.insert(QStringLiteral("xterm.screen.snapshot"),
                        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            if (!m_hooks.screen)
                                return {false, false, {},
                                        QStringLiteral("Screen 后端未挂载")};
                            const TermScreen *screen = m_hooks.screen();
                            if (screen == nullptr)
                                return {false, false, {},
                                        QStringLiteral("当前没有活动终端视图")};
                            const int maxLines = call.params
                                                     .value(QStringLiteral("max_lines"))
                                                     .toInt(screen->rows());
                            return {true, false, snapshot(*screen, maxLines), QString()};
                        });
    meta("xterm.screen.snapshot",
         QStringLiteral("屏幕尾部 N 行结构化快照：每行 text + 非默认样式段 runs（解析后的"
                        "#rrggbb 色/bold/underline/inverse）+ OSC 8 链接段 links；"
                        "行号为绝对行号（与 wait 锚点同源）"),
         [] {
             QJsonObject params;
             QJsonObject maxLines;
             maxLines.insert(QStringLiteral("type"), QStringLiteral("integer"));
             maxLines.insert(QStringLiteral("description"),
                             QStringLiteral("返回尾部行数，默认当前屏行数，上限 400"));
             params.insert(QStringLiteral("max_lines"), maxLines);
             return params;
         }());

    // —— xterm.screen.find：滚动历史尾部扫描（子串/正则，语义对齐 Ctrl+F）——
    hooks->tools.insert(QStringLiteral("xterm.screen.find"),
                        [this](const AgentBridge::ToolCall &call) -> AgentBridge::ToolResult {
                            if (!m_hooks.lineCount || !m_hooks.textFrom)
                                return {false, false, {},
                                        QStringLiteral("Screen 后端未挂载")};
                            const QString pattern =
                                call.params.value(QStringLiteral("pattern")).toString();
                            if (pattern.isEmpty())
                                return {false, false, {},
                                        QStringLiteral("find 需要 pattern")};
                            const bool regex =
                                call.params.value(QStringLiteral("regex")).toBool(false);
                            const int window = qBound(
                                1, call.params.value(QStringLiteral("max_lines"))
                                       .toInt(kDefaultFindWindow),
                                kDefaultFindWindow);
                            const int maxHits = qBound(
                                1, call.params.value(QStringLiteral("max_hits"))
                                       .toInt(kDefaultFindHits),
                                kMaxFindHits);

                            const qint64 total = m_hooks.lineCount();
                            if (total <= 0) {
                                QJsonObject data;
                                data.insert(QStringLiteral("pattern"), pattern);
                                data.insert(QStringLiteral("regex"), regex);
                                data.insert(QStringLiteral("hits"), QJsonArray());
                                data.insert(QStringLiteral("truncated"), false);
                                return {true, false, data, QString()};
                            }
                            // 扫描窗从 total-window 起；textFrom 内部对齐同一窗口
                            const qint64 from = qMax<qint64>(0, total - window);
                            const QString text = m_hooks.textFrom(from, window);
                            bool bad = false;
                            const QJsonArray hits =
                                findMatches(text, from, pattern, regex, maxHits, &bad);
                            if (bad)
                                return {false, false, {},
                                        QStringLiteral("正则无效: %1").arg(pattern)};
                            QJsonObject data;
                            data.insert(QStringLiteral("pattern"), pattern);
                            data.insert(QStringLiteral("regex"), regex);
                            data.insert(QStringLiteral("hits"), hits);
                            data.insert(QStringLiteral("truncated"),
                                        hits.size() >= maxHits);
                            return {true, false, data, QString()};
                        });
    meta("xterm.screen.find",
         QStringLiteral("在屏幕尾部文本里查找（默认最近 400 行）：返回命中行的绝对行号/列/"
                        "命中串/整行；regex=true 时 pattern 为正则（大小写不敏感）"),
         [] {
             QJsonObject params;
             QJsonObject pattern;
             pattern.insert(QStringLiteral("type"), QStringLiteral("string"));
             pattern.insert(QStringLiteral("description"),
                            QStringLiteral("查找内容（子串或正则）"));
             params.insert(QStringLiteral("pattern"), pattern);
             QJsonObject regex;
             regex.insert(QStringLiteral("type"), QStringLiteral("boolean"));
             regex.insert(QStringLiteral("description"),
                          QStringLiteral("pattern 按正则解释，默认 false"));
             params.insert(QStringLiteral("regex"), regex);
             QJsonObject maxLines;
             maxLines.insert(QStringLiteral("type"), QStringLiteral("integer"));
             maxLines.insert(QStringLiteral("description"),
                             QStringLiteral("扫描尾部行数，默认 400"));
             params.insert(QStringLiteral("max_lines"), maxLines);
             QJsonObject maxHits;
             maxHits.insert(QStringLiteral("type"), QStringLiteral("integer"));
             maxHits.insert(QStringLiteral("description"),
                            QStringLiteral("最多返回命中数，默认 50，上限 200"));
             params.insert(QStringLiteral("max_hits"), maxHits);
             return params;
         }());
}
