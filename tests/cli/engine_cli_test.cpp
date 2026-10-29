// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end: lanes.exe add/list/run/config/remove against an engine it starts on demand.
#include <chrono>
#include <fstream>
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

  std::string credentials;

  EngineSandbox() {
    std::random_device rd;
    pipe = "lanes-test-engine-" + std::to_string(rd()) + std::to_string(rd());
    credentials = "LanesCliTest" + std::to_string(rd());
    server.add_file("/a.bin", {.size = 1 << 20, .seed = 100});
    server.add_file("/b.bin", {.size = 1 << 20, .seed = 101});
    for (int i = 1; i <= 3; ++i) {
      server.add_file("/p" + std::to_string(i) + ".bin", {.size = 1000, .seed = 102u + i});
    }
    server.start();
  }
  ~EngineSandbox() {
    for (const char *host : {"127.0.0.1"}) {
      (void)run({"login", "remove", host});
    }
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
             {"LANES_ENGINE_IDLE_SECONDS", "30"},
             {"LANES_CREDENTIAL_NAMESPACE", credentials}};
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

TEST_CASE("lanes add --batch and --pattern add many downloads", "[cli][e2e][engine]") {
  EngineSandbox s;
  const fs::path list = s.dir.path() / "urls.txt";
  {
    std::ofstream out(list);
    out << "# my downloads\n" << s.server.url("/a.bin") << "\n\n" << s.server.url("/b.bin") << "\n";
  }
  const auto batch = s.run({"add", "--batch", platform::path_to_utf8(list), "-o", s.downloads()});
  CHECK(batch.exit_code == 0);
  CHECK(batch.out.find("Added 2 of 2") != std::string::npos);

  const auto pattern =
      s.run({"add", "--pattern", s.server.url("/p[1-3].bin"), "-o", s.downloads()});
  CHECK(pattern.exit_code == 0);
  CHECK(pattern.out.find("Added 3 of 3") != std::string::npos);

  const auto again = s.run({"add", "--batch", platform::path_to_utf8(list), "-o", s.downloads()});
  CHECK(again.out.find("Added 0 of 2 download(s), 2 already in the list") != std::string::npos);

  // Five downloads share the default four slots: wait for every one of them.
  const std::vector<std::string> names = {"a.bin", "b.bin", "p1.bin", "p2.bin", "p3.bin"};
  const auto all_there = [&] {
    return std::all_of(names.begin(), names.end(),
                       [&](const std::string &n) { return fs::exists(s.dir.path() / "dl" / n); });
  };
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (std::chrono::steady_clock::now() < deadline && !all_there()) {
    std::this_thread::sleep_for(200ms);
  }
  for (const auto &name : names) {
    CHECK(fs::exists(s.dir.path() / "dl" / name));
  }
}

TEST_CASE("lanes add reports duplicates and honours --if-exists", "[cli][e2e][engine]") {
  EngineSandbox s;
  REQUIRE(s.run({"add", s.server.url("/a.bin"), "-o", s.downloads()}).exit_code == 0);
  REQUIRE(s.wait_for_list("completed"));
  const auto dup = s.run({"add", s.server.url("/a.bin"), "-o", s.downloads()});
  CHECK(dup.exit_code == 1);
  CHECK(dup.err.find("already in the list as #1") != std::string::npos);
  CHECK(dup.err.find("--if-exists") != std::string::npos);

  const auto renamed =
      s.run({"add", s.server.url("/a.bin"), "-o", s.downloads(), "--if-exists", "rename"});
  CHECK(renamed.exit_code == 0);
  REQUIRE(s.wait_for_list("a (2).bin"));
}

TEST_CASE("lanes categories and category folders", "[cli][e2e][engine]") {
  EngineSandbox s;
  CHECK(s.run({"categories"}).out.find("Video") != std::string::npos);
  REQUIRE(s.run({"category", "set", "Binaries", "--ext", "bin"}).exit_code == 0);
  CHECK(s.run({"categories"}).out.find("Binaries") != std::string::npos);
  REQUIRE(
      s.run({"config", "download_dir", platform::path_to_utf8(s.dir.path() / "dl")}).exit_code ==
      0);
  REQUIRE(s.run({"add", s.server.url("/a.bin")}).exit_code == 0); // no -o: category decides
  REQUIRE(s.wait_for_list("completed"));
  CHECK(fs::exists(s.dir.path() / "dl" / "Binaries" / "a.bin"));
  CHECK(s.run({"category", "remove", "Nope"}).exit_code == 1);
  REQUIRE(s.run({"category", "reset"}).exit_code == 0);
  CHECK(s.run({"categories"}).out.find("Binaries") == std::string::npos);
}

TEST_CASE("lanes login saves a site login for downloads", "[cli][e2e][engine]") {
  EngineSandbox s;
  const std::string url = s.server.url("/a.bin?basic_auth=alice:pa55");
  const std::string out = platform::path_to_utf8(s.dir.path() / "secret.bin");
  CHECK(s.run({"get", url, "-o", out}).exit_code == 1); // 401 without a login

  REQUIRE(s.run({"login", "add", s.server.url("/"), "alice", "--password", "pa55"}).exit_code == 0);
  const auto list = s.run({"login", "list"});
  CHECK(list.out.find("127.0.0.1  alice") != std::string::npos);
  CHECK(list.out.find("pa55") == std::string::npos);
  CHECK(s.run({"get", url, "-o", out}).exit_code == 0);

  REQUIRE(s.run({"login", "remove", "127.0.0.1"}).exit_code == 0);
  CHECK(s.run({"login", "list"}).out.find("No saved logins") != std::string::npos);
}
