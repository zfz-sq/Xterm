cmake_minimum_required(VERSION 3.19)
project(ansitest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(ansitest
    main.cpp
    ../../completionengine.cpp
    ../../completionengine.h
    ../../terminalview.cpp
    ../../terminalview.h
)
set_target_properties(ansitest PROPERTIES WIN32_EXECUTABLE OFF)

target_include_directories(ansitest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(ansitest PRIVATE Qt6::Widgets)
