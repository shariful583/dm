// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

#include "core/error.hpp"
#include "core/net/probe.hpp"
#include "core/net/request.hpp"

namespace lanes::core::engine {

struct DownloadProgress {
  std::uint64_t downloaded = 0;
  std::optional<std::uint64_t> total;
  double bytes_per_second = 0;
};

struct DownloadOptions {
  net::RequestOptions request;
  std::filesystem::path directory; // created if missing
  // File name inside `directory`; empty = the server's suggestion. Always sanitized.
  std::string filename;
  // Result of an earlier probe (e.g. from the add dialog); probed here if empty. POST downloads
  // are never probed: the response itself provides size and name.
  std::optional<net::ProbeResult> probe;
  // Called on the download thread at most every `progress_interval`, and once at the end.
  std::function<void(const DownloadProgress &)> on_progress;
  std::chrono::milliseconds progress_interval{250};
};

struct DownloadResult {
  std::filesystem::path path; // final file
  std::uint64_t size = 0;
  net::ProbeResult info;
};

// Downloads into `<path>.part`, then renames it to the final path (replacing an existing file).
// Blocking: runs the transfer on the calling thread. Request `stop` to cancel; the result is then
// Cancelled and the .part file is kept for resuming.
//
// Errors: those of net::probe, Http (status in Error::http_status), Network/Timeout, NoSpace and
// FileTooLarge (checked before writing), Io/AccessDenied for file problems.
//
// Single connection for now; segmented downloads arrive in step 5.
Result<DownloadResult> download_file(const DownloadOptions &options, std::stop_token stop = {});

} // namespace lanes::core::engine
