cmake_minimum_required(VERSION 3.19)
project(completiontest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(completiontest
    main.cpp
    ../../completionengine.cpp
    ../../completionengine.h
    ../../termscreen.cpp
    ../../termscreen.h
    ../../screenview.cpp
    ../../screenview.h
)
# 控制台子系统，否则 printf 输出看不到
set_target_properties(completiontest PROPERTIES WIN32_EXECUTABLE OFF)

target_include_directories(completiontest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(completiontest PRIVATE Qt6::Widgets)