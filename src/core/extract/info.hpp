// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::extract {

// One downloadable format of a video, as yt-dlp describes it (`yt-dlp -J`).
struct Format {
  std::string id;       // format_id, e.g. "137"
  std::string ext;      // "mp4", "m4a", "webm"
  std::string protocol; // "https", "m3u8_native", "http_dash_segments", ...
  std::string url;
  std::vector<std::pair<std::string, std::string>> http_headers;
  std::optional<std::uint64_t> filesize; // exact, or yt-dlp's estimate (see size_is_estimate)
  bool size_is_estimate = false;
  int width = 0;
  int height = 0;
  double fps = 0;
  double tbr = 0; // total kbit/s
  double abr = 0; // audio kbit/s
  std::string vcodec = "none";
  std::string acodec = "none";
  std::string note; // format_note, e.g. "1080p", "medium"
  std::string language;
  bool has_drm = false;
  // http_dash_segments: fragment URLs (absolute, or relative to fragment_base_url), the
  // initialization segment first.
  std::string fragment_base_url;
  std::vector<std::string> fragments;

  bool has_video() const { return !vcodec.empty() && vcodec != "none"; }
  bool has_audio() const { return !acodec.empty() && acodec != "none"; }
};

struct Subtitle {
  std::string ext; // "vtt", "srt", "json3", ...
  std::string url;
};

struct PlaylistEntry {
  std::string id;
  std::string url; // page URL of the entry
  std::string title;
  double duration = 0;
};

struct VideoInfo {
  std::string id;
  std::string title;
  std::string extractor; // "youtube", "generic", ...
  std::string webpage_url;
  std::string thumbnail;
  std::string uploader;
  double duration = 0;
  bool is_live = false;
  std::vector<Format> formats;
  // Language → files, uploaded subtitles only (not automatic captions).
  std::vector<std::pair<std::string, std::vector<Subtitle>>> subtitles;
  bool is_playlist = false;
  std::vector<PlaylistEntry> entries;
};

// Parses yt-dlp's JSON (-J). InvalidArgument if it is not a video or playlist. Thread-safe.
Result<VideoInfo> parse_info(std::string_view json);

// What to download for a quality choice.
struct Selection {
  const Format *video = nullptr; // may also carry the audio (combined format)
  const Format *audio = nullptr; // separate audio, or the only stream for audio choices
  std::string container;         // extension of the final file: "mp4", "mkv", "webm", "m4a", "mp3"
  bool to_mp3 = false;           // convert the audio to MP3 after downloading
  std::optional<std::uint64_t> size; // sum of the formats' sizes, when all are known
};

// Choices: "best", "<height>p" (best up to that height, e.g. "1080p"), "audio" (best audio,
// M4A preferred), "mp3" (best audio, converted), or yt-dlp format ids ("18", "137+140"). Formats
// with DRM, storyboards and live streams are never chosen. Among equal heights, plain HTTPS
// (fastest with Lanes' segmented engine) and MP4-compatible codecs win. The returned pointers
// point into `info`. NotFound / Unsupported (only DRM) when nothing fits. Thread-safe.
Result<Selection> select_formats(const VideoInfo &info, std::string_view choice);

// The quality choices a picker shows: one per available video height (best first), then
// "audio" and "mp3", each with its Selection.
std::vector<std::pair<std::string, Selection>> quality_choices(const VideoInfo &info);

// Netscape cookie file (for yt-dlp --cookies) with `cookies` ("a=1; b=2") for `url`'s site:
// "www." is dropped and subdomains included, so cookies of "www.example.com" also reach
// "example.com" and "m.example.com". Thread-safe.
std::string netscape_cookies(std::string_view cookies, std::string_view url);

// One progress line of the yt-dlp fallback downloader, as printed for Lanes' progress template:
// "LANES|<downloaded>|<total>|<total estimate>|<speed>" (yt-dlp prints "NA" for unknown values).
struct ToolProgress {
  std::uint64_t downloaded = 0;
  std::optional<std::uint64_t> total;
  double speed = 0;
};
std::optional<ToolProgress> parse_progress_line(std::string_view line);
inline constexpr std::string_view kProgressTemplate =
    "download:LANES|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|"
    "%(progress.total_bytes_estimate)s|%(progress.speed)s";

} // namespace lanes::core::extract
