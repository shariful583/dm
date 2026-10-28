// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/error.hpp"
#include "core/net/request.hpp"

namespace lanes::core::net {

// What the server told us about a download before fetching it.
struct ProbeResult {
  std::string final_url;             // after redirects
  std::optional<std::uint64_t> size; // nullopt = unknown (chunked, no Content-Length)
  bool accepts_ranges = false;       // proven by a 206, or advertised (HTTP) / implied (FTP)
  std::string etag;                  // as sent, including quotes / W/ prefix
  std::string last_modified;         // HTTP-date as sent
  std::string content_type;
  std::string content_disposition;
  std::string filename; // suggested name, sanitized: Content-Disposition > URL path > "download"
};

// Probes `request` (HEAD, then a `Range: bytes=0-0` GET when HEAD is refused or does not show
// range support). Only GET requests can be probed; POST downloads learn the same facts from their
// response. Blocking; call from a worker thread. Thread-safe.
//
// Errors: InvalidArgument (bad/unsupported URL), Http (status >= 400; message has the status),
// Network/Timeout (connection failures).
Result<ProbeResult> probe(const RequestOptions &request);

// Suggested sanitized file name from Content-Disposition, else the URL path, else "download".
std::string suggest_filename(const std::string &content_disposition, const std::string &url);

} // namespace lanes::core::net
