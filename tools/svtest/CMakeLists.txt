cmake_minimum_required(VERSION 3.19)
project(svtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort)

qt_standard_project_setup()

qt_add_executable(svtest
    main.cpp
    ../../screenview.cpp
    ../../screenview.h
    ../../termscreen.cpp
    ../../termscreen.h
    ../../sshsession.cpp
    ../../sshsession.h
    ../../conpty.cpp
    ../../conpty.h
    ../../serialsession.cpp
    ../../serialsession.h
    ../../completionengine.cpp
    ../../completionengine.h
    ../../sshtarget.h
    ../../transport.h
)
# 控制台子系统，否则 printf 输出看不到
set_target_properties(svtest PROPERTIES WIN32_EXECUTABLE OFF)

target_include_directories(svtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(svtest PRIVATE Qt6::Widgets Qt6::SerialPort)
