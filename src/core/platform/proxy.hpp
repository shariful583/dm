// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace lanes::core::platform {

// The proxy Windows would use for `url` (Settings → Network → Proxy): the manual proxy and its
// bypass list, a PAC script, or automatic detection (WPAD), as WinHTTP evaluates them. Returns a
// curl proxy URL ("http://host:port", "socks4a://host:port"), or "" for a direct connection
// (also for loopback hosts and when detection fails). Results are cached per scheme + host for a
// few minutes, so calling it for every connection is cheap. Thread-safe.
std::string system_proxy_for(std::string_view url);

// --- pieces of the above, exposed for tests (portable, no system calls) ---

// A WinHTTP/Internet Options proxy list → curl proxy URL for `scheme` ("http", "https", "ftp").
// Accepts "host:port", "http=h:p;https=h2:p2;socks=h3:p3", several entries separated by ';' or
// whitespace (the first that fits wins), and "scheme://host:port". "" if none applies.
std::string proxy_from_list(std::string_view list, std::string_view scheme);

// True if `host` matches the Internet Options bypass list ("<local>;*.corp.example;10.*", ';' or
// whitespace separated, '*' wildcards, case-insensitive). "<local>" matches names without a dot.
bool proxy_bypassed(std::string_view bypass_list, std::string_view host);

// localhost, 127.0.0.0/8 and ::1 never go through a proxy.
bool is_loopback_host(std::string_view host);

} // namespace lanes::core::platform
