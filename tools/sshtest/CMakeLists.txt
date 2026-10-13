cmake_minimum_required(VERSION 3.19)
project(sshtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

qt_standard_project_setup()

qt_add_executable(sshtest
    main.cpp
    ../../conpty.cpp
    ../../conpty.h
    ../../sshsession.cpp
    ../../sshsession.h
    ../../sshtarget.h
    ../../transport.h
)

target_include_directories(sshtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(sshtest PRIVATE Qt6::Core)
