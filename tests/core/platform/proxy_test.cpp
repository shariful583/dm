// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/platform/proxy.hpp"

using namespace lanes::core::platform;

TEST_CASE("Windows proxy lists become curl proxy URLs", "[proxy]") {
  CHECK(proxy_from_list("proxy.corp:8080", "https") == "http://proxy.corp:8080");
  CHECK(proxy_from_list("http=h1:80;https=h2:443;ftp=h3:21", "https") == "http://h2:443");
  CHECK(proxy_from_list("http=h1:80;https=h2:443", "ftp").empty());
  CHECK(proxy_from_list("socks=s:1080", "http") == "socks4a://s:1080");
  CHECK(proxy_from_list("http=h1:80;socks=s:1080", "https") == "socks4a://s:1080");
  CHECK(proxy_from_list("a:1; b:2", "http") == "http://a:1"); // PAC "PROXY a:1; PROXY b:2"
  CHECK(proxy_from_list("HTTPS=socks5h://s:9", "https") == "socks5h://s:9");
  CHECK(proxy_from_list("", "http").empty());
}

TEST_CASE("the proxy bypass list matches hosts", "[proxy]") {
  const char *bypass = "<local>;*.corp.example;10.*; Intranet.Example";
  CHECK(proxy_bypassed(bypass, "fileserver")); // <local>: no dot
  CHECK(proxy_bypassed(bypass, "dl.corp.example"));
  CHECK_FALSE(proxy_bypassed(bypass, "corp.example.com"));
  CHECK(proxy_bypassed(bypass, "10.1.2.3"));
  CHECK(proxy_bypassed(bypass, "intranet.example")); // case-insensitive
  CHECK_FALSE(proxy_bypassed(bypass, "example.com"));
  CHECK_FALSE(proxy_bypassed("", "anything"));
}

TEST_CASE("loopback addresses never use a proxy", "[proxy]") {
  CHECK(is_loopback_host("localhost"));
  CHECK(is_loopback_host("127.0.0.1"));
  CHECK(is_loopback_host("::1"));
  CHECK_FALSE(is_loopback_host("127example.com"));
#ifdef _WIN32
  CHECK(system_proxy_for("http://127.0.0.1:8080/a.bin").empty());
  CHECK(system_proxy_for("not a url").empty());
#endif
}
