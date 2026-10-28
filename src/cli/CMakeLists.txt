# SPDX-License-Identifier: GPL-3.0-or-later

find_package(CLI11 CONFIG REQUIRED)

add_executable(lanes_cli main.cpp)
# Command name is `lanes`; the GUI is lanes-gui.exe.
set_target_properties(lanes_cli PROPERTIES OUTPUT_NAME lanes)
target_link_libraries(lanes_cli PRIVATE lanes::core CLI11::CLI11)
lanes_set_compiler_options(lanes_cli)
