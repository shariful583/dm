// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/http.hpp"

#include <thread>

#include "core/net/curl.hpp"

namespace lanes::core::media {

namespace {

struct Sink {
  std::string body;
  std::size_t max = 0;
  bool too_large = false;
};

std::size_t collect(char *data, std::size_t size, std::size_t count, void *user) {
  auto &sink = *static_cast<Sink *>(user);
  const std::size_t n = size * count;
  if (sink.body.size() + n > sink.max) {
    sink.too_large = true;
    return CURL_WRITEFUNC_ERROR;
  }
  sink.body.append(data, n);
  return n;
}

bool transient(const Error &e) {
  return e.code == ErrorCode::Network || e.code == ErrorCode::Timeout || e.http_status == 429 ||
         e.http_status == 500 || e.http_status == 502 || e.http_status == 503 ||
         e.http_status == 504;
}

Result<Fetched> fetch_once(const net::RequestOptions &request, std::size_t max_bytes) {
  auto easy = net::make_easy();
  net::RequestStorage storage;
  if (auto ok = net::apply_request(easy.get(), request, storage); !ok) {
    return make_error(ok.error());
  }
  Sink sink;
  sink.max = max_bytes;
  curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, &collect);
  curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &sink);
  const CURLcode rc = curl_easy_perform(easy.get());
  if (sink.too_large) {
    return make_error(ErrorCode::FileTooLarge, "the playlist is unexpectedly large");
  }
  if (rc != CURLE_OK) {
    return make_error(net::transfer_error(rc, storage.error_buffer));
  }
  if (net::is_http(easy.get()) && net::response_code(easy.get()) >= 400) {
    return make_error(net::http_status_error(net::response_code(easy.get())));
  }
  Fetched f;
  f.body = std::move(sink.body);
  const char *url = nullptr;
  curl_easy_getinfo(easy.get(), CURLINFO_EFFECTIVE_URL, &url);
  f.final_url = url != nullptr ? url : request.url;
  const char *type = nullptr;
  curl_easy_getinfo(easy.get(), CURLINFO_CONTENT_TYPE, &type);
  f.content_type = type != nullptr ? type : "";
  return f;
}

} // namespace

Result<Fetched> fetch_small(const net::RequestOptions &request, std::size_t max_bytes) {
  constexpr int kAttempts = 4;
  for (int attempt = 1;; ++attempt) {
    auto r = fetch_once(request, max_bytes);
    if (r || attempt == kAttempts || !transient(r.error())) {
      return r;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
  }
}

} // namespace lanes::core::media
