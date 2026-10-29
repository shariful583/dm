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
foreach(program ffmpeg.exe yt-dlp.exe deno.exe)
    set(source ${PROJECT_SOURCE_DIR}/third_party/bin/${program})
    if(EXISTS ${source})
        file(MAKE_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        file(CREATE_LINK ${source} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${program} COPY_ON_ERROR)
    else()
        message(STATUS "${program} not found: run tools/fetch-third-party.ps1 (stream tests skip)")
    endif()
endforeach()

# `cmake --install` lays out the installed program flat (all executables in one folder, as Lanes
# expects: it finds ffmpeg, yt-dlp, deno, lanes-gui and lanes-bridge next to itself). The
# installer (installer/lanes.iss, tools/make-installer.ps1) packs that folder.
set(CMAKE_INSTALL_BINDIR ".") # a normal variable: wins over GNUInstallDirs' cached "bin"

add_subdirectory(src/core)
add_subdirectory(src/cli)
if(WIN32)
    add_subdirectory(src/bridge)
endif()

# GUI (Qt 6.8+ MSVC kit, from the Qt installer). Found through CMAKE_PREFIX_PATH / Qt6_DIR, or
# the newest C:\Qt\6.*\msvc*_64 kit. Without Qt the GUI is skipped: the CI workflow builds core,
# CLI and tests; the release workflow installs Qt and builds everything.
option(LANES_BUILD_GUI "Build the Qt GUI (lanes-gui.exe) when Qt is found" ON)
if(LANES_BUILD_GUI)
    if(DEFINED ENV{QT_ROOT_DIR}) # set by aqtinstall / install-qt-action (CI)
        list(APPEND CMAKE_PREFIX_PATH $ENV{QT_ROOT_DIR})
    endif()
    if(WIN32 AND NOT Qt6_DIR)
        file(GLOB qt_kits LIST_DIRECTORIES true "C:/Qt/6.*/msvc*_64")
        list(SORT qt_kits COMPARE NATURAL ORDER DESCENDING)
        list(APPEND CMAKE_PREFIX_PATH ${qt_kits})
    endif()
    find_package(Qt6 6.8 QUIET COMPONENTS Quick QuickControls2 Qml Test)
    if(Qt6_FOUND)
        message(STATUS "Qt ${Qt6_VERSION} found: building lanes-gui")
        add_subdirectory(src/gui)
    else()
        message(STATUS "Qt 6.8+ (MSVC kit) not found: lanes-gui is skipped")
    endif()
endif()

# --- install (packaging) ---
# Component "app": Lanes itself; "tools": the bundled third-party programs (large).
install(FILES LICENSE THIRD_PARTY_NOTICES.md README.md DESTINATION . COMPONENT app)
install(DIRECTORY extension/ DESTINATION extension COMPONENT app)
# License texts of everything shipped: the bundled programs' (licenses/) and those of the
# libraries compiled into Lanes (vcpkg's copyright file per port).
install(DIRECTORY licenses/ DESTINATION licenses COMPONENT app)
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    foreach(port cli11 curl nlohmann-json openssl pugixml sqlite3 tl-expected zlib)
        set(copyright ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${port}/copyright)
        if(EXISTS ${copyright})
            install(FILES ${copyright} DESTINATION licenses RENAME ${port}.txt COMPONENT app)
        endif()
    endforeach()
endif()
foreach(program ffmpeg.exe yt-dlp.exe deno.exe)
    if(EXISTS ${PROJECT_SOURCE_DIR}/third_party/bin/${program})
        install(PROGRAMS ${PROJECT_SOURCE_DIR}/third_party/bin/${program} DESTINATION .
                COMPONENT tools)
    endif()
endforeach()
# The MSVC runtime next to the programs (redistributable), so no VC++ installer is needed.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT app)
include(InstallRequiredSystemLibraries)

if(LANES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tools/testserver)
    add_subdirectory(tests)
endif()
