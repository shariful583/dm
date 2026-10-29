// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/engine/download.hpp"
#include "core/error.hpp"
#include "core/net/request.hpp"

namespace lanes::core::media {

enum class StreamKind { Hls, Dash };

// One choice a stream offers (shown by `lanes info`, the GUI's quality list).
struct StreamChoice {
  std::string kind; // "video" (possibly with audio), "audio", "text"
  int width = 0;
  int height = 0;
  std::uint64_t bandwidth = 0;
  std::string codecs;
  std::string language;
};

struct StreamInfo {
  StreamKind kind = StreamKind::Hls;
  std::vector<StreamChoice> choices;
  std::string filename; // suggested .mp4 name
};

// True for an HLS playlist or DASH manifest, judged by Content-Type or the URL's extension.
bool looks_like_stream(std::string_view url, std::string_view content_type);

// Reads the playlist/manifest and lists what it offers. Unsupported for DRM or live streams.
Result<StreamInfo> inspect_stream(const net::RequestOptions &request);

// Downloads an HLS/DASH stream into one .mp4: picks the best variant up to
// options.max_video_height plus its audio, fetches the segments in parallel (decrypting AES-128),
// and joins them with ffmpeg without re-encoding. Segments are kept in "<file>.parts\" until the
// join succeeds, so cancelling and running again resumes. Uses directory / choose_directory /
// filename / if_file_exists / connections / limiters / on_progress / on_target / request.
Result<engine::DownloadResult> download_stream(const engine::DownloadOptions &options,
                                               std::stop_token stop = {});

// The entry point for any download: probes, and sends HLS/DASH to download_stream, everything
// else to engine::download_file (reusing the probe). Thread-safe.
Result<engine::DownloadResult> download_any(engine::DownloadOptions options,
                                            std::stop_token stop = {});

} // namespace lanes::core::media
