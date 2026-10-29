# SPDX-License-Identifier: GPL-3.0-or-later

find_package(CLI11 CONFIG REQUIRED)

# Command implementations and formatting, separate from main() so tests can use them.
add_library(lanes_cli_lib STATIC
    commands.cpp
    queue.cpp
    format.cpp)
target_include_directories(lanes_cli_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lanes_cli_lib PUBLIC lanes::core)
lanes_set_compiler_options(lanes_cli_lib)

add_executable(lanes_cli main.cpp)
# Command name is `lanes`; the GUI is lanes-gui.exe.
set_target_properties(lanes_cli PROPERTIES OUTPUT_NAME lanes)
target_link_libraries(lanes_cli PRIVATE lanes_cli_lib CLI11::CLI11)
lanes_set_compiler_options(lanes_cli)
