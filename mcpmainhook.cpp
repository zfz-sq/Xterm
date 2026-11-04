#include "mcpmainhook.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "framelessdialog.h"
#include "icons.h"

#include <QDateTime>

McpMainHook::McpMainHook(QStatusBar *statusBar, QObject *parent)
    : QObject(parent)
    , m_statusBar(statusBar)
{
    // 侧边按钮：命令栏收起态挂在右侧竖排按钮组里，只显示图标。
    // 位置由 MainWindow 决定（addSideButton），内容/图标刷新归本类。
    // 本类是 QObject 而非 QWidget，部件不以 this 为父；由主窗口挂载时重新设定父级。
    m_mcpBtn = new QToolButton;
    m_mcpBtn->setIcon(themedIcon(QStringLiteral("mcp.png")));
    m_mcpBtn->setToolTip(tr("显示 MCP 提示词"));
    connect(m_mcpBtn, &QToolButton::clicked, this, &McpMainHook::showPromptDialog);

    // 常驻 MCP 状态（就绪/AI 在线/输入中/未启用），随在线与打字状态切换
    m_mcpStatusLabel = new QLabel;
    m_mcpStatusLabel->hide(); // start() 后 refreshStatus 决定显示

    // 「AI 锁定」提示：AI 打字机输入期间常亮。普通输入被锁定，
    // 唯一的人工接管方式是 Ctrl+C（打断 AI 并释放）。
    m_agentLockLabel = new QLabel(tr("AI 正在操作 · 输入已锁定（Ctrl+C 打断）"));
    m_agentLockLabel->setStyleSheet(QStringLiteral("color: #E05252; font-weight: 600;"));
    m_agentLockLabel->setToolTip(tr("AI（MCP）正在向终端逐字输入命令。\n"
                                    "为避免人机输入交错，普通按键已被锁定。\n"
                                    "按 Ctrl+C 立即打断 AI 并恢复人工输入。"));
    m_agentLockLabel->hide();
}

void McpMainHook::start(const AgentBridge::Hooks &hooks, bool allowWrite)
{
    m_agent = new AgentBridge(this);
    m_agent->start(hooks, QString(), allowWrite);

    // MCP 在线角标: 有合法请求时点亮，30 秒无活动自动熄灭
    // （管道连接按请求建立，没有“断开”事件可依赖）
    m_mcpActiveTimer = new QTimer(this);
    m_mcpActiveTimer->setSingleShot(true);
    m_mcpActiveTimer->setInterval(30 * 1000);
    connect(m_mcpActiveTimer, &QTimer::timeout, this, [this] { setActive(false); });
    connect(m_agent, &AgentBridge::agentActive, this, [this] {
        setActive(true);
        m_mcpActiveTimer->start();
    });
    // M2.e 三态文案：最近一次工具调用带「Ns 前」，在线期间每秒刷新；
    // 打字态优先（refreshStatus 分支顺序），转回在线时自动带出最近调用
    m_lastCallTimer = new QTimer(this);
    m_lastCallTimer->setInterval(1000);
    connect(m_lastCallTimer, &QTimer::timeout, this, &McpMainHook::refreshStatus);
    connect(m_agent, &AgentBridge::agentToolCall, this,
            [this](const QString &method, bool, qint64) {
        m_lastCallMethod = method;
        m_lastCallMs = QDateTime::currentMSecsSinceEpoch();
        if (m_mcpActive)
            m_lastCallTimer->start();
        refreshStatus();
    });
    // AI 打字机：状态栏常亮锁定提示 + 命令进度，打完/被打断时释放
    connect(m_agent, &AgentBridge::agentTypingChanged, this,
            [this](bool active, const QString &text) {
        if (active) {
            setActive(true);
            m_mcpActiveTimer->stop(); // 打字期间角标常亮，不因 30 秒超时熄灭
            m_agentLockLabel->show();
            const QString shown = text.size() > 40
                                      ? text.left(40) + QStringLiteral("…") : text;
            m_statusBar->showMessage(
                tr("AI 正在输入: %1（按 Ctrl+C 打断）").arg(shown));
        } else {
            m_agentLockLabel->hide();
            m_statusBar->showMessage(QString());
            m_mcpActiveTimer->start(); // 恢复 30 秒无活动自动熄灭
        }
        refreshStatus();
    });

    refreshStatus(); // 初始：就绪 / 未启用（管道被其他实例占用时）
}

bool McpMainHook::isTyping() const
{
    return m_agent && m_agent->isTyping();
}

bool McpMainHook::interruptTyping()
{
    return m_agent && m_agent->interruptTyping();
}

void McpMainHook::setActive(bool on)
{
    if (m_mcpActive == on)
        return;
    m_mcpActive = on;
    if (!on && m_lastCallTimer)
        m_lastCallTimer->stop(); // 离线后不再刷新「N秒前」
    refreshIcon();
    refreshStatus();
}

