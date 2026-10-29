// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/extract/info.hpp"

namespace lanes::core::extract {

// yt-dlp.exe to run: LANES_YTDLP if set, else the updated copy in <app data>\bin, else the one
// next to the running program. Never searched on PATH. NotFound otherwise. Thread-safe.
Result<std::filesystem::path> find_ytdlp();

// The JavaScript runtime (deno.exe) yt-dlp needs for YouTube: next to the program, if present.
std::optional<std::filesystem::path> find_js_runtime();

// Where Lanes keeps its updated yt-dlp: <app data>\bin.
std::filesystem::path tools_dir();

// What the page request carries: cookies for logged-in or age-restricted videos ("a=1; b=2",
// written to a temporary Netscape cookie file for yt-dlp and deleted afterwards), user agent and
// referrer.
struct PageRequest {
  std::string url;
  std::string cookies;
  std::string user_agent;
  std::string referer;
};

struct ExtractOptions {
  PageRequest page;
  bool playlist = false; // a video address with a playlist: the playlist (entries come flat)
  std::chrono::seconds timeout{120};
};

// Runs `yt-dlp -J` for the page and parses the result. Errors carry yt-dlp's
// own message (e.g. "Sign in to confirm your age"). Blocking; Cancelled on `stop`. Thread-safe.
Result<VideoInfo> extract(const ExtractOptions &options, std::stop_token stop = {});

// yt-dlp's own downloader, for formats Lanes cannot fetch itself: `-f
// <format_ids>` into `output` (merged into its extension's container), progress reported per line.
Result<void> download_with_ytdlp(const PageRequest &page, const std::string &format_ids,
                                 const std::filesystem::path &output,
                                 const std::function<void(const ToolProgress &)> &on_progress,
                                 std::stop_token stop = {});

// `yt-dlp --version` of `exe`, e.g. "2026.10.26".
Result<std::string> ytdlp_version(const std::filesystem::path &exe);

} // namespace lanes::core::extract
