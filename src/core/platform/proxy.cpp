// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/proxy.hpp"

#include <vector>

namespace lanes::core::platform {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

std::string lower(std::string_view s) {
  std::string out(s);
  for (char &c : out) {
    c = lower(c);
  }
  return out;
}

std::vector<std::string_view> split(std::string_view list) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && (list[i] == ';' || list[i] == ' ' || list[i] == '\t')) {
      ++i;
    }
    std::size_t end = i;
    while (end < list.size() && list[end] != ';' && list[end] != ' ' && list[end] != '\t') {
      ++end;
    }
    if (end > i) {
      out.push_back(list.substr(i, end - i));
    }
    i = end;
  }
  return out;
}

// '*' matches any run of characters (including none); both sides already lowercase.
bool wildcard(std::string_view pattern, std::string_view text) {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star = std::string_view::npos;
  std::size_t mark = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (p < pattern.size() && pattern[p] == text[t]) {
      ++p;
      ++t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

} // namespace

std::string proxy_from_list(std::string_view list, std::string_view scheme) {
  const std::string want = lower(scheme);
  std::string fallback; // an entry without "scheme=" applies to every scheme
  for (std::string_view entry : split(list)) {
    std::string kind;
    if (const auto eq = entry.find('='); eq != std::string_view::npos) {
      kind = lower(entry.substr(0, eq));
      entry.remove_prefix(eq + 1);
    }
    if (entry.empty()) {
      continue;
    }
    std::string url;
    if (entry.find("://") != std::string_view::npos) {
      url = std::string(entry);
    } else if (kind == "socks") {
      url = "socks4a://" + std::string(entry); // Internet Options "Socks" means SOCKS4
    } else {
      url = "http://" + std::string(entry);
    }
    if (kind.empty()) {
      if (fallback.empty()) {
        fallback = url;
      }
    } else if (kind == want) {
      return url;
    } else if (kind == "socks" && fallback.empty()) {
      fallback = url; // IE sends everything without its own entry through the SOCKS proxy
    }
  }
  return fallback;
}

bool proxy_bypassed(std::string_view bypass_list, std::string_view host) {
  const std::string h = lower(host);
  for (std::string_view entry : split(bypass_list)) {
    const std::string pattern = lower(entry);
    if (pattern == "<local>") {
      if (h.find('.') == std::string::npos && h.find(':') == std::string::npos) {
        return true;
      }
    } else if (wildcard(pattern, h)) {
      return true;
    }
  }
  return false;
}

bool is_loopback_host(std::string_view host) {
  const std::string h = lower(host);
  return h == "localhost" || h.ends_with(".localhost") || h.starts_with("127.") || h == "::1" ||
         h == "[::1]";
}

} // namespace lanes::core::platform
