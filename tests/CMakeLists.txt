# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(lanes_tests
    cli/format_test.cpp
    core/error_test.cpp
    core/version_test.cpp
    core/engine/speed_meter_test.cpp
    core/limiter/rate_limiter_test.cpp
    core/net/probe_test.cpp
    core/net/request_test.cpp
    core/organize_test.cpp
    core/media/parse_test.cpp
    core/queue/schedule_test.cpp
    core/platform/path_test.cpp
    testserver/content_test.cpp
    testserver/test_server_test.cpp)
target_link_libraries(lanes_tests PRIVATE
    lanes::core lanes_cli_lib lanes::testserver Catch2::Catch2WithMain)
lanes_set_compiler_options(lanes_tests)

if(WIN32)
    # Helper programs the tests run as child processes.
    add_executable(lanes_test_child helpers/test_child.cpp)
    lanes_set_compiler_options(lanes_test_child)
    add_executable(lanes_test_downloader helpers/test_downloader.cpp) # killed mid-download
    target_link_libraries(lanes_test_downloader PRIVATE lanes::core)
    lanes_set_compiler_options(lanes_test_downloader)

    target_sources(lanes_tests PRIVATE
        cli/cli_test.cpp
        cli/engine_cli_test.cpp
        core/engine/download_test.cpp
        core/engine/engine_test.cpp
        core/engine/limit_test.cpp
        core/engine/organize_engine_test.cpp
        core/engine/resume_test.cpp
        core/engine/retry_test.cpp
        core/media/stream_test.cpp
        core/platform/file_test.cpp
        core/platform/ipc_test.cpp
        core/platform/process_test.cpp
        core/resume/sidecar_test.cpp
        core/store/store_test.cpp
        core/verify/checksum_test.cpp)
    target_compile_definitions(lanes_tests PRIVATE
        LANES_TEST_CHILD_PATH="$<TARGET_FILE:lanes_test_child>"
        LANES_TEST_DOWNLOADER_PATH="$<TARGET_FILE:lanes_test_downloader>"
        LANES_CLI_PATH="$<TARGET_FILE:lanes_cli>")
    add_dependencies(lanes_tests lanes_test_child lanes_test_downloader lanes_cli)
endif()

catch_discover_tests(lanes_tests)

# Smoke test: the CLI starts and reports the project version.
add_test(NAME cli_version COMMAND lanes_cli --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")
