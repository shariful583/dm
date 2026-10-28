// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::net {

struct Credentials {
  std::string username;
  std::string password;
};

struct ProxyOptions {
  // "http://host:port", "socks5h://host:port", ...; empty = direct connection.
  std::string url;
  std::optional<Credentials> credentials;
};

// Everything needed to repeat the request that fetches a download. Captured from
// the browser or given on the command line. Cookies and credentials are secrets: never logged or
// persisted (§9). Plain value type; not synchronized.
struct RequestOptions {
  std::string url;
  std::string method = "GET"; // "GET" or "POST"
  std::optional<std::string> body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string cookies; // "name=value; name2=value2"
  std::string referer;
  std::string user_agent;          // empty = default_user_agent()
  std::optional<Credentials> auth; // HTTP Basic or Digest, whichever the server asks for
  ProxyOptions proxy;
  bool follow_redirects = true;
  int max_redirects = 10;
  std::chrono::seconds connect_timeout{30};
  // Abort a transfer that stays below `low_speed_limit` bytes/s for `low_speed_time`.
  std::uint32_t low_speed_limit = 1024;
  std::chrono::seconds low_speed_time{60};
};

// "Lanes/<version>". Thread-safe.
std::string default_user_agent();

// Accepts only http, https, ftp and ftps URLs (never file:// or others).
// Thread-safe.
Result<void> validate_url(std::string_view url);

// Percent-decoded last path segment of `url` ("" if none). Not sanitized. Thread-safe.
std::string filename_from_url(std::string_view url);

// File name from a Content-Disposition header (RFC 6266): filename* (UTF-8 or ISO-8859-1) wins
// over filename. Returns UTF-8, not sanitized; nullopt if there is none. Thread-safe.
std::optional<std::string> filename_from_content_disposition(std::string_view header);

// `url` without embedded "user:password@" (for anything stored on disk, §9). Returns the input
// unchanged if it cannot be parsed. Thread-safe.
std::string strip_credentials(std::string_view url);

} // namespace lanes::core::net
