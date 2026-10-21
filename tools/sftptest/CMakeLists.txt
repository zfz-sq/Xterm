qt_add_executable(sftptest
    main.cpp
    "${CMAKE_SOURCE_DIR}/sftpclient.cpp"
    "${CMAKE_SOURCE_DIR}/sftpclient.h"
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

target_include_directories(sftptest PRIVATE "${CMAKE_SOURCE_DIR}")

target_link_libraries(sftptest
    PRIVATE
        Qt6::Core
        ssh
)

xterm_deploy_ssh_runtime(sftptest)

set_target_properties(sftptest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
