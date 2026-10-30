// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/net/curl.hpp"

#include <cstring>
#include <mutex>

#include "core/platform/proxy.hpp"

namespace lanes::core::net {

namespace {

constexpr long kReceiveBufferSize = 512 * 1024; // fewer, larger write callbacks

bool equals_ignore_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto lower = [](char c) {
      return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    if (lower(a[i]) != lower(b[i])) {
      return false;
    }
  }
  return true;
}

} // namespace

void ensure_curl_initialized() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

EasyHandle make_easy() {
  ensure_curl_initialized();
  return EasyHandle(curl_easy_init());
}

MultiHandle make_multi() {
  ensure_curl_initialized();
  return MultiHandle(curl_multi_init());
}

Result<void> apply_request(CURL *easy, const RequestOptions &options, RequestStorage &storage) {
  if (auto valid = validate_url(options.url); !valid) {
    return valid;
  }

  CURLcode rc = CURLE_OK;
  const auto set = [&](CURLoption option, auto value) {
    if (rc == CURLE_OK) {
      rc = curl_easy_setopt(easy, option, value);
    }
  };

  set(CURLOPT_ERRORBUFFER, storage.error_buffer);
  set(CURLOPT_URL, options.url.c_str());
  set(CURLOPT_PROTOCOLS_STR, "http,https,ftp,ftps");
  set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https,ftp,ftps");
  set(CURLOPT_FOLLOWLOCATION, options.follow_redirects ? 1L : 0L);
  set(CURLOPT_MAXREDIRS, static_cast<long>(options.max_redirects));
  set(CURLOPT_AUTOREFERER, 1L);
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.connect_timeout.count() * 1000));
  set(CURLOPT_LOW_SPEED_LIMIT, static_cast<long>(options.low_speed_limit));
  set(CURLOPT_LOW_SPEED_TIME, static_cast<long>(options.low_speed_time.count()));
  set(CURLOPT_BUFFERSIZE, kReceiveBufferSize);
  set(CURLOPT_TCP_KEEPALIVE, 1L);
  // Windows cert store via Schannel; offline/broken revocation servers must not fail downloads.
  set(CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));

  const std::string &agent = options.user_agent.empty() ? default_user_agent() : options.user_agent;
  set(CURLOPT_USERAGENT, agent.c_str());
  if (!options.referer.empty()) {
    set(CURLOPT_REFERER, options.referer.c_str());
  }
  if (!options.cookies.empty()) {
    set(CURLOPT_COOKIE, options.cookies.c_str());
  }

  if (equals_ignore_case(options.method, "POST")) {
    const std::string &body = options.body ? *options.body : std::string();
    set(CURLOPT_POST, 1L);
    set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    set(CURLOPT_COPYPOSTFIELDS, body.c_str());
  } else if (!equals_ignore_case(options.method, "GET")) {
    return make_error(ErrorCode::InvalidArgument, "unsupported HTTP method: " + options.method);
  }

  curl_slist *list = nullptr;
  for (const auto &[name, value] : options.headers) {
    // "Name:" with an empty value would make curl delete the header; "Name;" sends it empty.
    const std::string line = value.empty() ? name + ";" : name + ": " + value;
    curl_slist *appended = curl_slist_append(list, line.c_str());
    if (appended == nullptr) {
      curl_slist_free_all(list);
      return make_error(ErrorCode::Unknown, "out of memory building request headers");
    }
    list = appended;
  }
  storage.headers.reset(list);
  set(CURLOPT_HTTPHEADER, storage.headers.get());

  if (options.auth) {
    set(CURLOPT_USERNAME, options.auth->username.c_str());
    set(CURLOPT_PASSWORD, options.auth->password.c_str());
    set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC | CURLAUTH_DIGEST));
  }
  if (!options.proxy.url.empty()) {
    set(CURLOPT_PROXY, options.proxy.url.c_str());
    if (options.proxy.credentials) {
      set(CURLOPT_PROXYUSERNAME, options.proxy.credentials->username.c_str());
      set(CURLOPT_PROXYPASSWORD, options.proxy.credentials->password.c_str());
    }
  } else if (options.use_system_proxy) {
    // curl ignores the Windows proxy settings; "" also keeps it from using *_proxy variables.
    const std::string system = platform::system_proxy_for(options.url);
    set(CURLOPT_PROXY, system.c_str()); // copied by curl
  }

  if (rc != CURLE_OK) {
    return make_error(transfer_error(rc, storage.error_buffer));
  }
  return {};
}

Error transfer_error(CURLcode code, const char *error_buffer) {
  ErrorCode kind = ErrorCode::Network;
  switch (code) {
  case CURLE_OPERATION_TIMEDOUT:
    kind = ErrorCode::Timeout;
    break;
  case CURLE_UNSUPPORTED_PROTOCOL:
  case CURLE_URL_MALFORMAT:
    kind = ErrorCode::InvalidArgument;
    break;
  case CURLE_ABORTED_BY_CALLBACK:
    kind = ErrorCode::Cancelled;
    break;
  case CURLE_OUT_OF_MEMORY:
    kind = ErrorCode::Unknown;
    break;
  default:
    break;
  }
  std::string message = curl_easy_strerror(code);
  if (error_buffer != nullptr && error_buffer[0] != '\0') {
    message += ": ";
    message += error_buffer;
  }
  return Error{kind, std::move(message)};
}

std::optional<std::string> last_response_header(CURL *easy, const char *name) {
  curl_header *header = nullptr;
  if (curl_easy_header(easy, name, 0, CURLH_HEADER, -1, &header) != CURLHE_OK ||
      header == nullptr) {
    return std::nullopt;
  }
  return std::string(header->value);
}

long response_code(CURL *easy) {
  long code = 0;
  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
  return code;
}

Error http_status_error(long status) {
  std::string message = "server answered HTTP " + std::to_string(status);
  switch (status) {
  case 401:
    message += " (login required)";
    break;
  case 403:
    message += " (forbidden: the link may have expired)";
    break;
  case 404:
    message += " (not found)";
    break;
  case 410:
    message += " (gone: the link has expired)";
    break;
  case 429:
    message += " (too many requests)";
    break;
  case 503:
    message += " (service unavailable)";
    break;
  default:
    break;
  }
  return Error{ErrorCode::Http, std::move(message), static_cast<int>(status)};
}

ProbeResult probe_result_from_response(CURL *easy) {
  ProbeResult r;
  const char *effective = nullptr;
  curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &effective);
  r.final_url = effective != nullptr ? effective : "";

  curl_off_t length = -1;
  curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
  if (length >= 0) {
    r.size = static_cast<std::uint64_t>(length);
  }
  const char *type = nullptr;
  curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &type);
  r.content_type = type != nullptr ? type : "";

  r.etag = last_response_header(easy, "ETag").value_or("");
  r.last_modified = last_response_header(easy, "Last-Modified").value_or("");
  r.content_disposition = last_response_header(easy, "Content-Disposition").value_or("");
  const auto ranges = last_response_header(easy, "Accept-Ranges");
  r.accepts_ranges = ranges && equals_ignore_case(*ranges, "bytes");
  r.filename = suggest_filename(r.content_disposition, r.final_url, r.content_type);
  return r;
}

bool is_http(CURL *easy) {
  const char *scheme = nullptr;
  curl_easy_getinfo(easy, CURLINFO_SCHEME, &scheme);
  return scheme != nullptr &&
         (equals_ignore_case(scheme, "http") || equals_ignore_case(scheme, "https"));
}

} // namespace lanes::core::net
