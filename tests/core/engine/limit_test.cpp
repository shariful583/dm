// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "content.hpp"
#include "core/engine/download.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using namespace lanes::core::engine;
using lanes::core::limiter::RateLimiter;
using lanes::testserver::content_sha256;
using lanes::testserver::sha256_hex;
using lanes::testserver::TestServer;

namespace {

constexpr std::uint64_t kMiB = 1 << 20;
using Clock = std::chrono::steady_clock;

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Fixture {
  TestServer server; // unthrottled: only the limiter sets the pace
  TempDir dir;
  Fixture() {
    server.add_file("/a.bin", {.size = 4 * kMiB, .seed = 70});
    server.add_file("/b.bin", {.size = 2 * kMiB, .seed = 71});
    server.start();
  }
  DownloadOptions options(const std::string &path) const {
    DownloadOptions o;
    o.request.url = server.url(path);
    o.directory = dir.path();
    o.min_segment_size = 256 << 10;
    return o;
  }
};

// Average rate in bytes/s of `bytes` over the time `run` takes.
template <typename F> double measure(std::uint64_t bytes, F &&run) {
  const auto start = Clock::now();
  run();
  return static_cast<double>(bytes) / std::chrono::duration<double>(Clock::now() - start).count();
}

void check_within_10_percent(double measured, double limit) {
  INFO("measured " << measured / kMiB << " MiB/s, limit " << limit / kMiB << " MiB/s");
  CHECK(measured >= limit * 0.9);
  CHECK(measured <= limit * 1.1);
}

} // namespace

TEST_CASE("a download stays within 10 % of its speed limit", "[engine][limit]") {
  Fixture f;
  for (const int connections : {4, 1}) {
    auto o = f.options("/a.bin");
    o.filename = "a" + std::to_string(connections) + ".bin";
    o.connections = connections;
    o.speed_limit = 2 * kMiB;
    const double rate = measure(4 * kMiB, [&] {
      const auto result = download_file(o);
      REQUIRE(result);
      CHECK(sha256_hex(read_file(result->path)) == content_sha256(70, 4 * kMiB));
    });
    check_within_10_percent(rate, 2.0 * kMiB);
  }
}

TEST_CASE("a shared limit caps several downloads together", "[engine][limit]") {
  Fixture f;
  auto global = std::make_shared<RateLimiter>(2 * kMiB);
  const double rate = measure(4 * kMiB, [&] {
    std::jthread other([&] {
      auto o = f.options("/b.bin");
      o.filename = "b1.bin";
      o.limiters = {global};
      CHECK(download_file(o));
    });
    auto o = f.options("/b.bin");
    o.filename = "b2.bin";
    o.limiters = {global};
    CHECK(download_file(o));
  }); // both finished: 2 × 2 MiB
  check_within_10_percent(rate, 2.0 * kMiB);
}

TEST_CASE("the tighter of the download and shared limits wins", "[engine][limit]") {
  Fixture f;
  auto o = f.options("/b.bin");
  o.speed_limit = 4 * kMiB;
  o.limiters = {std::make_shared<RateLimiter>(1 * kMiB)};
  const double rate = measure(2 * kMiB, [&] { REQUIRE(download_file(o)); });
  check_within_10_percent(rate, 1.0 * kMiB);
}

TEST_CASE("a limit can be lifted mid-download", "[engine][limit]") {
  Fixture f;
  auto limit = std::make_shared<RateLimiter>(256 << 10); // would take 16 s
  auto o = f.options("/a.bin");
  o.limiters = {limit};
  std::jthread lifter([&] {
    std::this_thread::sleep_for(500ms);
    limit->set_rate(0);
  });
  const auto start = Clock::now();
  const auto result = download_file(o);
  REQUIRE(result);
  CHECK(Clock::now() - start < 3s);
  CHECK(sha256_hex(read_file(result->path)) == content_sha256(70, 4 * kMiB));
}
