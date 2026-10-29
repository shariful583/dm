// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/media/track.hpp"

namespace lanes::core::media::hls {

// One quality of a master playlist (#EXT-X-STREAM-INF).
struct Variant {
  std::string uri; // absolute
  std::uint64_t bandwidth = 0;
  int width = 0;
  int height = 0;
  std::string codecs;
  std::string audio_group; // AUDIO="..." → a separate audio rendition
};

// An alternative rendition (#EXT-X-MEDIA), e.g. a separate audio track.
struct Rendition {
  std::string type; // AUDIO, SUBTITLES, ...
  std::string group_id;
  std::string name;
  std::string language;
  std::string uri; // absolute; empty = muxed into the variant
  bool is_default = false;
};

struct MasterPlaylist {
  std::vector<Variant> variants;
  std::vector<Rendition> renditions;
};

// True for a master playlist (has #EXT-X-STREAM-INF), false for a media playlist.
bool is_master(std::string_view text);

// Parses playlists (RFC 8216); relative URIs are resolved against `base_url`. Thread-safe.
// Media playlists: Unsupported for DRM (SAMPLE-AES, non-identity KEYFORMAT) and live playlists
// (no #EXT-X-ENDLIST); InvalidArgument for malformed input.
Result<MasterPlaylist> parse_master(std::string_view text, std::string_view base_url);
Result<Track> parse_media(std::string_view text, std::string_view base_url);

// Resolves `uri` against `base` like a browser (absolute stays absolute). Thread-safe.
std::string resolve_url(std::string_view base, std::string_view uri);

} // namespace lanes::core::media::hls
