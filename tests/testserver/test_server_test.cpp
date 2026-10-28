// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>

#include "content.hpp"
#include "hls_fixture.hpp"
#include "test_server.hpp"

using namespace lanes::testserver;
using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t kSize = 1000;
constexpr std::uint64_t kSeed = 11;

struct Fixture {
  TestServer server;
  httplib::Client client;

  Fixture() : client("127.0.0.1", start_and_get_port(server)) { client.set_read_timeout(10s); }

  static int start_and_get_port(TestServer &server) {
    server.add_file("/a.bin", {.size = kSize, .seed = kSeed});
    server.start();
    return server.port();
  }

  httplib::Client new_client() const {
    httplib::Client c("127.0.0.1", server.port());
    c.set_read_timeout(10s);
    return c;
  }
};

} // namespace

TEST_CASE("serves a whole generated file with validators", "[testserver]") {
  Fixture f;
  const auto res = f.client.Get("/a.bin");
  REQUIRE(res);
  CHECK(res->status == 200);
  CHECK(res->body == make_content(kSeed, 0, kSize));
  CHECK(sha256_hex(res->body) == content_sha256(kSeed, kSize));
  CHECK(res->get_header_value("Accept-Ranges") == "bytes");
  CHECK_FALSE(res->get_header_value("ETag").empty());
  CHECK(res->get_header_value("Last-Modified") == "Wed, 01 Jan 2025 00:00:00 GMT");
}

TEST_CASE("HEAD reports size without a body", "[testserver]") {
  Fixture f;
  const auto res = f.client.Head("/a.bin");
  REQUIRE(res);
  CHECK(res->status == 200);
  CHECK(res->get_header_value("Content-Length") == std::to_string(kSize));
  CHECK(res->body.empty());
  CHECK(f.server.stats().body_responses == 0);
}

TEST_CASE("Range requests return 206 with Content-Range", "[testserver]") {
  Fixture f;
  const auto res = f.client.Get("/a.bin", httplib::Headers{{"Range", "bytes=100-199"}});
  REQUIRE(res);
  CHECK(res->status == 206);
  CHECK(res->get_header_value("Content-Range") == "bytes 100-199/1000");
  CHECK(res->body == make_content(kSeed, 100, 100));

  const auto tail = f.client.Get("/a.bin", httplib::Headers{{"Range", "bytes=990-"}});
  REQUIRE(tail);
  CHECK(tail->status == 206);
  CHECK(tail->body == make_content(kSeed, 990, 10));

  const auto bad = f.client.Get("/a.bin", httplib::Headers{{"Range", "bytes=5000-6000"}});
  REQUIRE(bad);
  CHECK(bad->status == 416);
}

TEST_CASE("no_range ignores Range and sends 200", "[testserver]") {
  Fixture f;
  const auto res = f.client.Get("/a.bin?no_range=1", httplib::Headers{{"Range", "bytes=100-199"}});
  REQUIRE(res);
  CHECK(res->status == 200);
  CHECK(res->body.size() == kSize);
  CHECK_FALSE(res->has_header("Accept-Ranges"));
}

TEST_CASE("no_head rejects HEAD; GET bytes=0-0 still probes", "[testserver]") {
  Fixture f;
  const auto head = f.client.Head("/a.bin?no_head=1");
  REQUIRE(head);
  CHECK(head->status == 405);

  const auto probe = f.client.Get("/a.bin?no_head=1", httplib::Headers{{"Range", "bytes=0-0"}});
  REQUIRE(probe);
  CHECK(probe->status == 206);
  CHECK(probe->get_header_value("Content-Range") == "bytes 0-0/1000");
}

TEST_CASE("chunked sends no Content-Length", "[testserver]") {
  Fixture f;
  const auto res = f.client.Get("/a.bin?chunked=1");
  REQUIRE(res);
  CHECK(res->status == 200);
  CHECK(res->get_header_value("Transfer-Encoding") == "chunked");
  CHECK_FALSE(res->has_header("Content-Length"));
  CHECK(res->body == make_content(kSeed, 0, kSize));
}

