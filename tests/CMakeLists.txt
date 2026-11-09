find_package(Qt6 REQUIRED COMPONENTS Test)

# 纯屏幕模型，不碰 SSH，也就不需要运行时部署
add_executable(test_termscreen
    test_termscreen.cpp
    "${CMAKE_SOURCE_DIR}/termscreen.cpp"
    "${CMAKE_SOURCE_DIR}/termscreen.h"
)

target_include_directories(test_termscreen
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_termscreen
    PRIVATE
        Qt6::Test
        Qt6::Gui
)

set_target_properties(test_termscreen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_termscreen COMMAND test_termscreen)

# 串口行编辑与全屏程序共存的状态机（离屏，不需要设备）
add_executable(test_screenview
    test_screenview.cpp
    "${CMAKE_SOURCE_DIR}/screenview.cpp"
    "${CMAKE_SOURCE_DIR}/screenview.h"
    "${CMAKE_SOURCE_DIR}/termscreen.cpp"
    "${CMAKE_SOURCE_DIR}/termscreen.h"
    "${CMAKE_SOURCE_DIR}/completionengine.cpp"
    "${CMAKE_SOURCE_DIR}/completionengine.h"
    # OSC 52 询问弹窗用 FramelessDialog（与主程序同款无边框风格，
    # 连带 titlebar/icons/qrc —— 与 test_mcpmainhook 同一套依赖组合）
    "${CMAKE_SOURCE_DIR}/framelessdialog.cpp"
    "${CMAKE_SOURCE_DIR}/framelessdialog.h"
    "${CMAKE_SOURCE_DIR}/titlebar.cpp"
    "${CMAKE_SOURCE_DIR}/titlebar.h"
    "${CMAKE_SOURCE_DIR}/icons.cpp"
    "${CMAKE_SOURCE_DIR}/icons.h"
    "${CMAKE_SOURCE_DIR}/resources/resources.qrc"
)

target_include_directories(test_screenview
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_screenview
    PRIVATE
        Qt6::Test
        Qt6::Widgets
)

set_target_properties(test_screenview PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_screenview COMMAND test_screenview)

add_executable(test_knownhosts
    test_knownhosts.cpp
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
)

target_include_directories(test_knownhosts
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_knownhosts
    PRIVATE
        Qt6::Test
)

xterm_deploy_ssh_runtime(test_knownhosts)

set_target_properties(test_knownhosts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_knownhosts COMMAND test_knownhosts)

# MCP 桥（独立管道名 + 临时 agent.json，不依赖真实配置和网络）
add_executable(test_agentbridge
    test_agentbridge.cpp
    "${CMAKE_SOURCE_DIR}/agentbridge.cpp"
    "${CMAKE_SOURCE_DIR}/auditlog.cpp"
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
)

target_include_directories(test_agentbridge
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_agentbridge
    PRIVATE
        Qt6::Test
        Qt6::Network
        Qt6::Gui
        advapi32
)

set_target_properties(test_agentbridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_agentbridge COMMAND test_agentbridge)

# AuditLog（M2.e 可观测）：纯文件协作类，验证 jsonl 追加/轮转/坏路径静默；
# AgentBridge 集成侧的审计与限流用例在 test_agentbridge 里
add_executable(test_auditlog
    test_auditlog.cpp
    "${CMAKE_SOURCE_DIR}/auditlog.cpp"
    "${CMAKE_SOURCE_DIR}/auditlog.h"
)

target_include_directories(test_auditlog
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_auditlog
    PRIVATE
        Qt6::Test
)

set_target_properties(test_auditlog PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_auditlog COMMAND test_auditlog)

# FsBridge + xterm.fs.* 工具（M2.b）：内存 fake 后端，不碰 SSH 栈与网络；
# hooksForSftp 的异步同步桥接由 test_sftpclient 覆盖
add_executable(test_agentbridge_fs
test_agentbridge_fs.cpp
    "${CMAKE_SOURCE_DIR}/fsbridge.cpp"
    "${CMAKE_SOURCE_DIR}/fsbridge.h"
    "${CMAKE_SOURCE_DIR}/agentbridge.cpp"
    "${CMAKE_SOURCE_DIR}/agentbridge.h"
    # agentbridge 引 AuditLog（M2.e 审计），连带
    "${CMAKE_SOURCE_DIR}/auditlog.cpp"
    "${CMAKE_SOURCE_DIR}/auditlog.h"
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
)

target_include_directories(test_agentbridge_fs
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_agentbridge_fs
    PRIVATE
        Qt6::Test
        Qt6::Network
        Qt6::Gui
        advapi32
)

