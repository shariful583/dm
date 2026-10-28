// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/net/probe.hpp"
#include "core/net/request.hpp"

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

struct DownloadOptions {
  net::RequestOptions request;
  std::filesystem::path directory; // created if missing
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

  // Called on the download thread at most every `progress_interval`, and once at the end.
  std::function<void(const DownloadProgress &)> on_progress;
  std::chrono::milliseconds progress_interval{250};
};

struct DownloadResult {
  std::filesystem::path path; // final file
  std::uint64_t size = 0;
  int connections = 1;            // connections started with (1 after a fallback to one connection)
  int splits = 0;                 // segments split for an idle connection
  int restarts = 0;               // slow connections replaced
  std::uint64_t resumed_from = 0; // bytes already on disk from an earlier run
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
