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
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupIcons();
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
    void browsePrivateKey();
    void onFilesClicked();          // 打开 SFTP 文件管理器（SSH 会话的独立第二连接）

    void sendRaw(const QByteArray &bytes);       // 唯一发送汇聚点 → 当前链路
    void sendLine(const QString &line);          // 预设命令按钮用

    Transport *activeTransport() const;
    void focusActiveTerminal();
    void appendActiveNote(const QString &text);
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
    ScreenView *m_serialView = nullptr;
    ScreenView *m_sshView = nullptr;
    CompletionEngine *m_completion = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTimer *m_saveTimer = nullptr; // 配置保存防抖（单发，见 scheduleProfileSave）
    class FileBrowserDialog *m_fileBrowser = nullptr; // SFTP 双栏窗口，存在期间独立于终端会话

    Mode m_mode = Mode::Serial;
    QString m_currentProfile;
    QString m_shellTitle;
    bool m_loading = false;
};
#endif // MAINWINDOW_H
