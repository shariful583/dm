// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/queue/schedule.hpp"

#include <charconv>
#include <cstdio>

namespace lanes::core::queue {

namespace {

using namespace std::chrono;

bool parse_int(std::string_view text, int &out) {
  const char *end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, out);
  return !text.empty() && ec == std::errc{} && ptr == end; // all of it, nothing else
}

// Monday = 0 ... Sunday = 6.
int weekday_index(local_days day) {
  const unsigned iso = weekday{day}.iso_encoding(); // Monday = 1 ... Sunday = 7
  return static_cast<int>(iso) - 1;
}

} // namespace

Result<TimeOfDay> parse_time_of_day(std::string_view text) {
  const auto colon = text.find(':');
  TimeOfDay t;
  if (colon == std::string_view::npos || !parse_int(text.substr(0, colon), t.hour) ||
      !parse_int(text.substr(colon + 1), t.minute) || t.hour < 0 || t.hour > 23 || t.minute < 0 ||
      t.minute > 59) {
    return make_error(ErrorCode::InvalidArgument,
                      "invalid time '" + std::string(text) + "' (use HH:MM, e.g. 07:30)");
  }
  return t;
}

std::string to_string(TimeOfDay time) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", time.hour, time.minute);
  return buf;
}

Result<std::int64_t> parse_local_datetime(std::string_view text) {
  const auto invalid = [&] {
    return make_error(ErrorCode::InvalidArgument,
                      "invalid date/time '" + std::string(text) + "' (use \"YYYY-MM-DD HH:MM\")");
  };
  const auto space = text.find(' ');
  if (space == std::string_view::npos) {
    return invalid();
  }
  const std::string_view date = text.substr(0, space);
  int y = 0;
  int m = 0;
  int d = 0;
  const auto dash1 = date.find('-');
  const auto dash2 = date.find('-', dash1 + 1);
  if (dash1 == std::string_view::npos || dash2 == std::string_view::npos ||
      !parse_int(date.substr(0, dash1), y) ||
      !parse_int(date.substr(dash1 + 1, dash2 - dash1 - 1), m) ||
      !parse_int(date.substr(dash2 + 1), d)) {
    return invalid();
  }
  const auto time = parse_time_of_day(text.substr(space + 1));
  if (!time) {
    return invalid();
  }
  const year_month_day ymd{year{y}, month{static_cast<unsigned>(m)}, day{static_cast<unsigned>(d)}};
  if (!ymd.ok()) {
    return invalid();
  }
  const local_seconds local = local_days{ymd} + hours{time->hour} + minutes{time->minute};
  // Earliest of an ambiguous (DST fall-back) time; a skipped (spring-forward) time moves on.
  const sys_seconds utc = current_zone()->to_sys(local, choose::earliest);
  return utc.time_since_epoch().count();
}

std::string format_local_datetime(std::int64_t unix_seconds) {
  const local_seconds local = current_zone()->to_local(sys_seconds{seconds{unix_seconds}});
  const local_days day = floor<days>(local);
  const year_month_day ymd{day};
  const hh_mm_ss hms{local - day};
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()));
  return buf;
}

bool crossed(TimeOfDay time, std::uint8_t days, local_seconds previous, local_seconds now) {
  if (now <= previous) {
    return false;
  }
  // Check the occurrence on each calendar day from previous's day to now's day.
  for (local_days day = floor<std::chrono::days>(previous); day <= floor<std::chrono::days>(now);
       day += std::chrono::days{1}) {
    const local_seconds at = day + hours{time.hour} + minutes{time.minute};
    if (at > previous && at <= now && (days & (1u << weekday_index(day))) != 0) {
      return true;
    }
  }
  return false;
}

local_seconds local_now() { return floor<seconds>(current_zone()->to_local(system_clock::now())); }

} // namespace lanes::core::queue
