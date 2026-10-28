# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(lanes_tests
    core/error_test.cpp
    core/version_test.cpp
    core/platform/path_test.cpp
    testserver/content_test.cpp
    testserver/test_server_test.cpp)
target_link_libraries(lanes_tests PRIVATE lanes::core lanes::testserver Catch2::Catch2WithMain)
lanes_set_compiler_options(lanes_tests)

if(WIN32)
    # Helper program driven by the process-runner tests.
    add_executable(lanes_test_child helpers/test_child.cpp)
    lanes_set_compiler_options(lanes_test_child)

    target_sources(lanes_tests PRIVATE
        core/platform/file_test.cpp
        core/platform/process_test.cpp)
    target_compile_definitions(lanes_tests PRIVATE
        LANES_TEST_CHILD_PATH="$<TARGET_FILE:lanes_test_child>")
    add_dependencies(lanes_tests lanes_test_child)
endif()

catch_discover_tests(lanes_tests)

# Smoke test: the CLI starts and reports the project version.
add_test(NAME cli_version COMMAND lanes_cli --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")
