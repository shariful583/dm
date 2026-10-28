# SPDX-License-Identifier: GPL-3.0-or-later
# Local HTTP test server. Test tooling only; never shipped.

find_package(CLI11 CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(lanes_testserver STATIC
    content.cpp
    hls_fixture.cpp
    test_server.cpp)
add_library(lanes::testserver ALIAS lanes_testserver)
target_include_directories(lanes_testserver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lanes_testserver
    PUBLIC httplib::httplib
    PRIVATE OpenSSL::Crypto)
lanes_set_compiler_options(lanes_testserver)

add_executable(lanes_testserver_app main.cpp)
set_target_properties(lanes_testserver_app PROPERTIES OUTPUT_NAME lanes-testserver)
target_link_libraries(lanes_testserver_app PRIVATE lanes::testserver CLI11::CLI11)
lanes_set_compiler_options(lanes_testserver_app)
