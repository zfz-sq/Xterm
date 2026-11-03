#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QJsonObject>
#include <QMainWindow>

#include "commandstore.h"
#include "completionengine.h"
#include "ssh/sshtypes.h"

class AgentBridge;
class HostStats;
class QAction;

class CommandBar;
class QLabel;
class QTimer;
class QToolButton;
class TitleBar;
class SerialSession;
class SshSession;
class ScreenView;
class Transport;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // 工作模式：只决定「当前显示哪个终端、参数栏是哪一套、命令发给谁」，
    // 两条链路本身各自独立存活，切换模式不影响任何一条连接。
    enum class Mode
    {
        Serial,
        Ssh
    };

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupIcons();
    void setupTitleBar();             // 无边框窗口自定义标题栏（VSCode 式工具条）
    void setupConnectionCombos();
    void setupSshInputs();
    void fitSshPortEdit(); // 端口编辑框拉到箭头前，避免数字被裁、中间留白
    void setupTerminal();
    void setupCompletion();
    void connectSignals();

    void refreshPorts();
    void reloadProfiles(const QString &selectName);
    void applyCurrentProfile();
    void syncCurrentProfile();
    void scheduleProfileSave(); // 配置写盘防抖：变化停稳后合并成一次落盘
    Profile currentProfileFromUi() const;
    QString selectedPortName() const;
    SshTarget sshTargetFromUi() const;

    void onConnectClicked();
    void onConnectionParamChanged();
    void onSerialModeToggled(bool pure);
    void browsePrivateKey();
    void onFilesClicked();          // 打开 SFTP 文件管理器（SSH 会话的独立第二连接）
    void onMcpClicked();            // 弹出 MCP 提示词
    void openKeywordSettings();     // 设置菜单: 关键词高亮编辑（表格，改即预览）
    void openFontSettings();        // 设置菜单: 终端字体族 + 字号（改即预览，两页同步）
    void applyKeywords(const QList<KeywordRule> &rules); // 规则落到存储并同步两个终端页
    void onHighlightRequested(const QString &word); // 终端选中词右键「高亮此词」
    void setMcpActive(bool on);     // MCP 在线角标: AI 正在访问本窗口时点亮
    void refreshMcpIcon();          // 按当前主题 + 在线状态重建 MCP 按钮图标
    void refreshMcpStatus();        // 状态栏常驻 MCP 状态（就绪/AI 在线/未启用/输入中）

    QJsonObject agentStatus() const;
    QString agentRecentText(int maxLines) const;
    QString agentSendLine(const QString &text);
    QString agentSendBytes(const QByteArray &bytes); // MCP send_control 用: 裸字节直达

    void sendRaw(const QByteArray &bytes);       // 唯一发送汇聚点 → 当前链路
    void sendLine(const QString &line);          // 预设命令按钮用
    void toggleKmsgMute();                       // 状态栏「内核日志静音」开关
    void updateKmsgMuteUi();                     // 开关文字/颜色/提示随状态刷新

    Transport *activeTransport() const;
    void focusActiveTerminal();
    void appendActiveNote(const QString &text);
    void setMode(Mode mode, bool persist = true);
    void updateModeUi();
    void updateConnectionUi();
    void updatePersistentStatus();
    QString connectionSummary() const;
    QString sshStatsStatusText() const;
    void clearHostStats();
    bool confirmHostKeyChange(const QString &host, const QString &stored, const QString &current);
    void promptHostKeyDecision(const SshHostKeyChange &change);
    void promptAuthAnswers(SshRequestId requestId, const QList<SshAuthPrompt> &prompts);
    void applyTheme();

    Ui::MainWindow *ui;
    SerialSession *m_serial = nullptr;
    SshSession *m_ssh = nullptr;
    CommandStore *m_store = nullptr;
    CommandBar *m_commandBar = nullptr;
    TitleBar *m_titleBar = nullptr;   // 自定义标题栏：菜单/主题/置顶/系统按钮
    QAction *m_connectAction = nullptr; // 工具菜单「连接/断开」项，文案随连接状态
    QAction *m_switchModeAction = nullptr; // 工具菜单「切换串口/SSH」项，文案随当前模式
    QToolButton *m_mcpBtn = nullptr; // MCP 按钮：挂命令栏收起态侧边按钮组（展开命令栏按钮下方）
    QTimer *m_mcpActiveTimer = nullptr; // MCP 在线角标自动熄灭（单发）
    bool m_mcpActive = false;           // 最近是否有合法 MCP 请求
    ScreenView *m_serialView = nullptr;
    ScreenView *m_sshView = nullptr;
    CompletionEngine *m_completion = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_xoffLabel = nullptr;      // 状态栏 XOFF 提示：发出 Ctrl+S 亮、Ctrl+Q 灭
    QLabel *m_mcpStatusLabel = nullptr; // 状态栏常驻 MCP 状态（就绪/AI 在线/未启用）
    QLabel *m_agentLockLabel = nullptr; // 状态栏「AI 锁定」提示：AI 打字时常亮，Ctrl+C 打断
    QToolButton *m_kmsgMuteBtn = nullptr; // 底部状态栏「内核日志静音」开关（仅串口页显示）
    bool m_kmsgMuted = false;             // 客户端记住的期望状态；连接建立/断开时重置
    QTimer *m_saveTimer = nullptr; // 配置保存防抖（单发，见 scheduleProfileSave）
    class FileBrowserDialog *m_fileBrowser = nullptr; // SFTP 双栏窗口，挂在共享 SshSession 上
    AgentBridge *m_agent = nullptr;
    HostStats *m_hostStats = nullptr;
    bool m_hostCpuOk = false;
    int m_hostCpu = 0;
    bool m_hostMemOk = false;
    bool m_hostMemPercentOk = false;
    int m_hostMemPercent = 0;
    qint64 m_hostUsedKiB = 0;
    qint64 m_hostTotalKiB = 0;

    Mode m_mode = Mode::Serial;
    QString m_currentProfile;
    QString m_shellTitle;
    bool m_loading = false;
};
#endif // MAINWINDOW_H