void McpMainHook::refreshStatus()
{
    if (!m_mcpStatusLabel)
        return;
    if (m_agent && m_agent->isTyping()) {
        m_mcpStatusLabel->setText(tr("MCP · AI 输入中"));
        m_mcpStatusLabel->setStyleSheet(QStringLiteral("color: #E05252; font-weight: 600;"));
        m_mcpStatusLabel->setToolTip(
            tr("AI（MCP）正在逐字输入命令，普通输入已锁定，按 Ctrl+C 打断"));
    } else if (m_mcpActive) {
        // 在线态：有最近调用就展示「tool（Ns 前）」，5 分钟后信息太陈旧不再带
        QString text = tr("MCP · AI 在线");
        QString tip = tr("AI 正在通过 MCP 访问本窗口（30 秒无活动自动转回就绪）");
        if (!m_lastCallMethod.isEmpty()) {
            const qint64 age = QDateTime::currentMSecsSinceEpoch() - m_lastCallMs;
            if (age < 5 * 60 * 1000) {
                text = tr("MCP · %1（%2 前）")
                           .arg(shortToolName(m_lastCallMethod), humanAge(age));
                tip = tr("AI 最近一次 MCP 调用：%1（耗时计入挂起等待），"
                         "30 秒无活动自动转回就绪").arg(m_lastCallMethod);
            }
        }
        m_mcpStatusLabel->setText(text);
        m_mcpStatusLabel->setStyleSheet(QStringLiteral("color: #4CAF50; font-weight: 600;"));
        m_mcpStatusLabel->setToolTip(tip);
    } else if (m_agent && m_agent->isServing()) {
        m_mcpStatusLabel->setText(tr("MCP 就绪"));
        m_mcpStatusLabel->setStyleSheet(QString());
        m_mcpStatusLabel->setToolTip(
            tr("本机 AI（Claude 等）可通过 MCP 工具操作此窗口连接的设备；"
               "连接信息在配置目录 agent.json"));
    } else {
        m_mcpStatusLabel->setText(tr("MCP 未启用"));
        m_mcpStatusLabel->setStyleSheet(QStringLiteral("color: #888888;"));
        m_mcpStatusLabel->setToolTip(
            tr("MCP 管道被另一个 Xterm 实例占用，AI 只能操作那个窗口"));
    }
    m_mcpStatusLabel->show();
}

void McpMainHook::refreshIcon()
{
    if (!m_mcpBtn)
        return;
    QPixmap pm = themedIcon(QStringLiteral("mcp.png")).pixmap(16, 16);
    if (m_mcpActive) {
        // 右下角绿色小圆点: AI 正在通过 MCP 访问本窗口
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(76, 175, 80));
        p.drawEllipse(QRectF(9.5, 9.5, 6.0, 6.0));
        p.end();
    }
    m_mcpBtn->setIcon(QIcon(pm));
    m_mcpBtn->setToolTip(m_mcpActive ? tr("MCP 在线（AI 正在访问本窗口），点击查看提示词")
                                     : tr("显示 MCP 提示词"));
}

// xterm.screen.read → screen.read：状态栏宽度有限，去掉命名空间前缀
QString McpMainHook::shortToolName(const QString &method)
{
    return method.startsWith(QLatin1String("xterm.")) ? method.mid(6) : method;
}

// 人读时长：60s 内按秒、1h 内按分、更久按时（与「最近调用」的陈旧度匹配）
QString McpMainHook::humanAge(qint64 ageMs)
{
    const qint64 s = ageMs / 1000;
    if (s < 60)
        return QObject::tr("%1 秒").arg(s);
    const qint64 m = s / 60;
    if (m < 60)
        return QObject::tr("%1 分").arg(m);
    return QObject::tr("%1 时").arg(m / 60);
}

void McpMainHook::showPromptDialog()
{
    QString script = QDir(QCoreApplication::applicationDirPath())
                         .absoluteFilePath(QStringLiteral("../../tools/xterm_mcp/server.py"));
    script = QDir::cleanPath(script);
    if (!QFileInfo::exists(script))
        script = QStringLiteral("tools/xterm_mcp/server.py");
    script = QDir::fromNativeSeparators(script);

    const QString config = QStringLiteral(
                               "{\n"
                               "  \"mcpServers\": {\n"
                               "    \"xterm\": {\n"
                               "      \"command\": \"python\",\n"
                               "      \"args\": [\"%1\"]\n"
                               "    }\n"
                               "  }\n"
                               "}")
                               .arg(script);

    // 父级取按钮所在顶层窗口（=主窗口），保持模态/叠放与原行为一致
    FramelessDialog dlg(m_mcpBtn ? m_mcpBtn->window() : nullptr);
    dlg.setDialogTitle(tr("MCP 提示词"));
    dlg.resize(680, 520);
    auto *lay = new QVBoxLayout;
    auto *hint = new QLabel(tr("把提示词交给已经接上 Xterm MCP 的 AI。服务程序只连接本机正在运行的这个窗口。"));
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto *edit = new QPlainTextEdit(AgentBridge::instructions());
    edit->setReadOnly(true);
    lay->addWidget(edit, 1);
    lay->addWidget(new QLabel(tr("MCP 配置：")));
    auto *cfg = new QPlainTextEdit(config);
    cfg->setReadOnly(true);
    cfg->setFixedHeight(140);
    lay->addWidget(cfg);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton(tr("复制提示词"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [edit] {
        QGuiApplication::clipboard()->setText(edit->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.setContentLayout(lay);
    dlg.exec();
}
