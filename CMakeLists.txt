cmake_minimum_required(VERSION 3.19)

project(Xterm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

# vcpkg triplet 决定 .deps/vcpkg_installed/ 下的安装目录名；
# 换工具链（如 MSVC x64-windows）时改这里或用 -DXTERM_VCPKG_TRIPLET=... 覆盖。
set(XTERM_VCPKG_TRIPLET
    "x64-mingw-dynamic"
    CACHE STRING "vcpkg triplet of the libssh/OpenSSL install")
set(XTERM_SSH_DEPS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vcpkg_installed/${XTERM_VCPKG_TRIPLET}"
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

qt_add_executable(Xterm
    WIN32
    MACOSX_BUNDLE
    main.cpp
    icons.cpp
    icons.h
    mainwindow.cpp
    mainwindow.h
    thememanager.cpp
    thememanager.h
    hotkeymanager.cpp
    hotkeymanager.h
    titlebar.cpp
    titlebar.h
    framelessdialog.cpp
    framelessdialog.h
    mainwindow.ui
    agentbridge.cpp
    agentbridge.h
    auditlog.cpp
    auditlog.h
    fsbridge.cpp
    fsbridge.h
    hostbridge.cpp
    hostbridge.h
    screenbridge.cpp
    screenbridge.h
    sessionbridge.cpp
    sessionbridge.h
    mcpmainhook.cpp
    mcpmainhook.h
    sessioncontroller.cpp
    sessioncontroller.h
    uibuilder.cpp
    uibuilder.h
    kmsgmutetoggle.cpp
    kmsgmutetoggle.h
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
    filebrowsermodel.cpp
    filebrowsermodel.h
    filebrowserwidgets.cpp
    filebrowserwidgets.h
    transferqueue.cpp
    transferqueue.h
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
    ssh/statschannel.cpp
    ssh/statschannel.h
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
        ws2_32
        bcrypt
        shell32
        ole32
        shlwapi
        uuid
)

xterm_deploy_ssh_runtime(Xterm)

# tools/ 下的 SSH/SFTP 调试工具，日常构建用不到，默认不编（需要时 -DXTERM_BUILD_TOOLS=ON）
option(XTERM_BUILD_TOOLS "Build the sshtest/sftptest debug tools" OFF)
if(XTERM_BUILD_TOOLS)
    add_subdirectory(tools/sshtest)
    add_subdirectory(tools/sftptest)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS Xterm
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 发布部署：运行时 DLL（libssh/OpenSSL，与构建期 DeploySshRuntime 同源）
# 与第三方许可说明一并安装。
install(DIRECTORY "${XTERM_SSH_DEPS_ROOT}/bin/"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        FILES_MATCHING PATTERN "*.dll")
install(FILES "${XTERM_WINPTHREAD_DLL}"
        DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
