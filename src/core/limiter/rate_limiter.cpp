// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/limiter/rate_limiter.hpp"

#include <algorithm>

namespace lanes::core::limiter {

namespace {
constexpr double kBurstSeconds = 0.1;
constexpr double kMinBurst = 16 * 1024;
} // namespace

RateLimiter::RateLimiter(std::uint64_t bytes_per_second)
    : rate_(bytes_per_second), tokens_(0), last_(Clock::now()) {
  tokens_ = burst();
}

double RateLimiter::burst() const {
  return std::max(static_cast<double>(rate_) * kBurstSeconds, kMinBurst);
}

void RateLimiter::refill(Clock::time_point now) {
  if (now > last_) {
    const double seconds = std::chrono::duration<double>(now - last_).count();
    tokens_ = std::min(burst(), tokens_ + static_cast<double>(rate_) * seconds);
    last_ = now;
  }
}

void RateLimiter::set_rate(std::uint64_t bytes_per_second) {
  std::lock_guard lock(mutex_);
  refill(Clock::now());
  rate_ = bytes_per_second;
  tokens_ = std::min(tokens_, burst());
}

std::uint64_t RateLimiter::rate() const {
  std::lock_guard lock(mutex_);
  return rate_;
}

bool RateLimiter::available(Clock::time_point now) {
  std::lock_guard lock(mutex_);
  if (rate_ == 0) {
    return true;
  }
  refill(now);
  return tokens_ > 0;
}

void RateLimiter::consume(std::uint64_t bytes, Clock::time_point now) {
  std::lock_guard lock(mutex_);
  if (rate_ == 0) {
    return;
  }
  refill(now);
  tokens_ -= static_cast<double>(bytes);
}

} // namespace lanes::core::limiter
