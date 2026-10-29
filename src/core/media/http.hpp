// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

#include "core/error.hpp"
#include "core/net/request.hpp"

namespace lanes::core::media {

struct Fetched {
  std::string body;
  std::string final_url; // after redirects: the base for relative URIs
  std::string content_type;
};

// GETs a small resource (playlist, manifest, key) into memory, with the request's headers,
// cookies and credentials. FileTooLarge beyond `max_bytes`; Http for status >= 400.
// Retries transient failures a few times. Blocking. Thread-safe.
Result<Fetched> fetch_small(const net::RequestOptions &request, std::size_t max_bytes = 16u << 20);

} // namespace lanes::core::media
