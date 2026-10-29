# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)

project(lanes
    VERSION 0.1.0
    DESCRIPTION "Lanes Download Manager"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dynamic CRT (/MD, /MDd) everywhere; deps are static libs built against it (x64-windows-static-md).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

option(LANES_BUILD_TESTS "Build Lanes tests" ON)
option(LANES_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(LanesCompilerOptions)

# Third-party programs fetched by tools/fetch-third-party.ps1 go next to the executables, where
# Lanes looks for them. A hard link: no copy of a 100+ MB file per build folder.
foreach(program ffmpeg.exe)
    set(source ${PROJECT_SOURCE_DIR}/third_party/bin/${program})
    if(EXISTS ${source})
        file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        file(CREATE_LINK ${source} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${program} COPY_ON_ERROR)
    else()
        message(STATUS "${program} not found: run tools/fetch-third-party.ps1 (stream tests skip)")
    endif()
endforeach()

add_subdirectory(src/core)
add_subdirectory(src/cli)

if(LANES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tools/testserver)
    add_subdirectory(tests)
endif()
