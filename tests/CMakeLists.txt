find_package(Qt6 REQUIRED COMPONENTS Test)

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
