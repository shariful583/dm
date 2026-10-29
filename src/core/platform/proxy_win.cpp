// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/proxy.hpp"

#include <chrono>
#include <map>
#include <mutex>

#include <windows.h>
#include <winhttp.h>

#include "core/platform/win/win32.hpp"

namespace lanes::core::platform {

namespace {

constexpr auto kCacheTime = std::chrono::minutes(5);

std::string take(wchar_t *&p) {
  std::string out = p != nullptr ? win::narrow(p) : std::string();
  if (p != nullptr) {
    ::GlobalFree(p);
  }
  p = nullptr;
  return out;
}

struct InternetHandle {
  HINTERNET h = nullptr;
  ~InternetHandle() {
    if (h != nullptr) {
      ::WinHttpCloseHandle(h);
    }
  }
};

// Scheme and host (no user info, port or brackets) of an absolute URL; lowercase.
std::pair<std::string, std::string> scheme_and_host(std::string_view url) {
  const auto colon = url.find("://");
  if (colon == std::string_view::npos) {
    return {};
  }
  std::string scheme(url.substr(0, colon));
  std::string_view rest = url.substr(colon + 3);
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const auto at = rest.rfind('@'); at != std::string_view::npos) {
    rest.remove_prefix(at + 1);
  }
  std::string_view host = rest;
  if (!rest.empty() && rest.front() == '[') {
    host = rest.substr(1, rest.find(']') - 1);
  } else if (const auto port = rest.find(':'); port != std::string_view::npos) {
    host = rest.substr(0, port);
  }
  std::string h(host);
  for (char &c : scheme) {
    c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  for (char &c : h) {
    c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  return {scheme, h};
}

// PAC script or WPAD evaluation for `url`. "" = direct (or detection failed).
std::string auto_proxy(const std::wstring &url, const std::wstring &pac_url, bool detect) {
  InternetHandle session{::WinHttpOpen(L"Lanes", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (session.h == nullptr) {
    return {};
  }
  // Keep a missing or unreachable PAC server from stalling downloads for long.
  ::WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 5000);
  WINHTTP_AUTOPROXY_OPTIONS options{};
  options.fAutoLogonIfChallenged = TRUE;
  if (!pac_url.empty()) {
    options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
    options.lpszAutoConfigUrl = pac_url.c_str();
  } else if (detect) {
    options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
    options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
  } else {
    return {};
  }
  WINHTTP_PROXY_INFO info{};
  if (!::WinHttpGetProxyForUrl(session.h, url.c_str(), &options, &info)) {
    return {};
  }
  std::string list = take(info.lpszProxy);
  std::string bypass = take(info.lpszProxyBypass);
  if (info.dwAccessType != WINHTTP_ACCESS_TYPE_NAMED_PROXY) {
    return {};
  }
  (void)bypass; // a PAC script decides bypassing itself
  return list;
}

std::string evaluate(std::string_view url, const std::string &scheme, const std::string &host) {
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
  if (!::WinHttpGetIEProxyConfigForCurrentUser(&config)) {
    return {};
  }
  const bool detect = config.fAutoDetect != FALSE;
  const std::string pac = take(config.lpszAutoConfigUrl);
  const std::string manual = take(config.lpszProxy);
  const std::string bypass = take(config.lpszProxyBypass);

  // Windows tries the automatic settings first, then the manual proxy.
  if (detect || !pac.empty()) {
    const std::string list = auto_proxy(win::widen(url), win::widen(pac), detect);
    if (!list.empty()) {
      return proxy_from_list(list, scheme);
    }
  }
  if (!manual.empty() && !proxy_bypassed(bypass, host)) {
    return proxy_from_list(manual, scheme);
  }
  return {};
}

} // namespace

std::string system_proxy_for(std::string_view url) {
  const auto [scheme, host] = scheme_and_host(url);
  if (host.empty() || is_loopback_host(host)) {
    return {};
  }
  struct Entry {
    std::string proxy;
    std::chrono::steady_clock::time_point expires;
  };
  static std::mutex mutex;
  static std::map<std::string, Entry> cache;
  const std::string key = scheme + "://" + host;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(mutex);
    if (const auto it = cache.find(key); it != cache.end() && it->second.expires > now) {
      return it->second.proxy;
    }
  }
  std::string proxy = evaluate(url, scheme, host); // may take seconds (WPAD); not under the lock
  std::lock_guard lock(mutex);
  cache[key] = {proxy, now + kCacheTime};
  return proxy;
}

} // namespace lanes::core::platform
