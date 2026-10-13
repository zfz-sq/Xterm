cmake_minimum_required(VERSION 3.19)
project(buttontest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(buttontest
    main.cpp
    ../../icons.cpp
    ../../icons.h
    ../../commandbar.cpp
    ../../commandbar.h
    ../../commandstore.cpp
    ../../commandstore.h
)

target_include_directories(buttontest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(buttontest PRIVATE Qt6::Widgets)
