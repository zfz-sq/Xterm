cmake_minimum_required(VERSION 3.19)

project(libsshprobe LANGUAGES CXX)

set(XTERM_SSH_DEPS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.deps/vcpkg_installed/x64-mingw-dynamic"
    CACHE PATH "Root of dynamic libssh/OpenSSL dependencies")
list(PREPEND CMAKE_PREFIX_PATH "${XTERM_SSH_DEPS_ROOT}")

find_package(libssh CONFIG REQUIRED)

add_executable(libsshprobe main.cpp)
target_link_libraries(libsshprobe PRIVATE ssh)

function(xterm_deploy_ssh_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            -DSOURCE_DIR="${XTERM_SSH_DEPS_ROOT}/bin"
            -DDEST_DIR="$<TARGET_FILE_DIR:${target}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/DeploySshRuntime.cmake"
        VERBATIM)
endfunction()

xterm_deploy_ssh_runtime(libsshprobe)