TEST_CASE("rate throttles the body", "[testserver]") {
  TestServer server;
  server.add_file("/big.bin", {.size = 256 * 1024, .seed = 1});
  server.start();
  httplib::Client client("127.0.0.1", server.port());

  const auto start = std::chrono::steady_clock::now();
  const auto res = client.Get("/big.bin?rate=1048576"); // 256 KiB at 1 MiB/s ≈ 250 ms
  const auto elapsed = std::chrono::steady_clock::now() - start;
  REQUIRE(res);
  CHECK(res->body.size() == 256 * 1024);
  CHECK(elapsed >= 200ms);
  CHECK(elapsed < 2s);
}

TEST_CASE("slow_requests throttles only the first bodies", "[testserver]") {
  TestServer server;
  server.add_file("/big.bin", {.size = 128 * 1024, .seed = 1});
  server.start();
  httplib::Client client("127.0.0.1", server.port());
  const std::string path = "/big.bin?slow_requests=1&slow_rate=262144"; // 128 KiB ≈ 500 ms

  auto start = std::chrono::steady_clock::now();
  REQUIRE(client.Get(path));
  CHECK(std::chrono::steady_clock::now() - start >= 400ms);

  start = std::chrono::steady_clock::now();
  REQUIRE(client.Get(path));
  CHECK(std::chrono::steady_clock::now() - start < 300ms);
}

TEST_CASE("drop_after cuts the first body, later ones complete", "[testserver]") {
  Fixture f;
  const auto first = f.client.Get("/a.bin?drop_after=300");
  CHECK_FALSE(first); // connection closed before Content-Length bytes arrived

  const auto second = f.client.Get("/a.bin?drop_after=300");
  REQUIRE(second);
  CHECK(second->body == make_content(kSeed, 0, kSize));
}

TEST_CASE("drop_prob=1 always drops", "[testserver]") {
  Fixture f;
  CHECK_FALSE(f.client.Get("/a.bin?drop_prob=1"));
  CHECK_FALSE(f.client.Get("/a.bin?drop_prob=1"));
}

TEST_CASE("status injection for the first K requests", "[testserver]") {
  Fixture f;
  const std::string path = "/a.bin?status=429&status_times=2&retry_after=3";
  const auto r1 = f.client.Get(path);
  const auto r2 = f.client.Get(path);
  const auto r3 = f.client.Get(path);
  REQUIRE((r1 && r2 && r3));
  CHECK(r1->status == 429);
  CHECK(r1->get_header_value("Retry-After") == "3");
  CHECK(r2->status == 429);
  CHECK(r3->status == 200);
  CHECK(r3->body.size() == kSize);
}

TEST_CASE("max_conns answers 503 beyond the limit", "[testserver]") {
  TestServer server;
  server.add_file("/big.bin", {.size = 64 * 1024, .seed = 1});
  server.start();
  const std::string path = "/big.bin?max_conns=1&rate=65536"; // first body takes ~1 s

  std::thread slow([&] {
    httplib::Client c("127.0.0.1", server.port());
    const auto res = c.Get(path);
    CHECK((res && res->status == 200));
  });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (server.stats().body_responses == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }

  httplib::Client second("127.0.0.1", server.port());
  const auto res = second.Get(path);
  REQUIRE(res);
  CHECK(res->status == 503);
  slow.join();
  CHECK(server.stats().max_concurrent_bodies == 1);
}

