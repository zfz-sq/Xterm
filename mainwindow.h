#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include "commandstore.h"
#include "completionengine.h"

class CommandBar;
class QLabel;
class QTimer;
class SerialSession;
class SshSession;
class TerminalView;
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

private:
    void setupIcons();
    void setupConnectionCombos();
    void setupSshInputs();
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
    void pushSshHistory(const QString &host, const QString &user);

    void onConnectClicked();
    void onConnectionParamChanged();
    void browsePrivateKey();

    void sendRaw(const QByteArray &bytes);       // 唯一发送汇聚点 → 当前链路
    void sendLine(const QString &line);          // 预设命令按钮用

    Transport *activeTransport() const;
    TerminalView *activeTerminal() const;
    void setMode(Mode mode, bool persist = true);
    void updateModeUi();
    void updateConnectionUi();
    void updatePersistentStatus();
    QString connectionSummary() const;
    void applyTheme();

    Ui::MainWindow *ui;
    SerialSession *m_serial = nullptr;
    SshSession *m_ssh = nullptr;
    CommandStore *m_store = nullptr;
    CommandBar *m_commandBar = nullptr;
    TerminalView *m_serialView = nullptr;
    TerminalView *m_sshView = nullptr;
    CompletionEngine *m_completion = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_saveTimer = nullptr; // 配置保存防抖（单发，见 scheduleProfileSave）

    Mode m_mode = Mode::Serial;
    QString m_currentProfile;
    bool m_loading = false;
};
#endif // MAINWINDOW_H
