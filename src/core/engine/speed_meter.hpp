// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <utility>

namespace lanes::core::engine {

// Transfer rate over a sliding window, from cumulative byte counts. Not synchronized.
class SpeedMeter {
public:
  using Clock = std::chrono::steady_clock;

  explicit SpeedMeter(std::chrono::milliseconds window = std::chrono::milliseconds(2000))
      : window_(window) {}

  // Records that `total_bytes` have been transferred so far, at `now`.
  void add(std::uint64_t total_bytes, Clock::time_point now = Clock::now());

  // Bytes per second across the window (0 until two samples exist).
  double rate() const;

private:
  std::chrono::milliseconds window_;
  std::deque<std::pair<Clock::time_point, std::uint64_t>> samples_;
};

} // namespace lanes::core::engine
