cmake_minimum_required(VERSION 3.19)
project(sftptest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core)

# 与主程序同款 libssh2 静态配置（WinCNG、无压缩）
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND "WinCNG" CACHE STRING "" FORCE)
set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
set(ENABLE_ECDSA_WINCNG ON CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/libssh2 libssh2-build EXCLUDE_FROM_ALL)

qt_standard_project_setup()

qt_add_executable(sftptest
    main.cpp
    ../../sftpclient.cpp
    ../../sftpclient.h
    ../../sshtarget.h
)

target_include_directories(sftptest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(sftptest PRIVATE Qt6::Core libssh2 ws2_32 bcrypt)
