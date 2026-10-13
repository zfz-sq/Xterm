cmake_minimum_required(VERSION 3.19)
project(feedtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(feedtest
    main.cpp
    ../../termscreen.cpp
    ../../termscreen.h
)
set_target_properties(feedtest PROPERTIES WIN32_EXECUTABLE OFF)

target_include_directories(feedtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(feedtest PRIVATE Qt6::Widgets)