set_target_properties(test_agentbridge_fs PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_agentbridge_fs COMMAND test_agentbridge_fs)

# HostBridge + xterm.host.* 工具（M2.c）：内存 fake 后端（snapshot 手造帧 +
# exec 回放预置 df//proc/ps 文本），不碰 SSH 栈与网络；hooksForStats 的
# 异步同步桥接由 test_sftpclient 覆盖。hoststats.h 只用 HostSnapshot 纯结构
# 不链 hoststats.cpp/moc
add_executable(test_agentbridge_host
    test_agentbridge_host.cpp
    "${CMAKE_SOURCE_DIR}/hostbridge.cpp"
    "${CMAKE_SOURCE_DIR}/hostbridge.h"
    "${CMAKE_SOURCE_DIR}/agentbridge.cpp"
    "${CMAKE_SOURCE_DIR}/agentbridge.h"
    # agentbridge 引 AuditLog（M2.e 审计），连带
    "${CMAKE_SOURCE_DIR}/auditlog.cpp"
    "${CMAKE_SOURCE_DIR}/auditlog.h"
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
)

target_include_directories(test_agentbridge_host
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_agentbridge_host
    PRIVATE
        Qt6::Test
        Qt6::Network
        Qt6::Gui
        advapi32
)

set_target_properties(test_agentbridge_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_agentbridge_host COMMAND test_agentbridge_host)

# ScreenBridge + xterm.screen.snapshot/find（M2.d）：静态 snapshot 直接喂
# TermScreen（真 VT 序列，连带 termscreen.cpp），工具级走独立管道的 fake
# 后端；waitForPattern（AgentBridge 内置）在 test_agentbridge 里覆盖
add_executable(test_agentbridge_screen
    test_agentbridge_screen.cpp
    "${CMAKE_SOURCE_DIR}/screenbridge.cpp"
    "${CMAKE_SOURCE_DIR}/screenbridge.h"
    "${CMAKE_SOURCE_DIR}/termscreen.cpp"
    "${CMAKE_SOURCE_DIR}/termscreen.h"
    "${CMAKE_SOURCE_DIR}/agentbridge.cpp"
    "${CMAKE_SOURCE_DIR}/agentbridge.h"
    # agentbridge 引 AuditLog（M2.e 审计），连带
    "${CMAKE_SOURCE_DIR}/auditlog.cpp"
    "${CMAKE_SOURCE_DIR}/auditlog.h"
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
)

target_include_directories(test_agentbridge_screen
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_agentbridge_screen
    PRIVATE
        Qt6::Test
        Qt6::Network
        Qt6::Gui
        advapi32
)

set_target_properties(test_agentbridge_screen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_agentbridge_screen COMMAND test_agentbridge_screen)

# SessionBridge + xterm.session 扩展件（M2.f）：内存 fake 后端验证工具壳
# （list/current/switch/open/close 的参数透传与错误回传）；
# 真实动作落在 SessionController 的 agent* 适配器，
# 由 test_sessioncontroller 补用例覆盖
add_executable(test_agentbridge_session
    test_agentbridge_session.cpp
    "${CMAKE_SOURCE_DIR}/sessionbridge.cpp"
    "${CMAKE_SOURCE_DIR}/sessionbridge.h"
    "${CMAKE_SOURCE_DIR}/agentbridge.cpp"
    "${CMAKE_SOURCE_DIR}/agentbridge.h"
    # agentbridge 引 AuditLog（M2.e 审计），连带
    "${CMAKE_SOURCE_DIR}/auditlog.cpp"
    "${CMAKE_SOURCE_DIR}/auditlog.h"
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
)

target_include_directories(test_agentbridge_session
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_agentbridge_session
    PRIVATE
        Qt6::Test
        Qt6::Network
        Qt6::Gui
        advapi32
)

set_target_properties(test_agentbridge_session PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_agentbridge_session COMMAND test_agentbridge_session)

# 主题协作类（M1.a）：不碰主窗口，只验状态翻转/信号/QSS；qrc 链入供 :/style-*.qss
add_executable(test_thememanager
    test_thememanager.cpp
    "${CMAKE_SOURCE_DIR}/thememanager.cpp"
    "${CMAKE_SOURCE_DIR}/thememanager.h"
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
    "${CMAKE_SOURCE_DIR}/commandstore.h"
    "${CMAKE_SOURCE_DIR}/framelessdialog.cpp"
    "${CMAKE_SOURCE_DIR}/framelessdialog.h"
    # FramelessDialog 依赖 titlebar.cpp 里的 CaptionButton / applyWin11RoundedCorners
    "${CMAKE_SOURCE_DIR}/titlebar.cpp"
    "${CMAKE_SOURCE_DIR}/titlebar.h"
    "${CMAKE_SOURCE_DIR}/icons.cpp"
    "${CMAKE_SOURCE_DIR}/icons.h"
    "${CMAKE_SOURCE_DIR}/resources/resources.qrc"
)

