// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "content.hpp"
#include "core/engine/engine.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using lanes::testserver::content_sha256;
using lanes::testserver::sha256_hex;
using lanes::testserver::TestServer;

namespace {

constexpr std::uint64_t kSize = 2 << 20;
using Clock = std::chrono::steady_clock;

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Fixture {
  TestServer server;
  TempDir dir;
  std::unique_ptr<Engine> engine;

  Fixture() {
    server.add_file("/a.bin", {.size = kSize, .seed = 90});
    server.add_file("/b.bin", {.size = kSize, .seed = 91});
    server.add_file("/c.bin", {.size = kSize, .seed = 92});
    server.start();
    open();
  }
  void open() {
    auto e = Engine::open({.database = dir.path() / "lanes.db", .tick = 20ms});
    REQUIRE(e);
    engine = std::move(*e);
  }
  NewDownload download(const std::string &path, std::string queue = "") const {
    NewDownload n;
    n.request.url = server.url(path);
    n.directory = dir.path() / "dl";
    n.queue = std::move(queue);
    return n;
  }
  // Waits until download `id` reaches `state`.
  bool wait_for(DownloadId id, DownloadState state, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      if (const auto info = engine->get(id); info && info->state == state) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
  std::string file_hash(const std::string &name) const {
    return sha256_hex(read_file(dir.path() / "dl" / name));
  }
};

} // namespace

TEST_CASE("an added download starts and completes", "[engine][queue]") {
  Fixture f;
  std::vector<EngineEvent::Kind> events;
  std::mutex events_mutex;
  f.engine->subscribe([&](const EngineEvent &e) {
    std::lock_guard lock(events_mutex);
    events.push_back(e.kind);
  });
  const auto id = f.engine->add(f.download("/a.bin"));
  REQUIRE(id);
  REQUIRE(f.wait_for(*id, DownloadState::Completed));
  const auto info = f.engine->get(*id);
  CHECK(info->filename == "a.bin");
  CHECK(info->size == kSize);
  CHECK(info->downloaded == kSize);
  CHECK(info->completed_at.has_value());
  CHECK(f.file_hash("a.bin") == content_sha256(90, kSize));
  std::lock_guard lock(events_mutex);
  CHECK(events.front() == EngineEvent::Kind::Added);
  CHECK(std::count(events.begin(), events.end(), EngineEvent::Kind::Changed) >= 2);
}

TEST_CASE("queued downloads wait for their queue and respect its limit", "[engine][queue]") {
  Fixture f;
  queue::QueueSettings q{.name = "batch", .max_active = 1};
  REQUIRE(f.engine->save_queue(q));
  std::vector<DownloadId> ids;
  for (const char *path : {"/a.bin?rate=4194304", "/b.bin?rate=4194304", "/c.bin?rate=4194304"}) {
    ids.push_back(*f.engine->add(f.download(path, "batch")));
  }
  std::this_thread::sleep_for(200ms);
  for (const auto id : ids) {
    CHECK(f.engine->get(id)->state == DownloadState::Queued); // queue not started
  }
  CHECK(f.engine->idle()); // nothing will start on its own

  REQUIRE(f.engine->start_queue("batch"));
  int most_running = 0;
  const auto deadline = Clock::now() + 15s;
  while (Clock::now() < deadline) {
    int running = 0;
    int done = 0;
    for (const auto &d : f.engine->list()) {
      running += d.state == DownloadState::Running ? 1 : 0;
      done += d.state == DownloadState::Completed ? 1 : 0;
    }
    most_running = std::max(most_running, running);
    if (done == 3) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  }
  CHECK(most_running == 1);
  for (const auto id : ids) {
    CHECK(f.engine->get(id)->state == DownloadState::Completed);
  }
  CHECK(f.engine->idle());
}

TEST_CASE("pause keeps progress and resume finishes", "[engine][queue]") {
  Fixture f;
  const auto id = *f.engine->add(f.download("/a.bin?rate=1048576")); // ~0.5 s per connection
  REQUIRE(f.wait_for(id, DownloadState::Running));
  std::this_thread::sleep_for(300ms);
  REQUIRE(f.engine->pause(id));
  CHECK(f.engine->get(id)->state == DownloadState::Paused);
  std::this_thread::sleep_for(300ms); // the worker winds down and checkpoints
  CHECK(f.engine->get(id)->state == DownloadState::Paused);

  REQUIRE(f.engine->resume(id));
  REQUIRE(f.wait_for(id, DownloadState::Completed));
  CHECK(f.file_hash("a.bin") == content_sha256(90, kSize));
}

TEST_CASE("an immediate resume after pause is not lost", "[engine][queue]") {
  Fixture f;
  const auto id = *f.engine->add(f.download("/a.bin?rate=1048576"));
  REQUIRE(f.wait_for(id, DownloadState::Running));
  REQUIRE(f.engine->pause(id));
  REQUIRE(f.engine->resume(id)); // while the worker is still stopping
  REQUIRE(f.wait_for(id, DownloadState::Completed));
}

TEST_CASE("a scheduled download starts at its time", "[engine][queue]") {
  Fixture f;
  auto n = f.download("/a.bin");
  const auto start_at = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() +
                        2;
  n.start_at = start_at;
  const auto id = *f.engine->add(n);
  CHECK(f.engine->get(id)->state == DownloadState::Scheduled);
  CHECK_FALSE(f.engine->idle());
  std::this_thread::sleep_for(500ms);
  CHECK(f.engine->get(id)->state == DownloadState::Scheduled);
  REQUIRE(f.wait_for(id, DownloadState::Completed, 6s));
}

TEST_CASE("remove can delete a running download's files", "[engine][queue]") {
  Fixture f;
  const auto id = *f.engine->add(f.download("/a.bin?rate=262144"));
  REQUIRE(f.wait_for(id, DownloadState::Running));
  const auto deadline = Clock::now() + 5s;
  while (!fs::exists(f.dir.path() / "dl" / "a.bin.part") && Clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  REQUIRE(f.engine->remove(id, true));
  CHECK_FALSE(f.engine->get(id));
  std::this_thread::sleep_for(500ms); // deleted once the worker closed the files
  CHECK_FALSE(fs::exists(f.dir.path() / "dl" / "a.bin.part"));
  CHECK_FALSE(fs::exists(f.dir.path() / "dl" / "a.bin.lanes.json"));
  CHECK(f.engine->remove(id, false).error().code == ErrorCode::NotFound);
}

TEST_CASE("downloads survive an engine restart", "[engine][queue]") {
  Fixture f;
  const auto id = *f.engine->add(f.download("/a.bin?rate=524288"));
  REQUIRE(f.wait_for(id, DownloadState::Running));
  std::this_thread::sleep_for(700ms);
  f.engine.reset(); // like the engine process exiting mid-download

  f.open();
  const auto info = f.engine->get(id);
  REQUIRE(info);
  CHECK(info->downloaded > 0);
  f.server.reset_stats();
  REQUIRE(f.wait_for(id, DownloadState::Completed, 15s));
  CHECK(f.file_hash("a.bin") == content_sha256(90, kSize));
  CHECK(f.server.stats().bytes_sent < kSize); // resumed, not restarted
}

TEST_CASE("secrets never reach the database", "[engine][queue]") {
  Fixture f;
  auto n = f.download("/a.bin?rate=262144");
  n.request.cookies = "session=COOKIE-SECRET-1";
  n.request.auth = net::Credentials{"user", "PASSWORD-SECRET-2"};
  n.request.headers = {{"Authorization", "Bearer TOKEN-SECRET-3"}, {"X-Visible", "ok"}};
  const auto id = *f.engine->add(n);
  REQUIRE(f.wait_for(id, DownloadState::Running));
  f.engine.reset();
  std::string db;
  for (const char *name : {"lanes.db", "lanes.db-wal"}) {
    if (fs::exists(f.dir.path() / name)) {
      db += read_file(f.dir.path() / name);
    }
  }
  CHECK(db.find("X-Visible") != std::string::npos);       // the record is there...
  CHECK(db.find("COOKIE-SECRET-1") == std::string::npos); // ...without the secrets
  CHECK(db.find("PASSWORD-SECRET-2") == std::string::npos);
  CHECK(db.find("TOKEN-SECRET-3") == std::string::npos);
}

TEST_CASE("settings are validated and the global speed limit applies", "[engine][queue]") {
  Fixture f;
  CHECK(f.engine->set_setting("connections", "0").error().code == ErrorCode::InvalidArgument);
  CHECK(f.engine->set_setting("nonsense", "1").error().code == ErrorCode::InvalidArgument);
  REQUIRE(f.engine->set_setting("speed_limit", std::to_string(1 << 20))); // 1 MiB/s
  const auto start = Clock::now();
  const auto id = *f.engine->add(f.download("/a.bin")); // 2 MiB
  REQUIRE(f.wait_for(id, DownloadState::Completed));
  CHECK(Clock::now() - start >= 1700ms);
  bool found = false;
  for (const auto &[key, value] : f.engine->settings()) {
    if (key == "speed_limit") {
      found = value == std::to_string(1 << 20);
    }
  }
  CHECK(found);
}

TEST_CASE("failures are recorded and can be retried", "[engine][queue]") {
  Fixture f;
  const auto id = *f.engine->add(f.download("/missing.bin"));
  REQUIRE(f.wait_for(id, DownloadState::Failed));
  CHECK(f.engine->get(id)->error.find("404") != std::string::npos);

  f.server.add_file("/missing.bin", {.size = 1000, .seed = 93}); // now it exists
  REQUIRE(f.engine->resume(id));
  REQUIRE(f.wait_for(id, DownloadState::Completed));
}

TEST_CASE("stopping a queue sends its downloads back to waiting", "[engine][queue]") {
  Fixture f;
  REQUIRE(f.engine->save_queue({.name = "q", .max_active = 2}));
  const auto id = *f.engine->add(f.download("/a.bin?rate=262144", "q"));
  REQUIRE(f.engine->start_queue("q"));
  REQUIRE(f.wait_for(id, DownloadState::Running));
  REQUIRE(f.engine->stop_queue("q"));
  REQUIRE(f.wait_for(id, DownloadState::Queued));
  std::this_thread::sleep_for(300ms);
  CHECK(f.engine->get(id)->state == DownloadState::Queued); // does not restart by itself
}

TEST_CASE("a download's own speed limit changes while it runs", "[engine][queue]") {
  Fixture f;
  const auto id = *f.engine->add(f.download("/a.bin?rate=1048576")); // 2 MiB at ≤ 1 MiB/s
  REQUIRE(f.wait_for(id, DownloadState::Running));
  const auto start = Clock::now();
  REQUIRE(f.engine->set_speed_limit(id, 256 << 10));
  CHECK(f.engine->get(id)->speed_limit == (256u << 10));
  std::this_thread::sleep_for(1s);
  const auto slowed = f.engine->get(id)->downloaded;
  CHECK(slowed < (1u << 20) + (512u << 10)); // ≈ 256 KiB/s now (plus what came before)
  REQUIRE(f.engine->set_speed_limit(id, 0)); // lifted: finishes at the server's pace
  REQUIRE(f.wait_for(id, DownloadState::Completed));
  CHECK(Clock::now() - start < 5s);
  CHECK(f.engine->set_speed_limit(999, 1).error().code == ErrorCode::NotFound);
}

TEST_CASE("the system proxy setting is validated", "[engine][queue]") {
  Fixture f;
  REQUIRE(f.engine->set_setting("system_proxy", "0"));
  CHECK(f.engine->set_setting("system_proxy", "2").error().code == ErrorCode::InvalidArgument);
  const auto id = *f.engine->add(f.download("/a.bin")); // direct either way: loopback
  REQUIRE(f.wait_for(id, DownloadState::Completed));
}

TEST_CASE("the engine keeps the computer awake while it downloads", "[engine][queue]") {
  Fixture f;
  CHECK_FALSE(f.engine->keeping_awake());
  const auto id = *f.engine->add(f.download("/a.bin?rate=1048576")); // about 2 s
  REQUIRE(f.wait_for(id, DownloadState::Running));
  const auto deadline = Clock::now() + 2s;
  while (!f.engine->keeping_awake() && Clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  CHECK(f.engine->keeping_awake());
  REQUIRE(f.engine->set_setting("prevent_sleep", "0"));
  std::this_thread::sleep_for(100ms);
  CHECK_FALSE(f.engine->keeping_awake()); // turned off while it runs
  REQUIRE(f.engine->set_setting("prevent_sleep", "1"));
  REQUIRE(f.wait_for(id, DownloadState::Completed));
  std::this_thread::sleep_for(100ms);
  CHECK_FALSE(f.engine->keeping_awake()); // nothing left to do
}
