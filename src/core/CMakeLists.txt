# SPDX-License-Identifier: GPL-3.0-or-later

find_package(tl-expected CONFIG REQUIRED)

configure_file(version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.hpp @ONLY)

add_library(lanes_core STATIC
    error.cpp)
add_library(lanes::core ALIAS lanes_core)

target_include_directories(lanes_core PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(lanes_core PUBLIC tl::expected)
lanes_set_compiler_options(lanes_core)
