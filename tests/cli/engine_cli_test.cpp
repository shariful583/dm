// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end: lanes.exe add/list/run/config/remove against an engine it starts on demand.
#include <chrono>
#include <random>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "../core/platform/temp_dir.hpp"
#include "content.hpp"
#include "core/platform/ipc.hpp"
#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using lanes::testserver::TestServer;

namespace {

// An isolated engine: its own data folder and pipe, stopped when the test ends.
struct EngineSandbox {
  TempDir dir;
  std::string pipe;
  TestServer server;

  EngineSandbox() {
    std::random_device rd;
    pipe = "lanes-test-engine-" + std::to_string(rd()) + std::to_string(rd());
    server.add_file("/a.bin", {.size = 1 << 20, .seed = 100});
    server.add_file("/b.bin", {.size = 1 << 20, .seed = 101});
    server.start();
  }
  ~EngineSandbox() {
    (void)run({"engine", "--stop"});
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (platform::connect_pipe(pipe, 200ms) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(100ms);
    }
  }

  platform::CapturedOutput run(std::vector<std::string> args) const {
    platform::ProcessOptions o;
    o.executable = platform::path_from_utf8(LANES_CLI_PATH);
    o.args = std::move(args);
    o.env = {{"LANES_DATA_DIR", platform::path_to_utf8(dir.path() / "data")},
             {"LANES_PIPE", pipe},
             {"LANES_ENGINE_IDLE_SECONDS", "30"}};
    auto result = platform::run_capture(o, 60s);
    REQUIRE(result);
    return *result;
  }
  std::string downloads() const { return platform::path_to_utf8(dir.path() / "dl") + "\\"; }

  // Polls `lanes list` until `text` appears.
  bool wait_for_list(const std::string &text, std::chrono::seconds timeout = 20s) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (run({"list"}).out.find(text) != std::string::npos) {
        return true;
      }
      std::this_thread::sleep_for(200ms);
    }
    return false;
  }
};

} // namespace

TEST_CASE("lanes add downloads in the background engine", "[cli][e2e][engine]") {
  EngineSandbox s;
  const auto added = s.run({"add", s.server.url("/a.bin"), "-o", s.downloads()});
  REQUIRE(added.exit_code == 0);
  CHECK(added.out.find("Added #1") != std::string::npos);
  CHECK(platform::connect_pipe(s.pipe, 1s)); // the engine was started on demand
  REQUIRE(s.wait_for_list("completed"));
  CHECK(fs::exists(s.dir.path() / "dl" / "a.bin"));

  const auto list = s.run({"list"});
  CHECK(list.out.find("a.bin") != std::string::npos);
  CHECK(list.out.find("100.0%") != std::string::npos);

  REQUIRE(s.run({"remove", "1", "--delete-file"}).exit_code == 0);
  CHECK_FALSE(fs::exists(s.dir.path() / "dl" / "a.bin"));
  CHECK(s.run({"list"}).out.find("No downloads") != std::string::npos);
}

TEST_CASE("lanes run processes a queue until it is done", "[cli][e2e][engine]") {
  EngineSandbox s;
  REQUIRE(
      s.run({"add", s.server.url("/a.bin"), "-o", s.downloads(), "--queue", "main"}).exit_code ==
      0);
  REQUIRE(
      s.run({"add", s.server.url("/b.bin"), "-o", s.downloads(), "--queue", "main"}).exit_code ==
      0);
  std::this_thread::sleep_for(500ms);
  CHECK(s.run({"list", "--queue", "main"}).out.find("queued [main]") != std::string::npos);

  const auto run = s.run({"run", "--queue", "main", "--max", "1"});
  CHECK(run.exit_code == 0);
  CHECK(run.out.find("2 done, 0 failed") != std::string::npos);
  CHECK(fs::exists(s.dir.path() / "dl" / "a.bin"));
  CHECK(fs::exists(s.dir.path() / "dl" / "b.bin"));
}

TEST_CASE("lanes config, pause and errors", "[cli][e2e][engine]") {
  EngineSandbox s;
  REQUIRE(s.run({"config", "speed_limit", "2M"}).exit_code == 0);
  CHECK(s.run({"config", "speed_limit"}).out == "2097152");
  CHECK(s.run({"config"}).out.find("connections = 8") != std::string::npos);
  CHECK(s.run({"config", "connections", "99"}).exit_code == 2);
  CHECK(s.run({"config", "bogus"}).exit_code == 2);

  CHECK(s.run({"pause", "42"}).exit_code == 1); // no such download
  CHECK(s.run({"pause", "nope"}).exit_code == 2);
  CHECK(s.run({"add", "file:///C:/x"}).exit_code == 2);
  CHECK(s.run({"add", s.server.url("/a.bin"), "--at", "someday"}).exit_code == 2);

  const auto scheduled =
      s.run({"add", s.server.url("/a.bin"), "-o", s.downloads(), "--at", "2099-01-01 00:00"});
  REQUIRE(scheduled.exit_code == 0);
  CHECK(scheduled.out.find("starts at 2099-01-01 00:00") != std::string::npos);
  CHECK(s.run({"list"}).out.find("scheduled 2099-01-01 00:00") != std::string::npos);
  REQUIRE(s.run({"pause", "all"}).exit_code == 0);
  CHECK(s.run({"list"}).out.find("paused") != std::string::npos);
}
