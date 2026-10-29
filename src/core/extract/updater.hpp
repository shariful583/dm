// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace lanes::core::extract {

// yt-dlp updater: sites change often, so Lanes keeps yt-dlp current itself. The
// new yt-dlp.exe is checked against the release's SHA2-256SUMS, installed atomically into
// `folder` (the user's <app data>\bin, so no administrator rights are needed; find_ytdlp()
// prefers it), and the previous copy is kept as yt-dlp.exe.old for rollback.
struct UpdateOptions {
  std::string releases_url = "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest";
  std::filesystem::path folder;        // empty = tools_dir()
  std::chrono::hours min_interval{24}; // between checks (state in folder\update.json)
  bool force = false;                  // check even if checked recently
};

struct UpdateResult {
  bool checked = false; // false: checked within min_interval, nothing done
  bool updated = false;
  std::string version; // the yt-dlp now in use
};

// Blocking (network); call from a worker thread. Thread-safe against itself only per process.
Result<UpdateResult> update_ytdlp(const UpdateOptions &options, std::stop_token stop = {});

// Goes back to the yt-dlp before the last update (or to the bundled one). NotFound if there is
// nothing to roll back to.
Result<void> rollback_ytdlp(const std::filesystem::path &folder = {});

// True if version `a` ("2026.10.26", "2026.10.26.1") is newer than `b`.
bool newer_version(std::string_view a, std::string_view b);

} // namespace lanes::core::extract