target_include_directories(test_thememanager
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_thememanager
    PRIVATE
        Qt6::Test
        Qt6::Widgets
        advapi32
)

set_target_properties(test_thememanager PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_thememanager COMMAND test_thememanager)

# 配置落盘：SSH 密码不进 config.json，私钥路径仍保留
add_executable(test_commandstore
    test_commandstore.cpp
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
    "${CMAKE_SOURCE_DIR}/commandstore.h"
    "${CMAKE_SOURCE_DIR}/sshtarget.h"
)

target_include_directories(test_commandstore
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_commandstore
    PRIVATE
        Qt6::Test
        Qt6::Gui
        advapi32
)

set_target_properties(test_commandstore PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_commandstore COMMAND test_commandstore)

# 全局快捷键协作类（M1.b）：只验 action 创建/快捷键/触发信号/文案，不依赖主窗口
add_executable(test_hotkeymanager
    test_hotkeymanager.cpp
    "${CMAKE_SOURCE_DIR}/hotkeymanager.cpp"
    "${CMAKE_SOURCE_DIR}/hotkeymanager.h"
)

target_include_directories(test_hotkeymanager
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_hotkeymanager
    PRIVATE
        Qt6::Test
        Qt6::Widgets
)

set_target_properties(test_hotkeymanager PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_hotkeymanager COMMAND test_hotkeymanager)

# MCP 挂钩协作类（M1.c）：验部件/初始可见性、守卫启动前安全、start 后状态标签联动。
# McpMainHook 拥有 AgentBridge + QToolButton/QLabel，并引用 FramelessDialog（提示词对话框），
# 故连带 agentbridge/commandstore（桥接依赖）+ icons/resources.qrc（themedIcon+mcp.png）
# + framelessdialog/titlebar（对话框及其按钮依赖）。
add_executable(test_mcpmainhook
    test_mcpmainhook.cpp
    "${CMAKE_SOURCE_DIR}/mcpmainhook.cpp"
    "${CMAKE_SOURCE_DIR}/mcpmainhook.h"
    "${CMAKE_SOURCE_DIR}/agentbridge.cpp"
    "${CMAKE_SOURCE_DIR}/agentbridge.h"
    # agentbridge 引 AuditLog（M2.e 审计），连带
    "${CMAKE_SOURCE_DIR}/auditlog.cpp"
    "${CMAKE_SOURCE_DIR}/auditlog.h"
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
    "${CMAKE_SOURCE_DIR}/commandstore.h"
    "${CMAKE_SOURCE_DIR}/framelessdialog.cpp"
    "${CMAKE_SOURCE_DIR}/framelessdialog.h"
    "${CMAKE_SOURCE_DIR}/titlebar.cpp"
    "${CMAKE_SOURCE_DIR}/titlebar.h"
    "${CMAKE_SOURCE_DIR}/icons.cpp"
    "${CMAKE_SOURCE_DIR}/icons.h"
    "${CMAKE_SOURCE_DIR}/resources/resources.qrc"
)

target_include_directories(test_mcpmainhook
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_mcpmainhook
    PRIVATE
        Qt6::Test
        Qt6::Widgets
        Qt6::Network
        advapi32
)

set_target_properties(test_mcpmainhook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_mcpmainhook COMMAND test_mcpmainhook)

add_executable(test_libsshconnection
    test_libsshconnection.cpp
    "${CMAKE_SOURCE_DIR}/ssh/libsshapi.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/libsshconnection.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
)

target_include_directories(test_libsshconnection
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_libsshconnection
    PRIVATE
        Qt6::Test
        ssh
)

xterm_deploy_ssh_runtime(test_libsshconnection)

set_target_properties(test_libsshconnection PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_libsshconnection COMMAND test_libsshconnection)

add_executable(test_sshworker
    test_sshworker.cpp
    "${CMAKE_SOURCE_DIR}/ssh/libsshapi.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/libsshconnection.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/shellchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sftpchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/statschannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sshworker.cpp"
)

target_include_directories(test_sshworker
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_sshworker
    PRIVATE
        Qt6::Test
        ssh
)

xterm_deploy_ssh_runtime(test_sshworker)

set_target_properties(test_sshworker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_sshworker COMMAND test_sshworker)

