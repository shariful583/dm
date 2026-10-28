// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "content.hpp"
#include "core/engine/download.hpp"
#include "core/platform/path.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace lanes::core;
using namespace lanes::core::engine;
using lanes::testserver::content_sha256;
using lanes::testserver::sha256_hex;
using lanes::testserver::TestServer;

namespace {

constexpr std::uint64_t kSize = 9 * 1024 * 1024 + 77; // crosses the 4 MiB write buffer twice

std::string env_var(const char *name) {
#ifdef _MSC_VER
  char *value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char *value = std::getenv(name);
  return value != nullptr ? value : "";
#endif
}

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
    server.add_file("/files/data.bin", {.size = kSize, .seed = 21});
    server.add_file(
        "/files/named",
        {.size = 5000, .seed = 22, .content_disposition = "attachment; filename=\"report.pdf\""});
    server.add_file("/files/empty.txt", {.size = 0});
    server.add_file("/files/export", {.size = 70000,
                                      .seed = 23,
                                      .content_disposition = "attachment; filename=\"export.csv\"",
                                      .post_body = "token=abc&format=csv"});
    server.start();
  }
  DownloadOptions options(const std::string &path) const {
    DownloadOptions o;
    o.request.url = server.url(path);
    o.directory = dir.path();
    return o;
  }
};

} // namespace

TEST_CASE("downloads a file intact and renames the .part", "[engine][download]") {
  Fixture f;
  const auto result = download_file(f.options("/files/data.bin"));
  REQUIRE(result);
  CHECK(result->path == f.dir.path() / "data.bin");
  CHECK(result->size == kSize);
  CHECK(sha256_hex(read_file(result->path)) == content_sha256(21, kSize));
  CHECK_FALSE(fs::exists(f.dir.path() / "data.bin.part"));
  CHECK(result->info.accepts_ranges);
}

TEST_CASE("uses the server's name, or the caller's sanitized name", "[engine][download]") {
  Fixture f;
  const auto named = download_file(f.options("/files/named"));
  REQUIRE(named);
  CHECK(named->path.filename() == "report.pdf");

  auto custom = f.options("/files/named");
  custom.filename = "my:report?.pdf";
  const auto renamed = download_file(custom);
  REQUIRE(renamed);
  CHECK(renamed->path.filename() == "my_report_.pdf");

  auto nested = f.options("/files/named");
  nested.directory = f.dir.path() / "new" / "folder";
  const auto deep = download_file(nested);
  REQUIRE(deep);
  CHECK(fs::exists(f.dir.path() / "new" / "folder" / "report.pdf"));
}

TEST_CASE("unknown size (chunked) and empty files", "[engine][download]") {
  Fixture f;
  const auto chunked = download_file(f.options("/files/data.bin?chunked=1"));
  REQUIRE(chunked);
  CHECK(chunked->size == kSize);
  CHECK(sha256_hex(read_file(chunked->path)) == content_sha256(21, kSize));

  const auto empty = download_file(f.options("/files/empty.txt"));
  REQUIRE(empty);
  CHECK(empty->size == 0);
  CHECK(fs::file_size(empty->path) == 0);
}

TEST_CASE("follows redirects and works when HEAD is refused", "[engine][download]") {
  Fixture f;
  const auto redirected = download_file(f.options("/redirect?to=files/data.bin&hops=2"));
  REQUIRE(redirected);
  CHECK(redirected->path.filename() == "data.bin");
  CHECK(redirected->size == kSize);

  const auto no_head = download_file(f.options("/files/data.bin?no_head=1"));
  REQUIRE(no_head);
  CHECK(sha256_hex(read_file(no_head->path)) == content_sha256(21, kSize));
}

TEST_CASE("POST downloads send the body and use the response's name", "[engine][download]") {
  Fixture f;
  auto options = f.options("/files/export");
  options.request.method = "POST";
  options.request.body = "token=abc&format=csv";
  const auto result = download_file(options);
  REQUIRE(result);
  CHECK(result->path.filename() == "export.csv");
  CHECK(result->size == 70000);
  CHECK(sha256_hex(read_file(result->path)) == content_sha256(23, 70000));

  options.request.body = "token=wrong";
  const auto refused = download_file(options);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().http_status == 403);
}