TEST_CASE("POST-only file needs the right body", "[testserver]") {
  TestServer server;
  server.add_file("/post.bin", {.size = 500,
                                .seed = 3,
                                .content_disposition = "attachment; filename=\"export.csv\"",
                                .post_body = "token=abc"});
  server.start();
  httplib::Client client("127.0.0.1", server.port());

  const auto get = client.Get("/post.bin");
  REQUIRE(get);
  CHECK(get->status == 405);

  const auto wrong = client.Post("/post.bin", "token=nope", "application/x-www-form-urlencoded");
  REQUIRE(wrong);
  CHECK(wrong->status == 403);

  const auto ok = client.Post("/post.bin", "token=abc", "application/x-www-form-urlencoded");
  REQUIRE(ok);
  CHECK(ok->status == 200);
  CHECK(ok->body == make_content(3, 0, 500));
  CHECK(ok->get_header_value("Content-Disposition") == "attachment; filename=\"export.csv\"");
}

TEST_CASE("signed URLs expire and reject tampering", "[testserver]") {
  Fixture f;
  const auto path_of = [&](const std::string &url) { return url.substr(f.server.url("").size()); };

  const auto valid = f.client.Get(path_of(f.server.signed_url("/a.bin", 1h)));
  REQUIRE(valid);
  CHECK(valid->status == 200);
  CHECK(valid->body.size() == kSize);

  const auto expired = f.client.Get(path_of(f.server.signed_url("/a.bin", -1s)));
  REQUIRE(expired);
  CHECK(expired->status == 403);

  std::string tampered = path_of(f.server.signed_url("/a.bin", 1h));
  tampered.back() = tampered.back() == '0' ? '1' : '0';
  const auto bad = f.client.Get(tampered);
  REQUIRE(bad);
  CHECK(bad->status == 403);
}

TEST_CASE("redirect chains end at the target", "[testserver]") {
  Fixture f;
  const auto hop = f.client.Get("/redirect?to=a.bin&hops=3");
  REQUIRE(hop);
  CHECK(hop->status == 302);
  CHECK(hop->get_header_value("Location").starts_with("/redirect?to=a.bin&hops=2"));

  f.client.set_follow_location(true);
  const auto final = f.client.Get("/redirect?to=a.bin&hops=3");
  REQUIRE(final);
  CHECK(final->status == 200);
  CHECK(final->body == make_content(kSeed, 0, kSize));
}

TEST_CASE("change_content changes bytes and validators", "[testserver]") {
  Fixture f;
  const auto before = f.client.Head("/a.bin");
  f.server.change_content("/a.bin", kSeed + 1);
  const auto after = f.client.Get("/a.bin");
  REQUIRE((before && after));
  CHECK(before->get_header_value("ETag") != after->get_header_value("ETag"));
  CHECK(before->get_header_value("Last-Modified") != after->get_header_value("Last-Modified"));
  CHECK(after->body == make_content(kSeed + 1, 0, kSize));
}

TEST_CASE("stats count separate client connections", "[testserver]") {
  Fixture f;
  for (int i = 0; i < 3; ++i) {
    auto c = f.new_client();
    REQUIRE(c.Get("/a.bin"));
  }
  const Stats s = f.server.stats();
  CHECK(s.connections == 3);
  CHECK(s.body_responses == 3);
  CHECK(s.bytes_sent == 3 * kSize);
}

TEST_CASE("serves the HLS fixture over HTTP", "[testserver][hls]") {
  TestServer server;
  const HlsFixture fx = make_hls_fixture("/hls", {.segments = 2, .segment_size = 4096});
  server.add_hls_fixture("/hls", fx);
  server.start();
  httplib::Client client("127.0.0.1", server.port());

  const auto master = client.Get("/hls/master.m3u8");
  REQUIRE(master);
  CHECK(master->get_header_value("Content-Type") == "application/vnd.apple.mpegurl");
  CHECK(master->body == fx.files.at("master.m3u8").body);

  const auto key = client.Get("/hls/key.bin");
  const auto seg = client.Get("/hls/low/seg1.ts", httplib::Headers{{"Range", "bytes=16-31"}});
  REQUIRE((key && seg));
  CHECK(key->body == fx.key);
  CHECK(seg->status == 206);
  CHECK(seg->body == fx.files.at("low/seg1.ts").body.substr(16, 16));
}