add_executable(test_sshsession
    test_sshsession.cpp
    "${CMAKE_SOURCE_DIR}/sshsession.cpp"
    "${CMAKE_SOURCE_DIR}/sshsession.h"
    "${CMAKE_SOURCE_DIR}/transport.h"
    "${CMAKE_SOURCE_DIR}/ssh/libsshapi.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/libsshconnection.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/shellchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sftpchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/statschannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sshworker.cpp"
)

target_include_directories(test_sshsession
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_sshsession
    PRIVATE
        Qt6::Test
        ssh
)

xterm_deploy_ssh_runtime(test_sshsession)

set_target_properties(test_sshsession PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_sshsession COMMAND test_sshsession)

add_executable(test_sftpchannel
    test_sftpchannel.cpp
    "${CMAKE_SOURCE_DIR}/sshsession.cpp"
    "${CMAKE_SOURCE_DIR}/sshsession.h"
    "${CMAKE_SOURCE_DIR}/transport.h"
    "${CMAKE_SOURCE_DIR}/ssh/libsshapi.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/libsshconnection.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/shellchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sftpchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/statschannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sshworker.cpp"
)

target_include_directories(test_sftpchannel
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_sftpchannel
    PRIVATE
        Qt6::Test
        ssh
)

xterm_deploy_ssh_runtime(test_sftpchannel)

set_target_properties(test_sftpchannel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_sftpchannel COMMAND test_sftpchannel)

add_executable(test_sftpclient
    test_sftpclient.cpp
    "${CMAKE_SOURCE_DIR}/sftpclient.cpp"
    "${CMAKE_SOURCE_DIR}/sftpclient.h"
    # hooksForStats 引 HostStats::snapshot（M2.c），连带 hoststats.cpp
    "${CMAKE_SOURCE_DIR}/hoststats.cpp"
    "${CMAKE_SOURCE_DIR}/hoststats.h"
    "${CMAKE_SOURCE_DIR}/sshsession.cpp"
    "${CMAKE_SOURCE_DIR}/sshsession.h"
    "${CMAKE_SOURCE_DIR}/transport.h"
    "${CMAKE_SOURCE_DIR}/ssh/libsshapi.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/libsshconnection.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/shellchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sftpchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/statschannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sshworker.cpp"
)

target_include_directories(test_sftpclient
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_sftpclient
    PRIVATE
        Qt6::Test
        ssh
)

xterm_deploy_ssh_runtime(test_sftpclient)

set_target_properties(test_sftpclient PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_sftpclient COMMAND test_sftpclient)

add_executable(test_statschannel
    test_statschannel.cpp
    "${CMAKE_SOURCE_DIR}/hoststats.cpp"
    "${CMAKE_SOURCE_DIR}/hoststats.h"
    # HostStats 构造里 connect SshSession 的信号，需要其 moc 符号，
    # 于是连带整条 worker 链。
    "${CMAKE_SOURCE_DIR}/sshsession.cpp"
    "${CMAKE_SOURCE_DIR}/sshsession.h"
    "${CMAKE_SOURCE_DIR}/transport.h"
    "${CMAKE_SOURCE_DIR}/ssh/libsshapi.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/libsshconnection.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/shellchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sftpchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/statschannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sshworker.cpp"
)

target_include_directories(test_statschannel
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_statschannel
    PRIVATE
        Qt6::Test
        ssh
)

xterm_deploy_ssh_runtime(test_statschannel)

set_target_properties(test_statschannel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_statschannel COMMAND test_statschannel)

