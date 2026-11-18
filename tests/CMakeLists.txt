find_package(Qt6 REQUIRED COMPONENTS Test)

function(xterm_add_test name)
    add_executable(${name} ${name}.cpp)
    target_sources(${name} PRIVATE $<TARGET_OBJECTS:xterm_resources>)
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE xterm_common Qt6::Test)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        AUTOUIC_SEARCH_PATHS "${CMAKE_SOURCE_DIR}"
    )
    xterm_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

xterm_add_test(test_termscreen)
xterm_add_test(test_screenview)
xterm_add_test(test_knownhosts)
xterm_add_test(test_agentbridge)
xterm_add_test(test_auditlog)
xterm_add_test(test_agentbridge_fs)
xterm_add_test(test_agentbridge_host)
xterm_add_test(test_agentbridge_screen)
xterm_add_test(test_agentbridge_session)
xterm_add_test(test_thememanager)
xterm_add_test(test_scrollflash)
xterm_add_test(test_commandstore)
xterm_add_test(test_commandbar)
xterm_add_test(test_hotkeymanager)
xterm_add_test(test_mcpmainhook)
xterm_add_test(test_libsshconnection)
xterm_add_test(test_sshworker)
xterm_add_test(test_sshsession)
xterm_add_test(test_sftpchannel)
xterm_add_test(test_sftpclient)
xterm_add_test(test_statschannel)
xterm_add_test(test_sessioncontroller)

foreach(test IN ITEMS
         test_knownhosts
         test_libsshconnection
         test_sshworker
         test_sshsession
         test_sftpchannel
         test_sftpclient
         test_statschannel
         test_sessioncontroller)
    xterm_deploy_ssh_runtime(${test})
endforeach()
