cmake_minimum_required(VERSION 3.19)

project(Xterm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort)

qt_standard_project_setup()

# qt_standard_project_setup() 不含 AUTORCC，qrc 资源需显式开启
set(CMAKE_AUTORCC ON)

# vendored libssh2（SFTP 协议栈）：纯 C、静态链接、WinCNG 加密后端（走
# Windows bcrypt API，免 OpenSSL 依赖）、不启 zlib 压缩。
# EXCLUDE_FROM_ALL：不随默认目标构建，仅在 Xterm 链接时才缩译。
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND "WinCNG" CACHE STRING "" FORCE)
set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libssh2 EXCLUDE_FROM_ALL)

qt_add_executable(Xterm
    WIN32
    MACOSX_BUNDLE
    main.cpp
    icons.cpp
    icons.h
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    serialsession.cpp
    serialsession.h
    transport.h
    conpty.cpp
    conpty.h
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
    filebrowserdialog.cpp
    filebrowserdialog.h
    transferdialog.cpp
    transferdialog.h
    resources/resources.qrc
    ${XTERM_RC_SOURCES}
)

target_link_libraries(Xterm
    PRIVATE
        Qt6::Widgets
        Qt6::SerialPort
        libssh2
        ws2_32
        bcrypt
)

install(TARGETS Xterm
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
