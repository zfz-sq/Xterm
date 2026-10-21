if(NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR)
    message(FATAL_ERROR "SOURCE_DIR and DEST_DIR are required")
endif()

string(REGEX REPLACE "^\"|\"$" "" SOURCE_DIR "${SOURCE_DIR}")
string(REGEX REPLACE "^\"|\"$" "" DEST_DIR "${DEST_DIR}")

file(GLOB _runtime_dlls "${SOURCE_DIR}/*.dll")
if(NOT _runtime_dlls)
    message(FATAL_ERROR "No runtime DLLs found in ${SOURCE_DIR}")
endif()

set(_has_libssh FALSE)
set(_has_libcrypto FALSE)
foreach(_dll IN LISTS _runtime_dlls)
    get_filename_component(_name "${_dll}" NAME)
    string(TOLOWER "${_name}" _name_lower)
    if(_name_lower STREQUAL "libssh.dll" OR _name_lower STREQUAL "ssh.dll")
        set(_has_libssh TRUE)
    endif()
    if(_name_lower STREQUAL "libcrypto-3-x64.dll")
        set(_has_libcrypto TRUE)
    endif()
endforeach()

if(NOT _has_libssh)
    message(FATAL_ERROR "Neither libssh.dll nor ssh.dll exists in ${SOURCE_DIR}")
endif()
if(NOT _has_libcrypto)
    message(FATAL_ERROR "libcrypto-3-x64.dll does not exist in ${SOURCE_DIR}")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")
foreach(_dll IN LISTS _runtime_dlls)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "${DEST_DIR}"
        RESULT_VARIABLE _copy_result
    )
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "Failed to copy ${_dll} to ${DEST_DIR}")
    endif()
endforeach()
