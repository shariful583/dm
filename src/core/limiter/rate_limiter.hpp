// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace lanes::core::limiter {

// Token bucket for download speed limits: `rate` bytes/s refill it, holding at
// most ~100 ms worth (16 KiB minimum) so idle time cannot be saved up into a burst.
//
// Bytes are charged after they arrive, so the balance may go negative ("debt"); callers wait
// while it is not positive. This keeps the long-run average exactly at the rate without splitting
// network reads.
//
// Thread-safe: one limiter may be shared by several downloads (e.g. the global limit), and
// set_rate may be called at any time from any thread.
class RateLimiter {
public:
  using Clock = std::chrono::steady_clock;

  explicit RateLimiter(std::uint64_t bytes_per_second = 0);

  // 0 = unlimited. Takes effect immediately.
  void set_rate(std::uint64_t bytes_per_second);
  std::uint64_t rate() const;

  // True if a transfer may receive now (unlimited, or the balance is positive).
  bool available(Clock::time_point now = Clock::now());

  // Charges `bytes` that were received (no-op when unlimited).
  void consume(std::uint64_t bytes, Clock::time_point now = Clock::now());

private:
  void refill(Clock::time_point now);
  double burst() const;

  mutable std::mutex mutex_;
  std::uint64_t rate_;
  double tokens_;
  Clock::time_point last_;
};

} // namespace lanes::core::limiter