TEST_CASE("request options reach the server", "[engine][download]") {
  Fixture f;
  auto options = f.options("/echo");
  options.filename = "echo.txt";
  options.request.method = "POST";
  options.request.body = "a=1&b=two";
  options.request.headers = {{"X-Custom", "value 1"}, {"Accept-Language", "en"}};
  options.request.cookies = "session=abc; theme=dark";
  options.request.referer = "https://example.com/page";
  options.request.user_agent = "TestAgent/1.0";
  const auto result = download_file(options);
  REQUIRE(result);
  const std::string echo = read_file(result->path);
  CHECK(echo.find("METHOD POST\n") != std::string::npos);
  CHECK(echo.find("X-Custom: value 1\n") != std::string::npos);
  CHECK(echo.find("Cookie: session=abc; theme=dark\n") != std::string::npos);
  CHECK(echo.find("Referer: https://example.com/page\n") != std::string::npos);
  CHECK(echo.find("User-Agent: TestAgent/1.0\n") != std::string::npos);
  CHECK(echo.find("BODY a=1&b=two\n") != std::string::npos);
  CHECK(echo.find("Accept-Encoding") == std::string::npos); // raw bytes, no compression

  // POST, so the probe's HEAD can't differ from the download (the echo body names the method).
  auto plain = f.options("/echo");
  plain.filename = "echo2.txt";
  plain.request.method = "POST";
  const auto defaults = download_file(plain);
  REQUIRE(defaults);
  CHECK(read_file(defaults->path).find("User-Agent: Lanes/") != std::string::npos);
}

TEST_CASE("HTTP authentication answers the server's challenge", "[engine][download]") {
  Fixture f;
  const std::string path = "/files/data.bin?basic_auth=user:pa:ss";
  auto options = f.options(path);
  const auto denied = download_file(options);
  REQUIRE_FALSE(denied);
  CHECK(denied.error().http_status == 401);

  options.request.auth = net::Credentials{"user", "pa:ss"};
  const auto result = download_file(options);
  REQUIRE(result);
  CHECK(sha256_hex(read_file(result->path)) == content_sha256(21, kSize));

  options.request.auth = net::Credentials{"user", "wrong"};
  const auto wrong = download_file(options);
  REQUIRE_FALSE(wrong);
  CHECK(wrong.error().http_status == 401);
}

TEST_CASE("progress events are throttled, monotonic and complete", "[engine][download]") {
  Fixture f;
  auto options = f.options("/files/data.bin?rate=12582912"); // 12 MiB/s: ~0.75 s
  options.progress_interval = std::chrono::milliseconds(100);
  std::vector<DownloadProgress> events;
  options.on_progress = [&](const DownloadProgress &p) { events.push_back(p); };
  const auto result = download_file(options);
  REQUIRE(result);
  REQUIRE(events.size() >= 3);
  CHECK(events.size() < 30);
  for (std::size_t i = 1; i < events.size(); ++i) {
    CHECK(events[i].downloaded >= events[i - 1].downloaded);
  }
  CHECK(events.back().downloaded == kSize);
  CHECK(events.back().total == kSize);
  CHECK(events.back().bytes_per_second > 4'000'000);
  CHECK(events.back().bytes_per_second < 20'000'000);
}

TEST_CASE("cancel stops quickly and keeps the .part file", "[engine][download]") {
  Fixture f;
  auto options = f.options("/files/data.bin?rate=262144"); // would take ~12 s
  std::stop_source stop;
  options.on_progress = [&](const DownloadProgress &p) {
    if (p.downloaded > 100'000) {
      stop.request_stop();
    }
  };
  const auto start = std::chrono::steady_clock::now();
  const auto result = download_file(options, stop.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == ErrorCode::Cancelled);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
  CHECK(fs::exists(f.dir.path() / "data.bin.part"));
  CHECK_FALSE(fs::exists(f.dir.path() / "data.bin"));
}

TEST_CASE("download errors", "[engine][download]") {
  Fixture f;
  const auto missing = download_file(f.options("/files/missing.bin"));
  REQUIRE_FALSE(missing);
  CHECK(missing.error().http_status == 404);
  CHECK(fs::is_empty(f.dir.path()));

  // The connection drops mid-file (retries come in step 8).
  const auto dropped = download_file(f.options("/files/data.bin?drop_after=1000000&drop_times=5"));
  REQUIRE_FALSE(dropped);
  CHECK(dropped.error().code == ErrorCode::Network);
  CHECK_FALSE(fs::exists(f.dir.path() / "data.bin"));

  // Probe said one size, the file on the server changed before the download started.
  auto changed = f.options("/files/data.bin");
  auto probe = net::probe(changed.request);
  REQUIRE(probe);
  changed.probe = *probe;
  f.server.change_content("/files/data.bin", 99, kSize + 10);
  const auto stale = download_file(changed);
  REQUIRE_FALSE(stale);
  CHECK(stale.error().message.find("changed on the server") != std::string::npos);
}

TEST_CASE("single-connection throughput", "[.][benchmark]") {
  TestServer server;
  constexpr std::uint64_t kBig = 1024ull * 1024 * 1024;
  server.add_file("/big.bin", {.size = kBig, .seed = 1});
  server.start();
  TempDir dir;
  DownloadOptions options;
  options.request.url = server.url("/big.bin");
  if (const std::string external = env_var("LANES_BENCH_URL"); !external.empty()) {
    options.request.url = external; // e.g. a standalone lanes-testserver.exe
  }
  options.directory = dir.path();
  const auto start = std::chrono::steady_clock::now();
  const auto result = download_file(options);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REQUIRE(result);
  WARN("1 GiB over one connection: " << seconds << " s, "
                                     << static_cast<double>(kBig) / seconds / 1e6 << " MB/s");
}
