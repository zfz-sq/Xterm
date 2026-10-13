cmake_minimum_required(VERSION 3.19)
project(ptytest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 纯 Win32, 不依赖 Qt
add_executable(ptytest main.cpp)
