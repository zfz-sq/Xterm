#ifndef SCREENBRIDGE_H
#define SCREENBRIDGE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

#include "agentbridge.h" // ToolHandler / Hooks（工具注册表）
#include "termscreen.h"  // TermScreen（snapshot 的数据形状，纯模型无 GUI 依赖）

/*
 * ScreenBridge: AgentBridge 的 xterm.screen 扩展工具后端（M2.d）。
 *
 * 两件工具：
 * - snapshot：屏幕尾部 N 行的结构化快照——每行 text（纯文本）+ runs
 *   （非默认样式段：解析后的 fg/bg 十六进制色 + bold/underline/inverse）
 *   + links（OSC 8 超链接段，M3.2 的 URI 池），行号与 wait 锚点同源的
 *   绝对行号，AI 可见颜色语义与链接（验收「URI 可见」）
 * - find：在滚动历史尾部扫描子串/正则（语义对齐 M3.3 的 Ctrl+F 查找：
 *   大小写不敏感、空匹配不算命中）；返回带绝对行号/列的命中列表
 *
 * 第三件 waitForPattern 是 AgentBridge 内置（挂起应答/超时/轮询都在
 * bridge 的 Waiter 机制里，外部工具复制这套生命周期不划算）。
 *
 * 后端注入：与 FsBridge/HostBridge 同款——纯函数集，本类自持数据源
 * （外部工具 lambda 不能捕获主窗口栈上的 Hooks 指针，AgentBridge::start
 * 是值拷贝）。测试喂 TermScreen 与假文本，零 GUI 依赖。
 */
class ScreenBridge
{
public:
    // Screen 后端能力，全部同步语义
    struct ScreenHooks
    {
        // 当前活动终端的屏幕模型（null = 无活动视图，snapshot 不可用）
        std::function<const TermScreen *()> screen;
        // 绝对行号总数（wait 锚点同源；find 的扫描窗上界）
        std::function<qint64()> lineCount;
        // 自绝对行号起的文本（find 的扫描源；上限 400 行与 read 一致）
        std::function<QString(qint64 fromLine, int maxLines)> textFrom;
    };

    ScreenBridge() = default;

    void setBackend(ScreenHooks hooks) { m_hooks = std::move(hooks); }

    // 两件工具填进 hooks->tools / hooks->toolMeta（AgentBridge::start 合入注册表）
    void registerTools(AgentBridge::Hooks *hooks);

    // 屏幕尾部 maxLines 行的结构化快照（公开静态，测试直接喂 TermScreen）。
    // 行号口径：主屏 = dropped + history + rows 的绝对行号；备用屏 = 屏内行号
    static QJsonObject snapshot(const TermScreen &screen, int maxLines);

    // find 命中扫描（公开静态）：text 按 '\n' 拆行，startLine 是首行绝对行号。
    // regex=true 时 pattern 按正则（大小写不敏感）；badPattern 出参报非法正则
    static QJsonArray findMatches(const QString &text, qint64 startLine,
                                  const QString &pattern, bool regex,
                                  int maxHits, bool *badPattern);

private:
    ScreenHooks m_hooks;
};

#endif // SCREENBRIDGE_H
