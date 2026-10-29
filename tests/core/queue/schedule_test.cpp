// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/queue/schedule.hpp"

using namespace lanes::core::queue;
using namespace std::chrono;

namespace {
// 2026-11-02 is a Monday.
local_seconds at(int day, int hour, int minute) {
  return local_days{year{2026} / November / day} + hours{hour} + minutes{minute};
}
} // namespace

TEST_CASE("time of day parsing", "[queue][schedule]") {
  CHECK(*parse_time_of_day("07:30") == TimeOfDay{7, 30});
  CHECK(*parse_time_of_day("0:05") == TimeOfDay{0, 5});
  CHECK(to_string(TimeOfDay{7, 5}) == "07:05");
  CHECK_FALSE(parse_time_of_day("24:00"));
  CHECK_FALSE(parse_time_of_day("12:60"));
  CHECK_FALSE(parse_time_of_day("7"));
  CHECK_FALSE(parse_time_of_day("7:3x"));
  CHECK_FALSE(parse_time_of_day(""));
}

TEST_CASE("crossed fires once per occurrence", "[queue][schedule]") {
  const TimeOfDay seven{7, 0};
  CHECK(crossed(seven, kEveryDay, at(2, 6, 59), at(2, 7, 0)));
  CHECK_FALSE(crossed(seven, kEveryDay, at(2, 7, 0), at(2, 7, 1))); // already fired
  CHECK_FALSE(crossed(seven, kEveryDay, at(2, 5, 0), at(2, 6, 0)));
  // Across midnight: 23:59 → 00:01 crosses 00:00.
  CHECK(crossed(TimeOfDay{0, 0}, kEveryDay, at(2, 23, 59), at(3, 0, 1)));
  // A long gap (engine asleep) still catches the time.
  CHECK(crossed(seven, kEveryDay, at(2, 20, 0), at(3, 8, 0)));
  CHECK_FALSE(crossed(seven, kEveryDay, at(2, 7, 0), at(2, 7, 0)));
}

TEST_CASE("crossed honours the days of the week", "[queue][schedule]") {
  const TimeOfDay seven{7, 0};
  const std::uint8_t monday = 1u << 0;
  const std::uint8_t weekend = (1u << 5) | (1u << 6);
  CHECK(crossed(seven, monday, at(2, 6, 0), at(2, 8, 0)));        // Monday
  CHECK_FALSE(crossed(seven, monday, at(3, 6, 0), at(3, 8, 0)));  // Tuesday
  CHECK(crossed(seven, weekend, at(7, 6, 0), at(7, 8, 0)));       // Saturday
  CHECK_FALSE(crossed(seven, weekend, at(6, 6, 0), at(6, 8, 0))); // Friday
}

TEST_CASE("local date/time round trip", "[queue][schedule]") {
  const auto t = parse_local_datetime("2026-11-01 02:30");
  REQUIRE(t);
  CHECK(format_local_datetime(*t) == "2026-11-01 02:30");
  CHECK_FALSE(parse_local_datetime("2026-13-01 02:30"));
  CHECK_FALSE(parse_local_datetime("2026-11-31 02:30"));
  CHECK_FALSE(parse_local_datetime("tomorrow"));
}
