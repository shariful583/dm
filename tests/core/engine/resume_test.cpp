// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "content.hpp"
#include "core/engine/download.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "core/resume/sidecar.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using namespace lanes::core::engine;
using lanes::testserver::content_sha256;
using lanes::testserver::make_content;
using lanes::testserver::sha256_hex;
using lanes::testserver::TestServer;

namespace {

constexpr std::uint64_t kSize = 8 << 20;
constexpr std::uint64_t kSeed = 50;

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
  fs::path final_path() const { return dir.path() / "data.bin"; }
  fs::path part_path() const { return dir.path() / "data.bin.part"; }
  fs::path sidecar() const { return resume::sidecar_path(final_path()); }

  DownloadOptions options(const std::string &url) const {
    DownloadOptions o;
    o.request.url = url;
    o.directory = dir.path();
    o.connections = 4;
    o.checkpoint_interval = 200ms;
    return o;
  }

  // Starts a throttled download and cancels it once `at_least` bytes have arrived.
  void download_partially(std::uint64_t at_least, DownloadOptions o) const {
    std::stop_source stop;
    o.on_progress = [&](const DownloadProgress &p) {
      if (p.downloaded >= at_least) {
        stop.request_stop();
      }
    };
    o.progress_interval = 20ms;
    const auto result = download_file(o, stop.get_token());
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::Cancelled);
  }

  // Every byte the sidecar says is done must really be in the .part file (checkpoint ordering).
  std::uint64_t check_sidecar_matches_disk(std::uint64_t seed = kSeed) const {
    const auto saved = resume::load(sidecar());
    REQUIRE(saved);
    REQUIRE(*saved);
    const std::string part = read_file(part_path());
    REQUIRE(part.size() == kSize);
    for (const auto &s : (*saved)->segments) {
      CHECK(part.substr(s.start, s.done) == make_content(seed, s.start, s.done));
    }
    return (*saved)->done_bytes();
  }
};

} // namespace

TEST_CASE("cancel, then resume only the missing bytes", "[engine][resume]") {
  Fixture f;
  f.download_partially(2 << 20, f.options(f.server.url("/files/data.bin?rate=2097152")));
  REQUIRE(fs::exists(f.part_path()));
  const std::uint64_t saved = f.check_sidecar_matches_disk();
  CHECK(saved >= (2 << 20));
  CHECK(saved < kSize);

  f.server.reset_stats();
  std::vector<DownloadProgress> events;
  auto o = f.options(f.server.url("/files/data.bin"));
  o.on_progress = [&](const DownloadProgress &p) { events.push_back(p); };
  const auto result = download_file(o);
  REQUIRE(result);
  CHECK(result->resumed_from == saved);
  CHECK(sha256_hex(read_file(result->path)) == content_sha256(kSeed, kSize));
  CHECK(f.server.stats().bytes_sent <= kSize - saved + (1 << 20)); // not downloaded again
  REQUIRE_FALSE(events.empty());
  CHECK(events.front().downloaded >= saved); // progress starts where it left off
  CHECK_FALSE(fs::exists(f.part_path()));
  CHECK_FALSE(fs::exists(f.sidecar()));
}

TEST_CASE("killing the process mid-download loses nothing", "[engine][resume]") {
  Fixture f;
  platform::ProcessOptions child;
  child.executable = platform::path_from_utf8(LANES_TEST_DOWNLOADER_PATH);
  child.args = {f.server.url("/files/data.bin?rate=1048576"), platform::path_to_utf8(f.dir.path()),
                "4"};
  auto process = platform::Process::start(child);
  REQUIRE(process);

  // Wait until the child has checkpointed a quarter of the file, then kill it hard.
  std::uint64_t seen = 0;
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (seen < kSize / 4 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
    if (auto saved = resume::load(f.sidecar()); saved && *saved) {
      seen = (*saved)->done_bytes();
    }
  }
  process->kill();
  REQUIRE(process->wait(5s).has_value());
  REQUIRE(seen >= kSize / 4);
  REQUIRE_FALSE(fs::exists(f.final_path()));

  const std::uint64_t saved = f.check_sidecar_matches_disk();
  f.server.reset_stats();
  const auto result = download_file(f.options(f.server.url("/files/data.bin")));
  REQUIRE(result);
  CHECK(result->resumed_from == saved);
  CHECK(sha256_hex(read_file(result->path)) == content_sha256(kSeed, kSize));
  CHECK(f.server.stats().bytes_sent <= kSize - saved + (1 << 20));
}

