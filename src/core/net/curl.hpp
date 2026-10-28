// SPDX-License-Identifier: GPL-3.0-or-later
// Internal libcurl helpers for core/net and core/engine. Not part of the public core API.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <curl/curl.h>

#include "core/error.hpp"
#include "core/net/probe.hpp"
#include "core/net/request.hpp"

namespace lanes::core::net {

// Runs curl_global_init once per process. Thread-safe.
void ensure_curl_initialized();

struct EasyDeleter {
  void operator()(CURL *h) const noexcept { curl_easy_cleanup(h); }
};
struct MultiDeleter {
  void operator()(CURLM *h) const noexcept { curl_multi_cleanup(h); }
};
struct SListDeleter {
  void operator()(curl_slist *l) const noexcept { curl_slist_free_all(l); }
};
using EasyHandle = std::unique_ptr<CURL, EasyDeleter>;
using MultiHandle = std::unique_ptr<CURLM, MultiDeleter>;
using SList = std::unique_ptr<curl_slist, SListDeleter>;

EasyHandle make_easy();
MultiHandle make_multi();

// State that must outlive a configured easy handle's transfer.
struct RequestStorage {
  SList headers;
  char error_buffer[CURL_ERROR_SIZE] = {};
};

// Applies `options` to `easy`: URL, method/body, headers, cookies, referer, user agent, auth,
// proxy, redirects (http/https/ftp/ftps only), timeouts, TLS settings. `storage` must outlive the
// transfer. Returns an error for an invalid URL or a curl option failure.
Result<void> apply_request(CURL *easy, const RequestOptions &options, RequestStorage &storage);

// Error for a failed transfer. The URL is not included (it may carry signed tokens, §9).
Error transfer_error(CURLcode code, const char *error_buffer);

// Value of header `name` in the last response of the transfer (after redirects), if present.
std::optional<std::string> last_response_header(CURL *easy, const char *name);

long response_code(CURL *easy);
bool is_http(CURL *easy);

// Error for an HTTP status >= 400 (code Http, http_status set).
Error http_status_error(long status);

// ProbeResult from the last response headers of `easy` (after headers arrived). accepts_ranges is
// set from Accept-Ranges: bytes; callers refine it.
ProbeResult probe_result_from_response(CURL *easy);

} // namespace lanes::core::net
