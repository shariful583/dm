# SPDX-License-Identifier: GPL-3.0-or-later

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

configure_file(version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.hpp @ONLY)

add_library(lanes_core STATIC
    error.cpp
    batch/batch.cpp
    category/category.cpp
    engine/download.cpp
    engine/download_state.cpp
    engine/engine.cpp
    engine/speed_meter.cpp
    extract/info.cpp
    extract/updater.cpp
    extract/video.cpp
    extract/ytdlp.cpp
    limiter/rate_limiter.cpp
    media/dash.cpp
    media/fetch.cpp
    media/ffmpeg.cpp
    media/hls.cpp
    media/http.cpp
    media/stream.cpp
    net/curl.cpp
    net/probe.cpp
    net/request.cpp
    platform/path.cpp
    platform/proxy.cpp
    queue/schedule.cpp
    resume/sidecar.cpp
    service/service.cpp
    store/sqlite.cpp
    store/store.cpp
    verify/checksum.cpp)
add_library(lanes::core ALIAS lanes_core)

# OS-specific code lives only in platform/. POSIX versions come with the Linux port.
if(WIN32)
    target_sources(lanes_core PRIVATE
        platform/console_win.cpp
        platform/credentials_win.cpp
        platform/file_win.cpp
        platform/fs_win.cpp
        platform/ipc_win.cpp
        platform/process_win.cpp
        platform/proxy_win.cpp
        platform/power_win.cpp
        platform/registry_win.cpp
        platform/system_win.cpp
        platform/win/win32.cpp)
endif()

target_include_directories(lanes_core PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(lanes_core
    PUBLIC tl::expected
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        pugixml::pugixml
        unofficial::sqlite3::sqlite3)
if(WIN32)
    target_link_libraries(lanes_core PRIVATE shell32 ole32 advapi32 winhttp powrprof)
endif()
lanes_set_compiler_options(lanes_core)