TEST_CASE("a file changed on the server is not resumed", "[engine][resume]") {
  Fixture f;
  f.download_partially(1 << 20, f.options(f.server.url("/files/data.bin?rate=2097152")));
  f.server.change_content("/files/data.bin", kSeed + 1); // same size, new ETag

  const auto refused = download_file(f.options(f.server.url("/files/data.bin")));
  REQUIRE_FALSE(refused);
  CHECK(refused.error().code == ErrorCode::RemoteChanged);
  CHECK(fs::exists(f.part_path())); // nothing thrown away without being asked

  auto restart = f.options(f.server.url("/files/data.bin"));
  restart.restart_if_changed = true;
  const auto result = download_file(restart);
  REQUIRE(result);
  CHECK(result->resumed_from == 0);
  CHECK(sha256_hex(read_file(result->path)) == content_sha256(kSeed + 1, kSize));
}

TEST_CASE("an expired link is reported and a new address continues", "[engine][resume]") {
  Fixture f;
  auto first = f.options(f.server.signed_url("/files/data.bin", 1h) + "&rate=2097152");
  first.filename = "data.bin";
  f.download_partially(2 << 20, first);
  const std::uint64_t saved = f.check_sidecar_matches_disk();

  auto expired = f.options(f.server.signed_url("/files/data.bin", -5s));
  expired.filename = "data.bin";
  const auto refused = download_file(expired);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().code == ErrorCode::LinkExpired);
  CHECK(refused.error().http_status == 403);
  CHECK(fs::exists(f.part_path()));

  auto refreshed = f.options(f.server.signed_url("/files/data.bin", 1h));
  refreshed.filename = "data.bin";
  const auto result = download_file(refreshed);
  REQUIRE(result);
  CHECK(result->resumed_from == saved);
  CHECK(sha256_hex(read_file(result->path)) == content_sha256(kSeed, kSize));
}

TEST_CASE("resume can be skipped, and a corrupt sidecar is ignored", "[engine][resume]") {
  Fixture f;
  f.download_partially(1 << 20, f.options(f.server.url("/files/data.bin?rate=2097152")));
  auto fresh = f.options(f.server.url("/files/data.bin"));
  fresh.resume = false;
  const auto restarted = download_file(fresh);
  REQUIRE(restarted);
  CHECK(restarted->resumed_from == 0);
  CHECK(sha256_hex(read_file(restarted->path)) == content_sha256(kSeed, kSize));

  f.download_partially(1 << 20, f.options(f.server.url("/files/data.bin?rate=2097152&run=2")));
  REQUIRE(platform::write_file_atomic(f.sidecar(), "{ not valid"));
  const auto recovered = download_file(f.options(f.server.url("/files/data.bin")));
  REQUIRE(recovered);
  CHECK(recovered->resumed_from == 0);
  CHECK(sha256_hex(read_file(recovered->path)) == content_sha256(kSeed, kSize));
}

TEST_CASE("downloads without range support keep no sidecar", "[engine][resume]") {
  Fixture f;
  auto o = f.options(f.server.url("/files/data.bin?no_range=1&rate=4194304"));
  f.download_partially(1 << 20, o);
  CHECK(fs::exists(f.part_path()));
  CHECK_FALSE(fs::exists(f.sidecar()));
}

TEST_CASE("the sidecar holds no secrets", "[engine][resume]") {
  Fixture f;
  const std::string url = "http://agent:s3cret@127.0.0.1:" + std::to_string(f.server.port()) +
                          "/files/data.bin?rate=2097152";
  auto o = f.options(url);
  o.request.cookies = "session=cookie-value-xyz";
  o.request.headers = {{"Authorization", "Bearer token-abc"}};
  o.request.auth = net::Credentials{"user", "pa55word"};
  f.download_partially(1 << 20, o);
  const std::string json = read_file(f.sidecar());
  CHECK(json.find("s3cret") == std::string::npos);
  CHECK(json.find("agent") == std::string::npos);
  CHECK(json.find("cookie-value-xyz") == std::string::npos);
  CHECK(json.find("token-abc") == std::string::npos);
  CHECK(json.find("pa55word") == std::string::npos);
  CHECK(json.find("data.bin") != std::string::npos);
}
