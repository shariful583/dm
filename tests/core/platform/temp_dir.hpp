// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include "core/platform/path.hpp"

// A fresh directory under the system temp folder, removed (with long-path support) on destruction.
class TempDir {
public:
  TempDir() {
    std::random_device rd;
    path_ = std::filesystem::temp_directory_path() /
            ("lanes-test-" + std::to_string(rd()) + std::to_string(rd()));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(lanes::core::platform::to_long_path(path_), ec);
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};
