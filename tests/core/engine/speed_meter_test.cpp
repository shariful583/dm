// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/engine/speed_meter.hpp"

using lanes::core::engine::SpeedMeter;
using namespace std::chrono_literals;

TEST_CASE("SpeedMeter averages over its window", "[engine][speed]") {
  SpeedMeter meter(2000ms);
  const auto t0 = SpeedMeter::Clock::time_point{} + 1h;
  CHECK(meter.rate() == 0.0);
  meter.add(0, t0);
  CHECK(meter.rate() == 0.0);
  meter.add(1000, t0 + 1s);
  CHECK(meter.rate() == Catch::Approx(1000.0));
  meter.add(3000, t0 + 2s);
  CHECK(meter.rate() == Catch::Approx(1500.0));

  // Old samples fall out: only the last ~2 s count.
  meter.add(13000, t0 + 3s); // 10 000 B/s in the last second
  meter.add(23000, t0 + 4s);
  CHECK(meter.rate() == Catch::Approx(10000.0));

  // Stall: rate decays toward zero.
  meter.add(23000, t0 + 6s);
  meter.add(23000, t0 + 7s);
  CHECK(meter.rate() < 6000.0);
}