# 会话生命周期 + UI 编排协作类（M1.d）：UiBuilder/SessionController 按主窗口
# 同序组装，验模式切换（栈页/按钮文案/静音开关可见性/信号）、连接入口守卫、
# 发送未连接提示、agent 适配器与 SSH 状态文本。SessionController 持有
# SshSession → 连带 worker 链；UiBuilder 连带标题栏/对话框/图标链；
# mainwindow.ui 由 AUTOUIC 生成 ui_mainwindow.h。
add_executable(test_sessioncontroller
    test_sessioncontroller.cpp
    "${CMAKE_SOURCE_DIR}/sessioncontroller.cpp"
    "${CMAKE_SOURCE_DIR}/sessioncontroller.h"
    "${CMAKE_SOURCE_DIR}/uibuilder.cpp"
    "${CMAKE_SOURCE_DIR}/uibuilder.h"
    "${CMAKE_SOURCE_DIR}/kmsgmutetoggle.cpp"
    "${CMAKE_SOURCE_DIR}/kmsgmutetoggle.h"
    "${CMAKE_SOURCE_DIR}/thememanager.cpp"
    "${CMAKE_SOURCE_DIR}/thememanager.h"
    "${CMAKE_SOURCE_DIR}/hotkeymanager.cpp"
    "${CMAKE_SOURCE_DIR}/hotkeymanager.h"
    "${CMAKE_SOURCE_DIR}/mcpmainhook.cpp"
    "${CMAKE_SOURCE_DIR}/mcpmainhook.h"
    "${CMAKE_SOURCE_DIR}/agentbridge.cpp"
    "${CMAKE_SOURCE_DIR}/agentbridge.h"
    # agentbridge 引 AuditLog（M2.e 审计），连带
    "${CMAKE_SOURCE_DIR}/auditlog.cpp"
    "${CMAKE_SOURCE_DIR}/auditlog.h"
    "${CMAKE_SOURCE_DIR}/commandstore.cpp"
    "${CMAKE_SOURCE_DIR}/commandstore.h"
    "${CMAKE_SOURCE_DIR}/commandbar.cpp"
    "${CMAKE_SOURCE_DIR}/commandbar.h"
    "${CMAKE_SOURCE_DIR}/completionengine.cpp"
    "${CMAKE_SOURCE_DIR}/completionengine.h"
    "${CMAKE_SOURCE_DIR}/screenview.cpp"
    "${CMAKE_SOURCE_DIR}/screenview.h"
    "${CMAKE_SOURCE_DIR}/termscreen.cpp"
    "${CMAKE_SOURCE_DIR}/termscreen.h"
    "${CMAKE_SOURCE_DIR}/serialsession.cpp"
    "${CMAKE_SOURCE_DIR}/serialsession.h"
    "${CMAKE_SOURCE_DIR}/transport.h"
    "${CMAKE_SOURCE_DIR}/sshtarget.h"
    "${CMAKE_SOURCE_DIR}/sshsession.cpp"
    "${CMAKE_SOURCE_DIR}/sshsession.h"
    "${CMAKE_SOURCE_DIR}/hoststats.cpp"
    "${CMAKE_SOURCE_DIR}/hoststats.h"
    "${CMAKE_SOURCE_DIR}/sftpclient.cpp"
    "${CMAKE_SOURCE_DIR}/sftpclient.h"
    "${CMAKE_SOURCE_DIR}/filebrowserdialog.cpp"
    "${CMAKE_SOURCE_DIR}/filebrowserdialog.h"
    "${CMAKE_SOURCE_DIR}/filebrowsermodel.cpp"
    "${CMAKE_SOURCE_DIR}/filebrowsermodel.h"
    "${CMAKE_SOURCE_DIR}/filebrowserwidgets.cpp"
    "${CMAKE_SOURCE_DIR}/filebrowserwidgets.h"
    "${CMAKE_SOURCE_DIR}/transferqueue.cpp"
    "${CMAKE_SOURCE_DIR}/transferqueue.h"
    "${CMAKE_SOURCE_DIR}/transferdialog.cpp"
    "${CMAKE_SOURCE_DIR}/transferdialog.h"
    "${CMAKE_SOURCE_DIR}/framelessdialog.cpp"
    "${CMAKE_SOURCE_DIR}/framelessdialog.h"
    "${CMAKE_SOURCE_DIR}/titlebar.cpp"
    "${CMAKE_SOURCE_DIR}/titlebar.h"
    "${CMAKE_SOURCE_DIR}/icons.cpp"
    "${CMAKE_SOURCE_DIR}/icons.h"
    "${CMAKE_SOURCE_DIR}/ssh/libsshapi.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/libsshconnection.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/shellchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sftpchannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sftprequest.h"
    "${CMAKE_SOURCE_DIR}/ssh/statschannel.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sshworker.cpp"
    "${CMAKE_SOURCE_DIR}/ssh/sshtypes.h"
    "${CMAKE_SOURCE_DIR}/resources/resources.qrc"
    "${CMAKE_SOURCE_DIR}/mainwindow.ui"
)

target_include_directories(test_sessioncontroller
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_sessioncontroller
    PRIVATE
        Qt6::Test
        Qt6::Widgets
        Qt6::SerialPort
        Qt6::Network
        ssh
        ws2_32
        bcrypt
        shell32
        ole32
        shlwapi
        uuid
        advapi32
)

xterm_deploy_ssh_runtime(test_sessioncontroller)

set_target_properties(test_sessioncontroller PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    # mainwindow.ui 在源码根目录，AUTOUIC 默认只在源文件同级目录找
    AUTOUIC_SEARCH_PATHS "${CMAKE_SOURCE_DIR}"
)

add_test(NAME test_sessioncontroller COMMAND test_sessioncontroller)
