// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lanes::core::media {

// Byte range of a resource: {offset, length}.
using ByteRange = std::pair<std::uint64_t, std::uint64_t>;

// HLS AES-128 key (other methods are rejected while parsing).
struct Key {
  std::string uri;
  std::optional<std::string> iv; // 16 bytes; absent: the segment's media sequence number
};

// One file to fetch: a media segment, or a track's initialization section.
struct Part {
  std::string url;
  std::optional<ByteRange> range;
  int key = -1;               // index into Track::keys, -1 = not encrypted
  std::uint64_t sequence = 0; // HLS media sequence number (default IV)
};

// What a stream's audio, video (or muxed) data consists of, in play order.
struct Track {
  std::optional<Part> init; // fMP4 / DASH initialization section
  std::vector<Part> parts;
  std::vector<Key> keys;
};

} // namespace lanes::core::media
