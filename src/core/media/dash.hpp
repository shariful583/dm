// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/media/track.hpp"

namespace lanes::core::media::dash {

// One downloadable quality of one kind of content (a DASH Representation), with its segments.
struct Representation {
  std::string id;
  std::string kind; // "video", "audio", "text", or "" if unknown
  std::uint64_t bandwidth = 0;
  int width = 0;
  int height = 0;
  std::string codecs;
  std::string mime_type;
  std::string language;
  Track track;
};

struct Manifest {
  std::vector<Representation> representations; // of the first Period
  double duration_seconds = 0;
};

// Parses a static MPD (ISO/IEC 23009-1): BaseURL chains, SegmentTemplate ($Number$ / $Time$ /
// SegmentTimeline, with formatting), SegmentList, and single-file representations. Thread-safe.
// Unsupported for DRM (ContentProtection) and live (type="dynamic"); InvalidArgument otherwise.
Result<Manifest> parse_mpd(std::string_view xml, std::string_view mpd_url);

// "PT1H2M3.5S" → 3723.5 seconds (also days, "P1DT2H"). -1 if malformed. Thread-safe.
double parse_iso_duration(std::string_view text);

} // namespace lanes::core::media::dash
