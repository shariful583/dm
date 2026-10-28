// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/limiter/rate_limiter.hpp"

using lanes::core::limiter::RateLimiter;
using namespace std::chrono_literals;

TEST_CASE("unlimited never blocks", "[limiter]") {
  RateLimiter unlimited;
  CHECK(unlimited.rate() == 0);
  unlimited.consume(1ull << 40);
  CHECK(unlimited.available());
}

TEST_CASE("token bucket pays back debt at the rate", "[limiter]") {
  RateLimiter limiter(100'000); // burst = max(10 000, 16 KiB) = 16 KiB
  const auto t0 = RateLimiter::Clock::now();
  CHECK(limiter.available(t0));
  limiter.consume(16 * 1024 + 50'000, t0);    // 50 000 bytes of debt
  CHECK_FALSE(limiter.available(t0 + 100ms)); // 10 000 repaid
  CHECK_FALSE(limiter.available(t0 + 450ms)); // 45 000 repaid
  CHECK(limiter.available(t0 + 550ms));       // debt cleared
}

TEST_CASE("idle time does not build up a burst", "[limiter]") {
  RateLimiter limiter(1'000'000);                  // burst = 100 000
  const auto t0 = RateLimiter::Clock::now() + 10s; // long idle
  limiter.consume(100'000, t0);
  CHECK_FALSE(limiter.available(t0)); // only one burst was available, not 10 s worth
}

TEST_CASE("rate changes apply at once", "[limiter]") {
  RateLimiter limiter(1000);
  limiter.consume(1'000'000);
  CHECK_FALSE(limiter.available());
  limiter.set_rate(0);
  CHECK(limiter.available());
  limiter.set_rate(500);
  CHECK(limiter.rate() == 500);
}
