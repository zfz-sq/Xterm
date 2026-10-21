find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(test_knownhosts
    test_knownhosts.cpp
    "${CMAKE_SOURCE_DIR}/ssh/knownhostsstore.cpp"
)

target_include_directories(test_knownhosts
    PRIVATE
        "${CMAKE_SOURCE_DIR}"
)

target_link_libraries(test_knownhosts
    PRIVATE
        Qt6::Test
)

set_target_properties(test_knownhosts PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_test(NAME test_knownhosts COMMAND test_knownhosts)
