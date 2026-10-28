// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/engine/download.hpp"
#include "core/error.hpp"

namespace lanes::cli {

// "2M", "1.5G", "500k", "64KiB", "100" (bytes). Binary units (K = 1024). Thread-safe.
core::Result<std::uint64_t> parse_size(std::string_view text);

std::string format_bytes(std::uint64_t bytes);     // "999 B", "1.5 KiB", "12.3 MiB"
std::string format_speed(double bytes_per_second); // "12.3 MiB/s"
std::string format_duration(double seconds);       // "3.2s", "45s", "2:05", "1:02:03"; "--" unknown

// Display width of UTF-8 text in console cells (one per code point; enough for our output).
std::size_t display_width(std::string_view utf8);

// IDM-style connection map of `width` cells over the file: █ done, ▒ partly, ░ not yet.
std::string segment_map(const std::vector<core::engine::SegmentProgress> &segments,
                        std::uint64_t total, int width);

// One-line progress for a console `columns` wide (always fits in columns - 1 cells):
// "name  ███▒░░  45.2%  123.4 MiB / 273.0 MiB  12.3 MiB/s  ETA 0:12  8 conn"
std::string progress_line(const core::engine::DownloadProgress &progress, std::string_view name,
                          int columns);

} // namespace lanes::cli
