// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::resume {

// A byte range of the download and how many of its bytes are durably on disk, from `start` on.
struct SavedSegment {
  std::uint64_t start = 0;
  std::uint64_t end = 0; // inclusive
  std::uint64_t done = 0;

  std::uint64_t length() const { return end - start + 1; }
};

// What `<file>.lanes.json` records so a download can continue after a pause, crash or reboot
// Never holds secrets: no cookies, credentials, auth headers or POST bodies,
// and the URL without user:password. Plain value type; not synchronized.
struct ResumeState {
  static constexpr int kVersion = 1;

  std::string url;       // as requested, credentials removed
  std::string final_url; // after redirects, credentials removed
  std::string filename;  // final file name (sanitized)
  std::uint64_t size = 0;
  std::string etag;
  std::string last_modified;
  std::vector<SavedSegment> segments; // sorted by start, tiling [0, size)

  std::uint64_t done_bytes() const;
};

// `<final path>.lanes.json`.
std::filesystem::path sidecar_path(const std::filesystem::path &final_path);

std::string to_json(const ResumeState &state);

// Parses and validates (version, segments sorted, in bounds, tiling the file exactly, done within
// each segment). InvalidArgument for anything malformed: such a sidecar cannot be trusted.
Result<ResumeState> from_json(std::string_view json);

// Atomically replaces the sidecar (temp file + flush + rename).
Result<void> save(const std::filesystem::path &sidecar, const ResumeState &state);

// nullopt if there is no sidecar; an error if it exists but is unreadable or invalid.
Result<std::optional<ResumeState>> load(const std::filesystem::path &sidecar);

} // namespace lanes::core::resume
