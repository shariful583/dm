# SPDX-License-Identifier: GPL-3.0-or-later
#
# lanes_set_compiler_options(<target>)
# Applies the project-wide warning level, conformance flags and platform defines.

function(lanes_set_compiler_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc)
        if(LANES_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/WX>)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(LANES_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(WIN32)
        # Windows 10+, wide-char Win32 APIs, no min/max macros.
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()
