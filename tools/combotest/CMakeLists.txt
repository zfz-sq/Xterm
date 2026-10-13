cmake_minimum_required(VERSION 3.19)
project(combotest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(combotest
    main.cpp
)
set_target_properties(combotest PROPERTIES WIN32_EXECUTABLE OFF)

target_link_libraries(combotest PRIVATE Qt6::Widgets)
