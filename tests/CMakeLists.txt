# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(lanes_tests
    core/error_test.cpp
    core/version_test.cpp)
target_link_libraries(lanes_tests PRIVATE lanes::core Catch2::Catch2WithMain)
lanes_set_compiler_options(lanes_tests)

catch_discover_tests(lanes_tests)

# Smoke test: the CLI starts and reports the project version.
add_test(NAME cli_version COMMAND lanes_cli --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")
