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

void McpMainHook::start(const AgentBridge::Hooks &hooks)
{
    m_agent = new AgentBridge(this);
    m_agent->start(hooks);

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
        m_mcpStatusLabel->setText(tr("MCP · AI 在线"));
        m_mcpStatusLabel->setStyleSheet(QStringLiteral("color: #4CAF50; font-weight: 600;"));
        m_mcpStatusLabel->setToolTip(
            tr("AI 正在通过 MCP 访问本窗口（30 秒无活动自动转回就绪）"));
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
