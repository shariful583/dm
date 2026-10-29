// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::category {

// A file category: files whose extension is listed go to `folder`.
// Plain value type.
struct Category {
  std::string name;                    // "Video"
  std::vector<std::string> extensions; // lowercase, without dot: "mp4", "mkv"
  // Empty: <download_dir>\<name>. Relative: under download_dir. Absolute: as is.
  std::string folder;
};

// Compressed, Documents, Music, Programs, Video (IDM's defaults). Everything else is "General"
// and goes to the download folder itself.
std::vector<Category> default_categories();

inline constexpr std::string_view kGeneral = "General";

// The category whose extensions include the file's (case-insensitive), or nullptr (General).
const Category *match(const std::vector<Category> &categories, std::string_view filename);

// Name of the matching category, or "General".
std::string category_name(const std::vector<Category> &categories, std::string_view filename);

// Folder for `filename`: its category's folder, or `download_dir` for General.
std::filesystem::path folder_for(const std::vector<Category> &categories, std::string_view filename,
                                 const std::filesystem::path &download_dir);

std::string to_json(const std::vector<Category> &categories);
Result<std::vector<Category>> from_json(std::string_view json);

// "mp4, .MKV ,webm" → {"mp4", "mkv", "webm"}.
std::vector<std::string> parse_extensions(std::string_view text);

} // namespace lanes::core::category
