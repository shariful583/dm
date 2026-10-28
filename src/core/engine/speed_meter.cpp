// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/speed_meter.hpp"

namespace lanes::core::engine {

void SpeedMeter::add(std::uint64_t total_bytes, Clock::time_point now) {
  samples_.emplace_back(now, total_bytes);
  // Keep one sample at or beyond the window edge so the rate spans the full window.
  while (samples_.size() > 2 && now - samples_[1].first >= window_) {
    samples_.pop_front();
  }
}

double SpeedMeter::rate() const {
  if (samples_.size() < 2) {
    return 0.0;
  }
  const auto &[t0, b0] = samples_.front();
  const auto &[t1, b1] = samples_.back();
  const double seconds = std::chrono::duration<double>(t1 - t0).count();
  if (seconds <= 0.0 || b1 < b0) {
    return 0.0;
  }
  return static_cast<double>(b1 - b0) / seconds;
}

} // namespace lanes::core::engine
