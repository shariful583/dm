// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace lanes::core::platform {

// UTF-8 ↔ std::filesystem::path (paths are fs::path, UTF-8 everywhere else).
// Thread-safe.
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path &path);

// Absolute, normalized path usable beyond MAX_PATH. Windows: adds the `\\?\` prefix
// (`\\?\UNC\server\share\...` for UNC paths); paths already in `\\?\` or `\\.\` form are returned
// unchanged. Other platforms: absolute + lexically normal. Never fails; if the path cannot be made
// absolute it is returned as given. Thread-safe, but relative paths depend on the process's
// current directory.
std::filesystem::path to_long_path(const std::filesystem::path &path);

// Makes `name` (UTF-8, a single path component, e.g. from Content-Disposition or a URL) safe as a
// file name on Windows — and therefore everywhere:
//  - control characters and < > : " / \ | ? * become '_'; invalid UTF-8 bytes become '_'
//  - leading spaces and trailing dots/spaces are removed (Windows drops them silently)
//  - reserved device names (CON, PRN, AUX, NUL, COM0-9, LPT0-9, also with an extension such as
//    "con.txt") get a '_' prefix
//  - the result is cut to at most `max_utf16_units` UTF-16 code units, keeping the extension
//  - an empty result becomes `fallback`
// Thread-safe (pure function).
std::string sanitize_filename(std::string_view name, std::string_view fallback = "download",
                              std::size_t max_utf16_units = 255);

} // namespace lanes::core::platform
