#ifndef SESSIONCONTROLLER_H
#define SESSIONCONTROLLER_H

#include <QJsonObject>
#include <QObject>
#include <QUrl>

#include "commandstore.h" // Profile / CommandItem
#include "ssh/sshtypes.h"

class CommandBar;
class CompletionEngine;
class FileBrowserDialog;
class HostStats;
class McpMainHook;
class QComboBox;
class SerialSession;
class ScreenView;
class SshSession;
class QTimer;
class Transport;
class QWidget;

namespace Ui {
class MainWindow;
} // namespace Ui

/*
 * SessionController: 会话生命周期协作类（MainWindow 拆分 M1.d）。
 *
 * 职责：
 * - 拥有串口/SSH 两条链路（SerialSession / SshSession）与主机状态采样
 *   （HostStats，含展示缓存）、客户端 Tab 补全引擎、配置写盘防抖定时器
 * - 连接/断开、模式切换、连接参数读写与 Profile 同步、认证/主机密钥弹窗
 * - 发送汇聚点 sendRaw / sendLine（含 AI 打字机锁定检查）
 * - MCP 桥的数据适配器 agentStatus / agentRecentText / agentSendLine /
 *   agentSendBytes（M1.c 起由主窗口组装成 Hooks 注入 McpMainHook）
 *
 * 边界：只做会话域的决策与持久化，不碰 UI 刷新——模式或链路状态变化经
 * modeChanged / connectionStateChanged 回抛，UiBuilder 据此刷新控件；
 * 两条链路收到的字节流直连终端视图（attachTerminal 时接线）。持有
 * Ui::MainWindow 结构仅为读写连接参数控件，不持有 MainWindow。
 */
class SessionController : public QObject
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

    explicit SessionController(QWidget *window, Ui::MainWindow *ui,
                               CommandStore *store, McpMainHook *mcp,
                               QObject *parent = nullptr);
    ~SessionController() override;

    // —— 会话访问（主窗口接线、SFTP 浏览器、UiBuilder 状态刷新用）——
    SerialSession *serial() const { return m_serial; }
    SshSession *ssh() const { return m_ssh; }
    Transport *activeTransport() const;
    Mode mode() const { return m_mode; }

    // 终端两页与命令栏由 UiBuilder 创建，构造后注入：
    // 完成链路→视图数据流接线、补全引擎挂载与输入历史周边。
    void attachTerminal(ScreenView *serialView, ScreenView *sshView,
                        CommandBar *commandBar);

    // —— 连接与断开（连接按钮 / F5 / 各输入框回车共用入口）——
    void toggleConnect();
    void refreshPorts();
    void browsePrivateKey();
    void openFileBrowser();          // 工具栏「文件」按钮 / F10
    void onSftpRequested();          // F10：未连接时终端内给提示
    // 终端 OSC 8 链接（Ctrl+点击）分发：http(s) 等交系统；file:// 本地存在则
    // 打开，否则 SSH 在线时当远端路径走 SFTP 取回（下载后自动用关联程序打开）
    void openLink(const QUrl &url);

    // —— 参数与配置 ——
    QString selectedPortName() const;
    QString connectionSummary() const; // 状态栏/agent 状态共用的连接摘要
    void reloadProfiles(const QString &selectName);
    void applyCurrentProfile();
    void syncCurrentProfile();
    void scheduleProfileSave(); // 配置写盘防抖：变化停稳后合并成一次落盘
    void onConnectionParamChanged();
    void onSerialModeToggled(bool pure);
    void onCommandsChanged(); // 命令栏增删改：补全源 + Profile 同步

    // —— 模式切换 ——
    void setMode(Mode mode, bool persist = true);
    void toggleMode();

    // —— 发送汇聚点 ——
    void sendRaw(const QByteArray &bytes); // 唯一发送出口 → 当前链路
    void sendLine(const QString &line);    // 预设命令按钮用
    void focusActiveTerminal();
    void appendActiveNote(const QString &text);

    // —— SSH 状态栏文本（数据源为 HostStats 下沉缓存）——
    QString sshStatusText() const;

    // M2.c：HostBridge 的 stats 缓存源（xterm.host.* 的 cpu/mem 数据，不拥有）
    HostStats *hostStats() const { return m_hostStats; }

    // —— MCP 数据适配器（依赖会话/视图状态，见类注释）——
    QJsonObject agentStatus() const;
    QString agentRecentText(int maxLines) const;
    qint64 agentRecentLineCount() const; // wait 的绝对行锚点（见 AgentBridge::Hooks）
    QString agentRecentTextFrom(qint64 fromLine, int maxLines) const;
    QString agentSendLine(const QString &text);
    QString agentSendBytes(const QByteArray &bytes); // sendControl 用: 裸字节直达
    char agentLineEnding() const;                    // 打字机行尾（串口 \n / SSH \r）

    // —— 关闭编排（closeEvent 调用）：同步配置、落盘、拆链路 ——
    void shutdownForClose();

signals:
    // setMode 之后发出（含首次）；UiBuilder 刷新模式/连接/状态栏
    void modeChanged();
    // 任一链路 opened/closed/state 变化之后发出；UiBuilder 刷新连接按钮与状态栏
    void connectionStateChanged();
    // SSH stats 新帧且当前在 SSH 页（原「仅 Ssh 模式刷状态栏」的节流）
    void statsChanged();

private slots:
    void promptHostKeyDecision(const SshHostKeyChange &change);
    void promptAuthAnswers(SshRequestId requestId, const QList<SshAuthPrompt> &prompts);

private:
    void wireSessions(); // 两条链路的信号 → 系统消息/统计启停/回抛刷新
    SshTarget sshTargetFromUi() const;
    Profile currentProfileFromUi() const;
    bool confirmHostKeyChange(const QString &host, const QString &stored,
                              const QString &current);
    ScreenView *activeView() const;

    Ui::MainWindow *m_ui = nullptr;
    QWidget *m_window = nullptr;      // 仅作弹窗 parent（居中于主窗口），不碰其它
    CommandStore *m_store = nullptr; // 配置状态源（不拥有）
    McpMainHook *m_mcp = nullptr;    // AI 打字机守卫查询（不拥有）
    SerialSession *m_serial = nullptr;
    SshSession *m_ssh = nullptr;
    HostStats *m_hostStats = nullptr;
    CompletionEngine *m_completion = nullptr; // 非 QObject，指针托管生命周期
    QTimer *m_saveTimer = nullptr;
    FileBrowserDialog *m_fileBrowser = nullptr; // SFTP 双栏窗口，挂在共享 SshSession 上
    ScreenView *m_serialView = nullptr;         // UiBuilder 创建（不拥有）
    ScreenView *m_sshView = nullptr;
    CommandBar *m_commandBar = nullptr;

    Mode m_mode = Mode::Serial;
    QString m_currentProfile;
    bool m_loading = false; // 批量回填参数期间抑制 onConnectionParamChanged
};

#endif // SESSIONCONTROLLER_H
