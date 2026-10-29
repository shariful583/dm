// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/limiter/rate_limiter.hpp"
#include "core/net/probe.hpp"
#include "core/net/request.hpp"
#include "core/verify/checksum.hpp"

namespace lanes::core::engine {

// One byte range of the file and how much of it has arrived (IDM's connection map).
struct SegmentProgress {
  std::uint64_t start = 0;
  std::optional<std::uint64_t> end; // inclusive; nullopt = until the server stops (unknown size)
  std::uint64_t received = 0;
};

struct DownloadProgress {
  std::uint64_t downloaded = 0;
  std::optional<std::uint64_t> total;
  double bytes_per_second = 0;
  std::vector<SegmentProgress> segments; // one per connection, in file order
};

// What to do when the final file already exists (a resumed .part is never a conflict).
enum class ExistingFile {
  Overwrite, // replace it when the download completes
  Rename,    // use "name (2).ext", "name (3).ext", ...
  Fail,      // AlreadyExists
};

struct DownloadOptions {
  net::RequestOptions request;
  // Created if missing. Empty: `choose_directory(final file name)` decides (file categories).
  std::filesystem::path directory;
  std::function<std::filesystem::path(const std::string &filename)> choose_directory;
  ExistingFile if_file_exists = ExistingFile::Overwrite;
  // Without request.auth, use the login saved for the URL's host (Windows Credential Manager).
  // curl sends it only when the server asks (401).
  bool use_saved_logins = true;
  // File name inside `directory`; empty = the server's suggestion. Always sanitized.
  std::string filename;
  // Result of an earlier probe (e.g. from the add dialog); probed here if empty. POST downloads
  // are never probed: the response itself provides size and name.
  std::optional<net::ProbeResult> probe;

  // Parallel connections (1-32), each fetching its own byte range over its own TCP connection.
  // Used when the server supports ranges and the size is known; otherwise one connection.
  int connections = 8;
  // Segments are never made smaller than this, so small files use fewer connections.
  std::uint64_t min_segment_size = 1 << 20;
  // Cap on simultaneous connections to one host (CURLMOPT_MAX_HOST_CONNECTIONS).
  int max_connections_per_host = 16;

  // Dynamic segmentation. When a connection finishes, it takes the second half of
  // the segment with the most bytes left, if that is more than `min_split_size`.
  bool split_when_idle = true;
  std::uint64_t min_split_size = 1 << 20;
  // A connection far slower than the others (< 1/3 of the median speed, > 2 s of work left) is
  // restarted on a fresh connection from where it stopped; at most 3 times per byte range.
  bool replace_slow_connections = true;

  // Resume. Range-capable downloads of known size keep `<file>.lanes.json` next
  // to `<file>.part`, checkpointed every `checkpoint_interval` and on cancel/failure (data flushed
  // to disk before the sidecar is replaced). With `resume`, an existing pair continues where it
  // stopped if the server's file is unchanged (size, ETag, Last-Modified); otherwise the result is
  // RemoteChanged, or a fresh start with `restart_if_changed`. To refresh an expired address,
  // run again with the new URL and the same `filename`.
  bool resume = true;
  bool restart_if_changed = false;
  std::chrono::milliseconds checkpoint_interval{1000};

  // Retries. A failed range keeps its bytes and is retried on a new connection
  // after `retry_delay`, doubling each time (±25 %, at most 30 s; a server's Retry-After wins).
  // A range gives up after `max_retries` consecutive failures without progress. Retried: network
  // errors, timeouts, early closes, HTTP 408/429/500/502/503/504. 429/503 and refused/dropped
  // connections lower the connection count to what the server still accepts.
  int max_retries = 10;
  std::chrono::milliseconds retry_delay{1000};

  // Speed limits. `speed_limit` caps this download (bytes/s, 0 = none);
  // `limiters` are shared buckets (e.g. the global limit) whose rate may change at any time.
  // All apply at once. A connection pauses while any bucket is empty; nothing sleeps.
  std::uint64_t speed_limit = 0;
  std::vector<std::shared_ptr<limiter::RateLimiter>> limiters;

  // Verified before the .part file is renamed; a mismatch fails with ChecksumMismatch and keeps
  // the .part file (without a resume sidecar: its content is wrong, a rerun starts over).
  std::optional<verify::Checksum> checksum;

  // Called on the download thread at most every `progress_interval`, and once at the end.
  std::function<void(const DownloadProgress &)> on_progress;
  // Called on the download thread once the final file path is decided (before data arrives).
  std::function<void(const std::filesystem::path &final_path)> on_target;
  std::chrono::milliseconds progress_interval{250};
};

struct DownloadResult {
  std::filesystem::path path; // final file
  std::uint64_t size = 0;
  int connections = 1;            // connections started with (1 after a fallback to one connection)
  int splits = 0;                 // segments split for an idle connection
  int restarts = 0;               // slow connections replaced
  std::uint64_t resumed_from = 0; // bytes already on disk from an earlier run
  int retries = 0;                // failed requests (probe or ranges) that were retried
  int connection_limit = 0;       // connections allowed at the end (lowered by 429/503 etc.)
  net::ProbeResult info;
};

// Downloads into `<path>.part`, then renames it to the final path (replacing an existing file).
// Blocking: runs every connection on the calling thread (one curl multi event loop). Request
// `stop` to cancel; the result is then Cancelled and the .part file is kept for resuming.
//
// A server that answers a range request with 200 (ignoring the range) is downloaded again over a
// single connection.
//
// Errors: those of net::probe, Http (status in Error::http_status), Network/Timeout, NoSpace and
// FileTooLarge (checked before writing), Io/AccessDenied for file problems, InvalidArgument for
// out-of-range options, RemoteChanged (see `resume`), LinkExpired (HTTP 403/410 once data exists:
// the address needs refreshing; the data is kept).
Result<DownloadResult> download_file(const DownloadOptions &options, std::stop_token stop = {});

} // namespace lanes::core::engine
