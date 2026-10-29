// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace lanes::core::queue {

struct TimeOfDay {
  int hour = 0;   // 0-23
  int minute = 0; // 0-59
  bool operator==(const TimeOfDay &) const = default;
};

// Days of the week as a bit mask: bit 0 = Monday ... bit 6 = Sunday.
inline constexpr std::uint8_t kEveryDay = 0x7F;

// A download queue (named queues, max simultaneous downloads, scheduler).
// `enabled` means the queue is processing now; the scheduler sets it at `start_time` and clears
// it at `stop_time` on the days in `days`. Plain value type.
struct QueueSettings {
  std::string name;
  int max_active = 3;
  bool enabled = false;
  std::optional<TimeOfDay> start_time;
  std::optional<TimeOfDay> stop_time;
  std::uint8_t days = kEveryDay;
};

// "07:00", "7:5" → TimeOfDay. InvalidArgument otherwise. Thread-safe.
Result<TimeOfDay> parse_time_of_day(std::string_view text);
std::string to_string(TimeOfDay time); // "07:05"

// "2026-11-01 02:00" (local time) → unix seconds. Thread-safe.
Result<std::int64_t> parse_local_datetime(std::string_view text);

// Unix seconds → "2026-11-01 02:00" in local time. Thread-safe.
std::string format_local_datetime(std::int64_t unix_seconds);

// True if `time` on an allowed day lies in (previous, now], local time. Used by the scheduler
// tick to fire queue start/stop exactly once, including across midnight. Thread-safe.
bool crossed(TimeOfDay time, std::uint8_t days, std::chrono::local_seconds previous,
             std::chrono::local_seconds now);

// Local time now (current Windows time zone). Thread-safe.
std::chrono::local_seconds local_now();

} // namespace lanes::core::queue
