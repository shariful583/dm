// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end: runs the real lanes.exe against the local test server.
#include <chrono>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "../core/platform/temp_dir.hpp"
#include "content.hpp"
#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using lanes::testserver::content_sha256;
using lanes::testserver::sha256_hex;
using lanes::testserver::TestServer;

namespace {

constexpr std::uint64_t kSize = 3 << 20;

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

platform::CapturedOutput run_lanes(std::vector<std::string> args) {
  platform::ProcessOptions o;
  o.executable = platform::path_from_utf8(LANES_CLI_PATH);
  o.args = std::move(args);
  auto result = platform::run_capture(o, 60s);
  REQUIRE(result);
  return *result;
}

struct Fixture {
  TestServer server;
  TempDir dir;
  Fixture() {
    server.add_file("/files/data.bin", {.size = kSize, .seed = 80});
    server.add_file("/files/named",
                    {.size = 1000,
                     .seed = 81,
                     .content_disposition = "attachment; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf"});
    server.start();
  }
  std::string url(const std::string &path) const { return server.url(path); }
  std::string out(const std::string &name = "") const {
    return platform::path_to_utf8(dir.path()) + "\\" + name;
  }
};

} // namespace

TEST_CASE("lanes get downloads into a folder or a named file", "[cli][e2e]") {
  Fixture f;
  const auto into_folder = run_lanes({"get", f.url("/files/data.bin"), "-o", f.out()});
  CHECK(into_folder.exit_code == 0);
  CHECK(into_folder.out.find("Saved ") != std::string::npos);
  CHECK(into_folder.out.find("connections") != std::string::npos);
  CHECK(sha256_hex(read_file(f.dir.path() / "data.bin")) == content_sha256(80, kSize));

  const auto named = run_lanes({"get", f.url("/files/data.bin"), "-o", f.out("copy.bin"), "-c", "2",
                                "--limit", "50M", "-q"});
  CHECK(named.exit_code == 0);
  CHECK(named.out.empty()); // --quiet
  CHECK(fs::exists(f.dir.path() / "copy.bin"));

  const auto unicode = run_lanes({"get", f.url("/files/named"), "-o", f.out()});
  CHECK(unicode.exit_code == 0);
  CHECK(fs::exists(f.dir.path() / platform::path_from_utf8("r\xC3\xA9sum\xC3\xA9.pdf")));
}

TEST_CASE("lanes get verifies checksums", "[cli][e2e]") {
  Fixture f;
  const std::string good = content_sha256(80, kSize);
  const auto ok =
      run_lanes({"get", f.url("/files/data.bin"), "-o", f.out("ok.bin"), "--sha256", good});
  CHECK(ok.exit_code == 0);
  CHECK(ok.out.find("SHA-256 OK") != std::string::npos);

  const auto bad = run_lanes(
      {"get", f.url("/files/data.bin"), "-o", f.out("bad.bin"), "--sha256", std::string(64, 'a')});
  CHECK(bad.exit_code == 1);
  CHECK(bad.err.find("SHA-256 mismatch") != std::string::npos);
  CHECK_FALSE(fs::exists(f.dir.path() / "bad.bin"));
  CHECK(fs::exists(f.dir.path() / "bad.bin.part"));
}

TEST_CASE("lanes get sends request options", "[cli][e2e]") {
  Fixture f;
  const auto r =
      run_lanes({"get", f.url("/echo"), "-o", f.out("echo.txt"), "-d", "x=1", "-H", "X-Test: yes",
                 "--cookie", "c=2", "--referer", "https://r.example/", "-A", "CliAgent/1"});
  REQUIRE(r.exit_code == 0);
  const std::string echo = read_file(f.dir.path() / "echo.txt");
  CHECK(echo.find("METHOD POST") != std::string::npos);
  CHECK(echo.find("X-Test: yes") != std::string::npos);
  CHECK(echo.find("Cookie: c=2") != std::string::npos);
  CHECK(echo.find("Referer: https://r.example/") != std::string::npos);
  CHECK(echo.find("User-Agent: CliAgent/1") != std::string::npos);
  CHECK(echo.find("BODY x=1") != std::string::npos);
}

TEST_CASE("lanes info describes the download", "[cli][e2e]") {
  Fixture f;
  const auto r = run_lanes({"info", f.url("/files/named")});
  CHECK(r.exit_code == 0);
  CHECK(r.out.find("r\xC3\xA9sum\xC3\xA9.pdf") != std::string::npos);
  CHECK(r.out.find("1000 bytes") != std::string::npos);
  CHECK(r.out.find("Resumable:     yes") != std::string::npos);

  const auto plain = run_lanes({"info", f.url("/files/data.bin?no_range=1")});
  CHECK(plain.out.find("Resumable:     no") != std::string::npos);
}

TEST_CASE("lanes reports errors with exit codes", "[cli][e2e]") {
  Fixture f;
  const auto missing = run_lanes({"get", f.url("/files/nope"), "-o", f.out()});
  CHECK(missing.exit_code == 1);
  CHECK(missing.err.find("404") != std::string::npos);

  CHECK(run_lanes({"get", f.url("/files/data.bin"), "--limit", "fast"}).exit_code == 2);
  CHECK(run_lanes({"get", f.url("/files/data.bin"), "-c", "99"}).exit_code == 2);
  CHECK(run_lanes({"get", "file:///C:/Windows/win.ini"}).exit_code == 2);
  CHECK(run_lanes({"get"}).exit_code == 2); // url is required
  CHECK(run_lanes({"--version"}).exit_code == 0);
}
