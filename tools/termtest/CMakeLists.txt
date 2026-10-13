cmake_minimum_required(VERSION 3.19)
project(termtest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Gui)

qt_standard_project_setup()

qt_add_executable(termtest
    main.cpp
    ../../termscreen.cpp
    ../../termscreen.h
)
set_target_properties(termtest PROPERTIES WIN32_EXECUTABLE OFF)

target_include_directories(termtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(termtest PRIVATE Qt6::Gui)
