// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/net/probe.hpp"
#include "test_server.hpp"

using namespace lanes::core;
using namespace lanes::core::net;
using lanes::testserver::TestServer;

namespace {

constexpr std::uint64_t kSize = 123457;

struct Server {
  TestServer server;
  Server() {
    server.add_file("/files/data.bin", {.size = kSize, .seed = 3});
    server.add_file("/files/named",
                    {.size = 1000,
                     .seed = 4,
                     .content_type = "application/pdf",
                     .content_disposition = "attachment; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf"});
    server.add_file("/files/post", {.size = 10, .seed = 5, .post_body = "x=1"});
    server.start();
  }
  RequestOptions request(const std::string &path) const { return {.url = server.url(path)}; }
};

} // namespace

TEST_CASE("probe reads size, validators and name", "[net][probe]") {
  Server s;
  const auto r = probe(s.request("/files/data.bin"));
  REQUIRE(r);
  CHECK(r->size == kSize);
  CHECK(r->accepts_ranges);
  CHECK_FALSE(r->etag.empty());
  CHECK(r->last_modified == "Wed, 01 Jan 2025 00:00:00 GMT");
  CHECK(r->final_url == s.server.url("/files/data.bin"));
  CHECK(r->filename == "data.bin");
  CHECK(s.server.stats().body_responses == 0); // HEAD only
}

TEST_CASE("probe takes the name from Content-Disposition", "[net][probe]") {
  Server s;
  const auto r = probe(s.request("/files/named"));
  REQUIRE(r);
  CHECK(r->filename == "r\xC3\xA9sum\xC3\xA9.pdf");
  CHECK(r->content_type == "application/pdf");
}

TEST_CASE("probe falls back to GET when HEAD is refused", "[net][probe]") {
  Server s;
  const auto r = probe(s.request("/files/data.bin?no_head=1"));
  REQUIRE(r);
  CHECK(r->size == kSize); // from Content-Range, not the 1-byte Content-Length
  CHECK(r->accepts_ranges);
  CHECK(r->filename == "data.bin");
}

TEST_CASE("probe detects range support the server does not advertise", "[net][probe]") {
  Server s;
  const auto r = probe(s.request("/files/data.bin?no_accept_ranges=1"));
  REQUIRE(r);
  CHECK(r->size == kSize);
  CHECK(r->accepts_ranges);
}

TEST_CASE("probe detects servers that ignore ranges", "[net][probe]") {
  Server s;
  constexpr std::uint64_t kBig = 64ull << 20; // far more than socket buffers can absorb
  s.server.add_file("/files/big.bin", {.size = kBig, .seed = 6});
  const auto r = probe(s.request("/files/big.bin?no_range=1"));
  REQUIRE(r);
  CHECK(r->size == kBig);
  CHECK_FALSE(r->accepts_ranges);
  // The range check stopped at the first body byte instead of downloading everything.
  CHECK(s.server.stats().bytes_sent < kBig / 2);
}

TEST_CASE("probe with HEAD refused and ranges ignored", "[net][probe]") {
  Server s;
  const auto r = probe(s.request("/files/data.bin?no_head=1&no_range=1"));
  REQUIRE(r);
  CHECK(r->size == kSize);
  CHECK_FALSE(r->accepts_ranges);
}

TEST_CASE("probe of a chunked response has unknown size", "[net][probe]") {
  Server s;
  const auto r = probe(s.request("/files/data.bin?chunked=1"));
  REQUIRE(r);
  CHECK_FALSE(r->size);
  CHECK_FALSE(r->accepts_ranges);
}

TEST_CASE("probe follows redirects", "[net][probe]") {
  Server s;
  const auto r = probe(s.request("/redirect?to=files/data.bin&hops=3"));
  REQUIRE(r);
  CHECK(r->final_url == s.server.url("/files/data.bin"));
  CHECK(r->size == kSize);
  CHECK(r->filename == "data.bin");
}

TEST_CASE("probe works on URLs signed for GET", "[net][probe]") {
  Server s;
  const auto r = probe({.url = s.server.signed_url("/files/data.bin", std::chrono::hours(1))});
  REQUIRE(r);
  CHECK(r->size == kSize);

  const auto expired =
      probe({.url = s.server.signed_url("/files/data.bin", std::chrono::seconds(-5))});
  REQUIRE_FALSE(expired);
  CHECK(expired.error().code == ErrorCode::Http);
  CHECK(expired.error().http_status == 403);
}

TEST_CASE("probe errors", "[net][probe]") {
  Server s;
  const auto missing = probe(s.request("/files/missing.bin"));
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == ErrorCode::Http);
  CHECK(missing.error().http_status == 404);
  CHECK(missing.error().message.find("404") != std::string::npos);

  const auto unavailable = probe(s.request("/files/data.bin?status=503&status_times=0"));
  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error().http_status == 503);

  const auto bad_scheme = probe({.url = "file:///C:/Windows/win.ini"});
  REQUIRE_FALSE(bad_scheme);
  CHECK(bad_scheme.error().code == ErrorCode::InvalidArgument);

  const int port = s.server.port();
  s.server.stop();
  const auto refused = probe({.url = "http://127.0.0.1:" + std::to_string(port) + "/x"});
  REQUIRE_FALSE(refused);
  CHECK(refused.error().code == ErrorCode::Network);

  RequestOptions post{.url = "http://127.0.0.1/", .method = "POST"};
  CHECK(probe(post).error().code == ErrorCode::InvalidArgument);
}
