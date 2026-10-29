// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/net/request.hpp"

#include <memory>

#include <curl/curl.h>

#include "core/net/curl.hpp"
#include "core/version.hpp"

namespace lanes::core::net {

namespace {

struct UrlDeleter {
  void operator()(CURLU *u) const noexcept { curl_url_cleanup(u); }
};
using UrlHandle = std::unique_ptr<CURLU, UrlDeleter>;

struct CurlFree {
  void operator()(char *p) const noexcept { curl_free(p); }
};
using CurlString = std::unique_ptr<char, CurlFree>;

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) {
      return false;
    }
  }
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.remove_suffix(1);
  }
  return s;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

std::string percent_decode(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex_value(s[i + 1]);
      const int lo = hex_value(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

bool is_valid_utf8(std::string_view s) {
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = c < 0x80             ? 1
                      : (c & 0xE0) == 0xC0 ? 2
                      : (c & 0xF0) == 0xE0 ? 3
                      : (c & 0xF8) == 0xF0 ? 4
                                           : 0;
    if (len == 0 || i + len > s.size()) {
      return false;
    }
    for (std::size_t k = 1; k < len; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
        return false;
      }
    }
    i += len;
  }
  return true;
}

std::string latin1_to_utf8(std::string_view s) {
  std::string out;
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x80) {
      out += ch;
    } else {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// Splits a header value on ';' outside double quotes.
std::vector<std::string_view> split_params(std::string_view value) {
  std::vector<std::string_view> parts;
  bool quoted = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && quoted) {
      ++i;
    } else if (value[i] == '"') {
      quoted = !quoted;
    } else if (value[i] == ';' && !quoted) {
      parts.push_back(value.substr(start, i - start));
      start = i + 1;
    }
  }
  parts.push_back(value.substr(start));
  return parts;
}

std::string unquote(std::string_view v) {
  if (v.size() < 2 || v.front() != '"' || v.back() != '"') {
    return std::string(v);
  }
  v = v.substr(1, v.size() - 2);
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      ++i;
    }
    out += v[i];
  }
  return out;
}

// filename*=charset'lang'value
std::optional<std::string> decode_ext_value(std::string_view v) {
  v = trim(v);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') { // tolerated, though not RFC 5987
    v = v.substr(1, v.size() - 2);
  }
  const auto q1 = v.find('\'');
  const auto q2 = q1 == std::string_view::npos ? q1 : v.find('\'', q1 + 1);
  if (q2 == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view charset = v.substr(0, q1);
  const std::string decoded = percent_decode(v.substr(q2 + 1));
  if (iequals(charset, "UTF-8")) {
    return is_valid_utf8(decoded) ? std::optional(decoded) : std::nullopt;
  }
  if (iequals(charset, "ISO-8859-1")) {
    return latin1_to_utf8(decoded);
  }
  return std::nullopt;
}

} // namespace

std::string default_user_agent() { return "Lanes/" + std::string(kVersionString); }

Result<void> validate_url(std::string_view url) {
  ensure_curl_initialized();
  UrlHandle handle(curl_url());
  const std::string copy(url);
  if (!handle || curl_url_set(handle.get(), CURLUPART_URL, copy.c_str(), 0) != CURLUE_OK) {
    return make_error(ErrorCode::InvalidArgument, "not a valid URL");
  }
  char *raw_scheme = nullptr;
  if (curl_url_get(handle.get(), CURLUPART_SCHEME, &raw_scheme, 0) != CURLUE_OK) {
    return make_error(ErrorCode::InvalidArgument, "URL has no scheme");
  }
  const CurlString scheme(raw_scheme);
  const std::string_view s(scheme.get());
  if (!iequals(s, "http") && !iequals(s, "https") && !iequals(s, "ftp") && !iequals(s, "ftps")) {
    return make_error(ErrorCode::InvalidArgument,
                      "unsupported URL scheme '" + std::string(s) + "' (use http, https or ftp)");
  }
  return {};
}

std::string filename_from_url(std::string_view url) {
  ensure_curl_initialized();
  UrlHandle handle(curl_url());
  const std::string copy(url);
  if (!handle || curl_url_set(handle.get(), CURLUPART_URL, copy.c_str(), 0) != CURLUE_OK) {
    return {};
  }
  char *raw_path = nullptr;
  if (curl_url_get(handle.get(), CURLUPART_PATH, &raw_path, 0) != CURLUE_OK) {
    return {};
  }
  const CurlString path(raw_path);
  const std::string_view p(path.get());
  const auto slash = p.rfind('/');
  // Decode after splitting, so an encoded "%2F" stays inside the name.
  return percent_decode(slash == std::string_view::npos ? p : p.substr(slash + 1));
}

std::string host_of(std::string_view url) {
  ensure_curl_initialized();
  UrlHandle handle(curl_url());
  const std::string copy(url);
  if (!handle || curl_url_set(handle.get(), CURLUPART_URL, copy.c_str(), 0) != CURLUE_OK) {
    return {};
  }
  char *raw = nullptr;
  if (curl_url_get(handle.get(), CURLUPART_HOST, &raw, 0) != CURLUE_OK) {
    return {};
  }
  const CurlString host(raw);
  std::string out(host.get());
  for (char &c : out) {
    c = lower(c);
  }
  return out;
}

std::string strip_credentials(std::string_view url) {
  ensure_curl_initialized();
  UrlHandle handle(curl_url());
  const std::string copy(url);
  if (!handle || curl_url_set(handle.get(), CURLUPART_URL, copy.c_str(), 0) != CURLUE_OK) {
    return copy;
  }
  curl_url_set(handle.get(), CURLUPART_USER, nullptr, 0);
  curl_url_set(handle.get(), CURLUPART_PASSWORD, nullptr, 0);
  char *raw = nullptr;
  if (curl_url_get(handle.get(), CURLUPART_URL, &raw, 0) != CURLUE_OK) {
    return copy;
  }
  const CurlString clean(raw);
  return std::string(clean.get());
}

std::optional<std::string> filename_from_content_disposition(std::string_view header) {
  std::optional<std::string> plain;
  std::optional<std::string> extended;
  for (const std::string_view part : split_params(header)) {
    const auto eq = part.find('=');
    if (eq == std::string_view::npos) {
      continue; // disposition type ("attachment") or junk
    }
    const std::string_view name = trim(part.substr(0, eq));
    const std::string_view value = trim(part.substr(eq + 1));
    if (iequals(name, "filename*")) {
      if (auto decoded = decode_ext_value(value)) {
        extended = std::move(decoded);
      }
    } else if (iequals(name, "filename")) {
      std::string v = unquote(value);
      plain = is_valid_utf8(v) ? std::move(v) : latin1_to_utf8(v);
    }
  }
  if (extended && !extended->empty()) {
    return extended;
  }
  if (plain && !plain->empty()) {
    return plain;
  }
  return std::nullopt;
}

} // namespace lanes::core::net
