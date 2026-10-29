// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <vector>

#include "core/error.hpp"
#include "core/limiter/rate_limiter.hpp"
#include "core/media/track.hpp"
#include "core/net/request.hpp"

namespace lanes::core::media {

struct FetchProgress {
  std::uint64_t bytes = 0; // received so far, including parts kept from an earlier run
  std::size_t parts_done = 0;
  std::size_t parts_total = 0;
};

struct FetchOptions {
  net::RequestOptions request; // headers, cookies, credentials for every request (url unused)
  int connections = 8;
  int max_retries = 6; // per part
  std::chrono::milliseconds retry_delay{500};
  std::vector<std::shared_ptr<limiter::RateLimiter>> limiters;
  std::function<void(const FetchProgress &)> on_progress; // throttled to progress_interval
  std::chrono::milliseconds progress_interval{250};
};

// Downloads every part of every track into `folder` (created if missing) over parallel
// connections, decrypting HLS AES-128 parts. Each finished part is its own file, so a later call
// with the same tracks and folder keeps finished parts and fetches only the rest (resume).
// Returns the files of each track in play order (initialization section first).
// Blocking. Cancelled when `stop` is requested; partial parts are discarded.
Result<std::vector<std::vector<std::filesystem::path>>>
fetch_tracks(const std::vector<Track> &tracks, const std::filesystem::path &folder,
             const FetchOptions &options, std::stop_token stop = {});

} // namespace lanes::core::media
