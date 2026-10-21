cmake_minimum_required(VERSION 3.19)

project(Xterm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

set(XTERM_SSH_DEPS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vcpkg_installed/x64-mingw-dynamic"
    CACHE PATH "Root of dynamic libssh/OpenSSL dependencies")
list(PREPEND CMAKE_PREFIX_PATH "${XTERM_SSH_DEPS_ROOT}")
find_package(libssh CONFIG REQUIRED)

get_filename_component(_xterm_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_file(XTERM_WINPTHREAD_DLL
    NAMES libwinpthread-1.dll
    HINTS "${_xterm_compiler_bin}"
    NO_DEFAULT_PATH
    REQUIRED)

function(xterm_deploy_ssh_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            -DSOURCE_DIR="${XTERM_SSH_DEPS_ROOT}/bin"
            -DWINPTHREAD_DLL="${XTERM_WINPTHREAD_DLL}"
            -DDEST_DIR="$<TARGET_FILE_DIR:${target}>"
            -P "${PROJECT_SOURCE_DIR}/cmake/DeploySshRuntime.cmake"
        VERBATIM)
endfunction()

# Windows 可执行文件图标: 从编译器同级目录找 windres（Qt 的 llvm-mingw 工具链不在 PATH）
set(XTERM_RC_SOURCES "")
if(WIN32)
    get_filename_component(_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(XTERM_RC_PROG NAMES windres.exe llvm-windres.exe HINTS "${_cxx_dir}")
    if(XTERM_RC_PROG)
        set(CMAKE_RC_COMPILER "${XTERM_RC_PROG}" CACHE FILEPATH "RC compiler" FORCE)
        enable_language(RC)
        set(XTERM_RC_SOURCES resources/app.rc)
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort Network)

qt_standard_project_setup()

# qt_standard_project_setup() 不含 AUTORCC，qrc 资源需显式开启
set(CMAKE_AUTORCC ON)

# vendored libssh2（SFTP 协议栈）：纯 C、静态链接、WinCNG 加密后端（走
# Windows bcrypt API，免 OpenSSL 依赖）、不启 zlib 压缩。
# EXCLUDE_FROM_ALL：不随默认目标构建，仅在 Xterm 链接时才缩译。
set(_xterm_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND "WinCNG" CACHE STRING "" FORCE)
set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
# WinCNG 默认只有有限域 DH。OpenSSH 9/10 默认只接受 curve25519 和
# ecdh-sha2-nistp*，对不上就会报 Unable to exchange encryption keys。
# ECDSA/ECDH 走 bcrypt，需要 Windows 10 及以上。
set(ENABLE_ECDSA_WINCNG ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/libssh2 EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_xterm_build_testing}")

qt_add_executable(Xterm
    WIN32
    MACOSX_BUNDLE
    main.cpp
    icons.cpp
    icons.h
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    agentbridge.cpp
    agentbridge.h
    serialsession.cpp
    serialsession.h
    transport.h
    sshtarget.h
    sshsession.cpp
    sshsession.h
    commandbar.cpp
    commandbar.h
    commandstore.cpp
    commandstore.h
    completionengine.cpp
    completionengine.h
    termscreen.cpp
    termscreen.h
    screenview.cpp
    screenview.h
    sftpclient.cpp
    sftpclient.h
    hoststats.cpp
    hoststats.h
    filebrowserdialog.cpp
    filebrowserdialog.h
    transferdialog.cpp
    transferdialog.h
    ssh/libsshapi.cpp
    ssh/libsshapi.h
    ssh/libsshconnection.cpp
    ssh/libsshconnection.h
    ssh/knownhostsstore.cpp
    ssh/knownhostsstore.h
    ssh/shellchannel.cpp
    ssh/shellchannel.h
    ssh/sftprequest.h
    ssh/sftpchannel.cpp
    ssh/sftpchannel.h
    ssh/sshworker.cpp
    ssh/sshworker.h
    ssh/sshtypes.h
    resources/resources.qrc
    ${XTERM_RC_SOURCES}
)

target_include_directories(Xterm PRIVATE "${CMAKE_SOURCE_DIR}")

target_link_libraries(Xterm
    PRIVATE
        Qt6::Widgets
        Qt6::SerialPort
        Qt6::Network
        ssh
        libssh2
        ws2_32
        bcrypt
        shell32
        ole32
        shlwapi
        uuid
)

xterm_deploy_ssh_runtime(Xterm)

add_subdirectory(tools/sshtest)
add_subdirectory(tools/sftptest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS Xterm
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
