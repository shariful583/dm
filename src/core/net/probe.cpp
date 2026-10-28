// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/net/probe.hpp"

#include <charconv>

#include "core/net/curl.hpp"
#include "core/platform/path.hpp"

namespace lanes::core::net {

namespace {

struct Attempt {
  CURLcode code = CURLE_OK;
  long status = 0;
  bool http = true;
  ProbeResult info;
  std::optional<std::string> content_range;
  std::optional<std::string> accept_ranges;
  Error transport_error; // valid when !transport_ok()
  bool transport_ok() const { return code == CURLE_OK; }
};

struct DiscardContext {
  bool got_body = false;
};

// The probe GET only needs headers: stop at the first body byte.
std::size_t stop_at_body(char *, std::size_t, std::size_t, void *user) {
  static_cast<DiscardContext *>(user)->got_body = true;
  return CURL_WRITEFUNC_ERROR;
}

Result<Attempt> run(const RequestOptions &request, bool head) {
  auto easy = make_easy();
  RequestStorage storage;
  if (auto ok = apply_request(easy.get(), request, storage); !ok) {
    return make_error(ok.error());
  }
  DiscardContext discard;
  if (head) {
    curl_easy_setopt(easy.get(), CURLOPT_NOBODY, 1L);
  } else {
    curl_easy_setopt(easy.get(), CURLOPT_RANGE, "0-0");
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, stop_at_body);
    curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &discard);
  }

  Attempt a;
  a.code = curl_easy_perform(easy.get());
  if (a.code == CURLE_WRITE_ERROR && discard.got_body) {
    a.code = CURLE_OK; // our own early stop
  }
  a.status = response_code(easy.get());
  a.http = is_http(easy.get());
  if (!a.transport_ok()) {
    a.transport_error = transfer_error(a.code, storage.error_buffer);
  }
  a.info = probe_result_from_response(easy.get());
  a.content_range = last_response_header(easy.get(), "Content-Range");
  a.accept_ranges = last_response_header(easy.get(), "Accept-Ranges");
  return a;
}

bool connection_failed(CURLcode code) {
  return code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_RESOLVE_PROXY ||
         code == CURLE_COULDNT_CONNECT || code == CURLE_OPERATION_TIMEDOUT ||
         code == CURLE_SSL_CONNECT_ERROR || code == CURLE_PEER_FAILED_VERIFICATION;
}

// Total size from "bytes 0-0/12345"; nullopt for "bytes 0-0/*" or garbage.
std::optional<std::uint64_t> total_from_content_range(const std::string &value) {
  const auto slash = value.rfind('/');
  if (slash == std::string::npos) {
    return std::nullopt;
  }
  std::uint64_t total = 0;
  const char *begin = value.data() + slash + 1;
  const char *end = value.data() + value.size();
  while (end > begin && (end[-1] == ' ' || end[-1] == '\r')) {
    --end;
  }
  const auto [ptr, ec] = std::from_chars(begin, end, total);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return total;
}

bool is_success(const Attempt &a) {
  return a.transport_ok() && (!a.http || (a.status >= 200 && a.status < 300));
}

// Applies a `Range: bytes=0-0` answer to `r`.
void apply_range_check(const Attempt &get, ProbeResult &r) {
  if (get.status == 206) {
    r.accepts_ranges = true;
    if (get.content_range) {
      if (const auto total = total_from_content_range(*get.content_range)) {
        r.size = total;
      }
    }
  } else {
    r.accepts_ranges = false; // 200: the server ignored the range
  }
}

} // namespace

std::string suggest_filename(const std::string &content_disposition, const std::string &url) {
  std::string name;
  if (!content_disposition.empty()) {
    name = filename_from_content_disposition(content_disposition).value_or("");
  }
  if (name.empty()) {
    name = filename_from_url(url);
  }
  return platform::sanitize_filename(name);
}

Result<ProbeResult> probe(const RequestOptions &request) {
  if (request.method != "GET") {
    return make_error(ErrorCode::InvalidArgument, "only GET downloads can be probed");
  }

  auto head = run(request, true);
  if (!head) {
    return make_error(head.error());
  }
  if (!head->transport_ok() && connection_failed(head->code)) {
    return make_error(head->transport_error);
  }

  if (is_success(*head)) {
    ProbeResult r = head->info;
    if (!head->http) { // FTP: REST works wherever SIZE does
      r.accepts_ranges = r.size.has_value();
      return r;
    }
    const std::string advertised = head->accept_ranges.value_or("");
    if (r.accepts_ranges && r.size) {
      return r;
    }
    if (advertised == "none" && r.size) {
      return r;
    }
    // Many servers handle Range without advertising it; one 1-byte request settles it, and
    // unlocks parallel connections.
    if (auto get = run(request, false); get && is_success(*get)) {
      apply_range_check(*get, r);
      if (!r.size && get->status == 200 && get->info.size) {
        r.size = get->info.size;
      }
    }
    return r;
  }

  // HEAD refused (405/501, or 403 on URLs signed for GET only) or broken: probe with GET.
  auto get = run(request, false);
  if (!get) {
    return make_error(get.error());
  }
  if (!get->transport_ok()) {
    return make_error(get->transport_error);
  }
  if (get->http && get->status >= 400) {
    return make_error(http_status_error(get->status));
  }
  ProbeResult r = get->info;
  if (get->http) {
    if (get->status == 206) {
      r.size.reset(); // Content-Length is the 1-byte range; the total is in Content-Range
    }
    apply_range_check(*get, r);
  } else {
    r.accepts_ranges = r.size.has_value();
  }
  return r;
}

} // namespace lanes::core::net
