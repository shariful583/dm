// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "content.hpp"
#include "core/engine/download.hpp"
#include "core/resume/sidecar.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using namespace lanes::core::engine;
using lanes::testserver::content_sha256;
using lanes::testserver::sha256_hex;
using lanes::testserver::TestServer;

namespace {

constexpr std::uint64_t kSize = 9 << 20;
constexpr std::uint64_t kSeed = 60;

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Fixture {
  TestServer server;
  TempDir dir;
  Fixture() {
    server.add_file("/files/data.bin", {.size = kSize, .seed = kSeed});
    server.start();
  }
  DownloadOptions options(const std::string &path) const {
    DownloadOptions o;
    o.request.url = server.url(path);
    o.directory = dir.path();
    o.filename = "data.bin";
    o.retry_delay = 20ms; // keep tests fast
    return o;
  }
  // Probe once on a clean URL, so fault parameters only hit the range requests.
  net::ProbeResult probe() const {
    auto p = net::probe({.url = server.url("/files/data.bin")});
    REQUIRE(p);
    return *p;
  }
  bool intact(const DownloadResult &r) const {
    return sha256_hex(read_file(r.path)) == content_sha256(kSeed, kSize);
  }
};

using Clock = std::chrono::steady_clock;

} // namespace

TEST_CASE("dropped connections are retried and keep their bytes", "[engine][retry]") {
  Fixture f;
  auto o = f.options("/files/data.bin?drop_after=100000&drop_times=3");
  o.connections = 4;
  DownloadProgress last;
  o.on_progress = [&](const DownloadProgress &p) { last = p; };
  const auto result = download_file(o);
  REQUIRE(result);
  CHECK(result->retries >= 3);
  CHECK(f.intact(*result));
  CHECK(last.downloaded == kSize); // retried bytes are not counted twice
}

TEST_CASE("a transient error on the first request is retried", "[engine][retry]") {
  Fixture f;
  // HEAD and the GET fallback both get 503, then the server recovers.
  const auto result = download_file(f.options("/files/data.bin?status=503&status_times=2"));
  REQUIRE(result);
  CHECK(result->retries >= 1);
  CHECK(f.intact(*result));
}

TEST_CASE("Retry-After is honoured", "[engine][retry]") {
  Fixture f;
  auto o = f.options("/files/data.bin?status=503&status_times=1&retry_after=1");
  o.probe = f.probe();
  o.connections = 4;
  const auto start = Clock::now();
  const auto result = download_file(o);
  const auto elapsed = Clock::now() - start;
  REQUIRE(result);
  CHECK(result->retries == 1);
  CHECK(elapsed >= 900ms); // waited the server's 1 s, not our 20 ms
  CHECK(f.intact(*result));
}

TEST_CASE("a per-IP connection cap lowers the connection count", "[engine][retry]") {
  Fixture f;
  // The server answers 503 (Retry-After: 1) to a third simultaneous request.
  auto o = f.options("/files/data.bin?max_conns=2&rate=4194304");
  o.probe = f.probe();
  o.connections = 8;
  const auto result = download_file(o);
  REQUIRE(result);
  CHECK(result->connection_limit <= 2);
  CHECK(result->retries >= 1);
  CHECK(f.server.stats().max_concurrent_bodies <= 2);
  CHECK(f.intact(*result));
}

TEST_CASE("permanent errors are not retried", "[engine][retry]") {
  Fixture f;
  auto o = f.options("/files/missing.bin"); // ranges answer 404
  o.probe = f.probe();
  o.retry_delay = 1s; // any retry would show in the time taken
  const auto start = Clock::now();
  const auto result = download_file(o);
  REQUIRE_FALSE(result);
  CHECK(result.error().http_status == 404);
  CHECK(Clock::now() - start < 900ms);
}

TEST_CASE("a stalled connection times out and is retried", "[engine][retry]") {
  Fixture f;
  // The first range request trickles at 1 KiB/s; below 50 KB/s for 1 s counts as stalled.
  auto o = f.options("/files/data.bin?slow_requests=1&slow_rate=1024");
  o.probe = f.probe();
  o.connections = 2;
  o.request.low_speed_limit = 50'000;
  o.request.low_speed_time = 1s;
  o.replace_slow_connections = false; // isolate the timeout path
  o.split_when_idle = false;
  const auto start = Clock::now();
  const auto result = download_file(o);
  REQUIRE(result);
  CHECK(result->retries >= 1);
  CHECK(Clock::now() - start < 5s);
  CHECK(f.intact(*result));
}

TEST_CASE("a whole-file download restarts after a drop", "[engine][retry]") {
  Fixture f;
  DownloadProgress last;
  auto o = f.options("/files/data.bin?no_range=1&drop_after=100000&drop_times=1");
  // Probe separately: the probe's own range check would otherwise be the request that drops.
  auto probe = net::probe({.url = f.server.url("/files/data.bin?no_range=1")});
  REQUIRE(probe);
  REQUIRE_FALSE(probe->accepts_ranges);
  o.probe = *probe;
  o.on_progress = [&](const DownloadProgress &p) { last = p; };
  const auto result = download_file(o);
  REQUIRE(result);
  CHECK(result->connections == 1);
  CHECK(result->retries == 1);
  CHECK(last.downloaded == kSize);
  CHECK(f.intact(*result));
}

TEST_CASE("ranges waiting for a retry stay in the resume file", "[engine][retry][resume]") {
  Fixture f;
  auto o = f.options("/files/data.bin?status=503&status_times=0"); // every range fails
  o.probe = f.probe();
  o.connections = 4;
  o.retry_delay = 10s; // still waiting when we cancel
  std::stop_source stop;
  std::jthread canceller([&] {
    std::this_thread::sleep_for(300ms);
    stop.request_stop();
  });
  const auto result = download_file(o, stop.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == ErrorCode::Cancelled);
  // Still a valid map of the whole file, so a resume continues instead of starting over.
  const auto saved = resume::load(resume::sidecar_path(f.dir.path() / "data.bin"));
  REQUIRE(saved);
  REQUIRE(*saved);
  CHECK((*saved)->size == kSize);

  const auto resumed = download_file(f.options("/files/data.bin"));
  REQUIRE(resumed);
  CHECK(f.intact(*resumed));
}
