// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli/format.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace lanes::cli {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

std::string scaled(double value, const char *suffix) {
  static constexpr std::array<const char *, 5> kUnits{"B", "KiB", "MiB", "GiB", "TiB"};
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < kUnits.size()) {
    value /= 1024.0;
    ++unit;
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s%s" : "%.1f %s%s", value, kUnits[unit],
                suffix);
  return buf;
}

// Cuts `text` to `cells` display cells, ending in "…" when shortened.
std::string fit(std::string_view text, std::size_t cells) {
  if (display_width(text) <= cells) {
    return std::string(text);
  }
  if (cells == 0) {
    return {};
  }
  std::string out;
  std::size_t used = 0;
  for (std::size_t i = 0; i < text.size() && used + 1 < cells;) {
    std::size_t len = 1;
    const auto c = static_cast<unsigned char>(text[i]);
    if (c >= 0xF0) {
      len = 4;
    } else if (c >= 0xE0) {
      len = 3;
    } else if (c >= 0xC0) {
      len = 2;
    }
    out.append(text.substr(i, len));
    i += len;
    ++used;
  }
  return out + "\xE2\x80\xA6"; // …
}

} // namespace

core::Result<std::uint64_t> parse_size(std::string_view text) {
  const auto invalid = [&] {
    return core::make_error(core::ErrorCode::InvalidArgument,
                            "invalid size '" + std::string(text) + "' (examples: 500K, 2M, 1.5G)");
  };
  std::string_view t = text;
  while (!t.empty() && t.back() == ' ') {
    t.remove_suffix(1);
  }
  std::size_t digits = 0;
  while (digits < t.size() && ((t[digits] >= '0' && t[digits] <= '9') || t[digits] == '.')) {
    ++digits;
  }
  if (digits == 0) {
    return invalid();
  }
  double number = 0;
  if (std::from_chars(t.data(), t.data() + digits, number).ec != std::errc{} || number < 0) {
    return invalid();
  }
  std::string unit;
  for (const char c : t.substr(digits)) {
    if (c != ' ') {
      unit += lower(c);
    }
  }
  double factor = 1;
  if (unit.empty() || unit == "b") {
    factor = 1;
  } else if (unit == "k" || unit == "kb" || unit == "kib") {
    factor = 1024.0;
  } else if (unit == "m" || unit == "mb" || unit == "mib") {
    factor = 1024.0 * 1024;
  } else if (unit == "g" || unit == "gb" || unit == "gib") {
    factor = 1024.0 * 1024 * 1024;
  } else {
    return invalid();
  }
  return static_cast<std::uint64_t>(std::llround(number * factor));
}

std::string format_bytes(std::uint64_t bytes) { return scaled(static_cast<double>(bytes), ""); }

std::string format_speed(double bytes_per_second) {
  return scaled(std::max(bytes_per_second, 0.0), "/s");
}

std::string format_duration(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0 || seconds > 99.0 * 3600) {
    return "--";
  }
  if (seconds < 9.95) {
    char short_buf[16];
    std::snprintf(short_buf, sizeof(short_buf), "%.1fs", seconds);
    return short_buf;
  }
  const auto total = static_cast<long long>(std::llround(seconds));
  const long long h = total / 3600;
  const long long m = (total / 60) % 60;
  const long long s = total % 60;
  char buf[32];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
  } else if (m > 0) {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
  } else {
    std::snprintf(buf, sizeof(buf), "%llds", s);
  }
  return buf;
}

std::size_t display_width(std::string_view utf8) {
  return static_cast<std::size_t>(std::count_if(utf8.begin(), utf8.end(), [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0) != 0x80; // count lead bytes only
  }));
}

std::string segment_map(const std::vector<core::engine::SegmentProgress> &segments,
                        std::uint64_t total, int width) {
  if (total == 0 || width <= 0) {
    return {};
  }
  std::string out;
  const auto cells = static_cast<std::uint64_t>(width);
  for (std::uint64_t i = 0; i < cells; ++i) {
    const std::uint64_t from = i * total / cells;
    const std::uint64_t to = (i + 1) * total / cells; // exclusive
    std::uint64_t done = 0;
    for (const auto &s : segments) {
      // Each segment's received bytes are contiguous from its start.
      const std::uint64_t got_from = s.start;
      const std::uint64_t got_to = s.start + s.received;
      const std::uint64_t lo = std::max(from, got_from);
      const std::uint64_t hi = std::min(to, got_to);
      if (hi > lo) {
        done += hi - lo;
      }
    }
    if (to > from && done >= to - from) {
      out += "\xE2\x96\x88"; // █
    } else if (done > 0) {
      out += "\xE2\x96\x92"; // ▒
    } else {
      out += "\xE2\x96\x91"; // ░
    }
  }
  return out;
}

std::string progress_line(const core::engine::DownloadProgress &p, std::string_view name,
                          int columns) {
  const std::size_t room = static_cast<std::size_t>(std::max(columns - 1, 20));
  std::string stats;
  int active = 0;
  for (const auto &s : p.segments) {
    if (!s.end || s.received < *s.end - s.start + 1) {
      ++active;
    }
  }
  char percent[16] = "";
  if (p.total && *p.total > 0) {
    std::snprintf(percent, sizeof(percent), "%5.1f%%",
                  100.0 * static_cast<double>(p.downloaded) / static_cast<double>(*p.total));
    stats = std::string(percent) + "  " + format_bytes(p.downloaded) + " / " +
            format_bytes(*p.total) + "  " + format_speed(p.bytes_per_second);
    const double left = static_cast<double>(*p.total - std::min(*p.total, p.downloaded));
    stats += "  ETA " + format_duration(p.bytes_per_second > 0 ? left / p.bytes_per_second : -1);
  } else {
    stats = format_bytes(p.downloaded) + "  " + format_speed(p.bytes_per_second);
  }
  if (active > 1) {
    stats += "  " + std::to_string(active) + " conn";
  }

  const std::string short_name = fit(name, 24);
  const std::size_t name_cells = display_width(short_name);
  const std::size_t stats_cells = display_width(stats);
  std::string line;
  if (p.total && *p.total > 0) {
    const std::size_t fixed = name_cells + 2 + 2 + stats_cells;
    if (room > fixed + 10) {
      const int bar = static_cast<int>(std::min<std::size_t>(room - fixed, 40));
      line = short_name + "  " + segment_map(p.segments, *p.total, bar) + "  " + stats;
    } else {
      line = short_name + "  " + stats;
    }
  } else {
    line = short_name + "  " + stats;
  }
  return fit(line, room);
}

} // namespace lanes::cli
