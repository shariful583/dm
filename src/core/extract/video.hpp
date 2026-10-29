// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/engine/download.hpp"
#include "core/error.hpp"
#include "core/extract/info.hpp"

namespace lanes::core::extract {

// What to take from a video page.
struct VideoRequest {
  std::string format = "best"; // select_formats() choice: "best", "720p", "audio", "mp3", ids
  std::vector<std::string> subtitles; // languages ("en", "de"); "en" also matches "en-US"
  bool thumbnail = false;
};

// Stored with queued downloads (store::DownloadRecord::video).
std::string to_json(const VideoRequest &request);
Result<VideoRequest> parse_video_request(std::string_view json);

// Downloads a video page: yt-dlp reads the page (unless `info` is given), the
// chosen formats are fetched the fastest way that works — plain HTTPS through Lanes' segmented
// engine, HLS/DASH through the stream code, anything else (or a format Lanes is refused) through
// yt-dlp's own downloader — and joined with ffmpeg without re-encoding (or converted to MP3).
// An expired format address is re-extracted once and the download continues. Parts are kept
// next to the final file until the join, so cancelling and running again resumes.
//
// options.request: the page URL, plus cookies / user agent / referrer for the site. Uses
// directory / choose_directory / filename (default: the video title) / if_file_exists /
// connections / limiters / speed_limit / on_progress / on_target. Blocking. Thread-safe.
Result<engine::DownloadResult> download_video(const engine::DownloadOptions &options,
                                              const VideoRequest &request,
                                              std::stop_token stop = {},
                                              const VideoInfo *info = nullptr);

} // namespace lanes::core::extract
