# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
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
    core/platform/proxy_test.cpp
    testserver/content_test.cpp
    testserver/test_server_test.cpp)
target_link_libraries(lanes_tests PRIVATE
    lanes::core lanes_cli_lib lanes::testserver nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
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
        core/service/service_test.cpp
        core/store/store_test.cpp
        core/verify/checksum_test.cpp
        bridge/bridge_test.cpp)
    target_link_libraries(lanes_tests PRIVATE lanes_bridge_lib)
    target_compile_definitions(lanes_tests PRIVATE
        LANES_TEST_CHILD_PATH="$<TARGET_FILE:lanes_test_child>"
        LANES_TEST_DOWNLOADER_PATH="$<TARGET_FILE:lanes_test_downloader>"
        LANES_CLI_PATH="$<TARGET_FILE:lanes_cli>"
        LANES_BRIDGE_PATH="$<TARGET_FILE:lanes_bridge>")
    add_dependencies(lanes_tests lanes_test_child lanes_test_downloader lanes_cli lanes_bridge)
endif()

catch_discover_tests(lanes_tests)

# Smoke test: the CLI starts and reports the project version.
add_test(NAME cli_version COMMAND lanes_cli --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}")

# GUI models and QML (only when the GUI is built). Needs the Qt DLLs windeployqt puts next to
# lanes-gui.exe, so it is built after it.
if(TARGET lanes_gui_lib)
    add_executable(lanes_gui_tests gui/gui_test.cpp)
    target_link_libraries(lanes_gui_tests PRIVATE
        lanes_gui_lib lanes_gui_libplugin lanes::testserver Catch2::Catch2 Qt6::QuickControls2)
    set_target_properties(lanes_gui_tests PROPERTIES AUTOMOC ON)
    lanes_set_compiler_options(lanes_gui_tests)
    add_dependencies(lanes_gui_tests lanes_gui)
    # Headless runs need the offscreen platform plugin, which windeployqt does not deploy.
    add_custom_command(TARGET lanes_gui_tests PRE_LINK
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:lanes_gui>/platforms
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>
                $<TARGET_FILE_DIR:lanes_gui>/platforms
        VERBATIM)
    catch_discover_tests(lanes_gui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Browser extension logic and syntax (plain JavaScript), when Node.js is installed.
find_program(LANES_NODE node)
if(LANES_NODE)
    add_test(NAME extension_rules
             COMMAND ${LANES_NODE} --test ${CMAKE_CURRENT_SOURCE_DIR}/extension/rules.test.mjs)
    foreach(script background.js content.js popup.js lib/rules.js)
        add_test(NAME extension_syntax_${script}
                 COMMAND ${LANES_NODE} --check ${PROJECT_SOURCE_DIR}/extension/${script})
    endforeach()
else()
    message(STATUS "Node.js not found: extension tests are skipped")
endif()
