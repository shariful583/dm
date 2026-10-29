// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/category/category.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "core/platform/path.hpp"

namespace lanes::core::category {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

std::string extension_of(std::string_view filename) {
  const auto dot = filename.rfind('.');
  if (dot == std::string_view::npos || dot + 1 == filename.size()) {
    return {};
  }
  return lower(filename.substr(dot + 1));
}

} // namespace

std::vector<Category> default_categories() {
  return {
      {"Compressed",
       {"zip", "rar", "7z", "gz", "tgz", "bz2", "xz", "zst", "tar", "cab", "arj", "lzh", "ace",
        "iso", "img", "dmg"},
       ""},
      {"Documents",
       {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "pps", "odt", "ods", "odp", "rtf",
        "txt", "csv", "epub", "mobi", "djvu", "md"},
       ""},
      {"Music",
       {"mp3", "m4a", "aac", "flac", "wav", "wma", "ogg", "opus", "aif", "aiff", "ape", "mid",
        "midi", "ra"},
       ""},
      {"Programs",
       {"exe", "msi", "msix", "appx", "apk", "bat", "cmd", "deb", "rpm", "appimage", "jar"},
       ""},
      {"Video",
       {"mp4", "mkv", "webm", "avi", "mov", "wmv", "flv", "m4v", "mpg", "mpeg", "3gp", "ts", "m2ts",
        "vob", "ogv"},
       ""},
  };
}

const Category *match(const std::vector<Category> &categories, std::string_view filename) {
  const std::string ext = extension_of(filename);
  if (ext.empty()) {
    return nullptr;
  }
  for (const auto &c : categories) {
    if (std::find(c.extensions.begin(), c.extensions.end(), ext) != c.extensions.end()) {
      return &c;
    }
  }
  return nullptr;
}

std::string category_name(const std::vector<Category> &categories, std::string_view filename) {
  const Category *c = match(categories, filename);
  return c != nullptr ? c->name : std::string(kGeneral);
}

std::filesystem::path folder_for(const std::vector<Category> &categories, std::string_view filename,
                                 const std::filesystem::path &download_dir) {
  const Category *c = match(categories, filename);
  if (c == nullptr) {
    return download_dir;
  }
  if (c->folder.empty()) {
    return download_dir / platform::path_from_utf8(platform::sanitize_filename(c->name));
  }
  const std::filesystem::path folder = platform::path_from_utf8(c->folder);
  return folder.is_absolute() ? folder : download_dir / folder;
}

std::string to_json(const std::vector<Category> &categories) {
  nlohmann::json j = nlohmann::json::array();
  for (const auto &c : categories) {
    j.push_back({{"name", c.name}, {"extensions", c.extensions}, {"folder", c.folder}});
  }
  return j.dump();
}

Result<std::vector<Category>> from_json(std::string_view json) {
  const auto j = nlohmann::json::parse(json, nullptr, false);
  if (!j.is_array()) {
    return make_error(ErrorCode::InvalidArgument, "categories must be a JSON array");
  }
  std::vector<Category> out;
  try {
    for (const auto &c : j) {
      Category category;
      category.name = c.at("name").get<std::string>();
      for (const auto &e : c.at("extensions")) {
        category.extensions.push_back(lower(e.get<std::string>()));
      }
      category.folder = c.value("folder", std::string());
      if (category.name.empty()) {
        return make_error(ErrorCode::InvalidArgument, "a category needs a name");
      }
      out.push_back(std::move(category));
    }
  } catch (const nlohmann::json::exception &e) {
    return make_error(ErrorCode::InvalidArgument, std::string("bad categories: ") + e.what());
  }
  return out;
}

std::vector<std::string> parse_extensions(std::string_view text) {
  std::vector<std::string> out;
  std::string current;
  const auto flush = [&] {
    while (!current.empty() && current.front() == '.') {
      current.erase(0, 1);
    }
    if (!current.empty() && std::find(out.begin(), out.end(), current) == out.end()) {
      out.push_back(current);
    }
    current.clear();
  };
  for (const char c : text) {
    if (c == ',' || c == ' ' || c == ';') {
      flush();
    } else {
      current += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
  }
  flush();
  return out;
}

} // namespace lanes::core::category
