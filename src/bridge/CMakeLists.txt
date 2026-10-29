# SPDX-License-Identifier: GPL-3.0-or-later
#
# lanes-bridge.exe: native-messaging host for the browser extension.

# Extension IDs allowed to talk to the bridge: the unpacked dev build (fixed by
# the `key` in extension/manifest.json) plus the Chrome Web Store and Edge Add-ons IDs once known.
set(LANES_EXTENSION_IDS "cljbmgibkoihmkphkhpelfgbbikjlfpf" CACHE STRING
    "Extension IDs allowed by the native-messaging host (semicolon-separated)")

find_package(nlohmann_json CONFIG REQUIRED)

add_library(lanes_bridge_lib STATIC bridge.cpp)
target_include_directories(lanes_bridge_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lanes_bridge_lib PUBLIC lanes::core PRIVATE nlohmann_json::nlohmann_json)
lanes_set_compiler_options(lanes_bridge_lib)

add_executable(lanes_bridge main.cpp)
set_target_properties(lanes_bridge PROPERTIES OUTPUT_NAME lanes-bridge)
target_compile_definitions(lanes_bridge PRIVATE LANES_EXTENSION_IDS="${LANES_EXTENSION_IDS}")
target_link_libraries(lanes_bridge PRIVATE lanes_bridge_lib)
lanes_set_compiler_options(lanes_bridge)

install(TARGETS lanes_bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT app)
