// SPDX-License-Identifier: GPL-3.0-or-later
// Video sites through the fake yt-dlp (tests/helpers/fake_ytdlp.cpp) and the local test server;
// media generated with the bundled ffmpeg (skipped without it).
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "content.hpp"
#include "core/engine/engine.hpp"
#include "core/extract/updater.hpp"
#include "core/extract/video.hpp"
#include "core/extract/ytdlp.hpp"
#include "core/media/ffmpeg.hpp"
#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using lanes::testserver::TestServer;

namespace {

std::string read_file(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

struct ScopedEnv {
  std::wstring name;
  ScopedEnv(const wchar_t *n, const fs::path &value) : name(n) {
    ::SetEnvironmentVariableW(n, value.wstring().c_str());
  }
  ScopedEnv(const wchar_t *n, const std::string &value) : name(n) {
    ::SetEnvironmentVariableW(n, platform::path_from_utf8(value).wstring().c_str());
  }
  ~ScopedEnv() { ::SetEnvironmentVariableW(name.c_str(), nullptr); }
};

bool run_ffmpeg(const fs::path &ffmpeg, const fs::path &dir, std::vector<std::string> args) {
  std::vector<std::string> all = {"-hide_banner", "-loglevel", "error", "-y"};
  all.insert(all.end(), args.begin(), args.end());
  platform::ProcessOptions o{ffmpeg, all};
  o.working_directory = dir;
  auto r = platform::run_capture(o, 60s);
  return r && r->exit_code == 0;
}

// What ffmpeg says about a file ("Video: ...", "Audio: ...").
std::string probe(const fs::path &file) {
  auto ffmpeg = media::find_ffmpeg();
  platform::ProcessOptions o{*ffmpeg, {"-hide_banner", "-i", platform::path_to_utf8(file)}};
  auto r = platform::run_capture(o, 30s);
  return r ? r->err : std::string();
}

// The page's media, made once per test run (about 100 KiB in all).
struct Media {
  TempDir dir;
  bool ok = false;
  Media() {
    auto ffmpeg = media::find_ffmpeg();
    if (!ffmpeg) {
      return;
    }
    const auto p = [this](const char *f) { return platform::path_to_utf8(dir.path() / f); };
    const std::vector<std::string> src = {
        "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=1",
        "-f", "lavfi", "-i", "sine=frequency=440:duration=1"};
    auto with = [&](std::vector<std::string> more) {
      std::vector<std::string> a = src;
      a.insert(a.end(), more.begin(), more.end());
      return a;
    };
    fs::create_directories(dir.path() / "hls");
    ok = run_ffmpeg(*ffmpeg, dir.path(),
                    with({"-map", "0:v", "-c:v", "mpeg4", "-q:v", "8", p("video.mp4")})) &&
         run_ffmpeg(*ffmpeg, dir.path(),
                    with({"-map", "1:a", "-c:a", "aac", "-b:a", "48k", p("audio.m4a")})) &&
         run_ffmpeg(*ffmpeg, dir.path(),
                    with({"-c:v", "mpeg4", "-q:v", "8", "-c:a", "aac", "-b:a", "48k",
                          p("combined.mp4")})) &&
         run_ffmpeg(*ffmpeg, dir.path() / "hls",
                    with({"-c:v", "mpeg4", "-q:v", "8", "-bsf:v", "dump_extra", "-c:a", "aac", "-f",
                          "hls", "-hls_time", "1", "-hls_playlist_type", "vod", "index.m3u8"}));
    std::ofstream(dir.path() / "sub.vtt") << "WEBVTT\n\n00:00.000 --> 00:01.000\nHello\n";
    std::ofstream(dir.path() / "thumb.jpg", std::ios::binary) << std::string(1000, '\xff');
  }
};

Media &media_files() {
  static Media m;
  return m;
}

struct Fixture {
  TestServer server;
  TempDir dir;
  ScopedEnv ytdlp{L"LANES_YTDLP", fs::path(LANES_FAKE_YTDLP_PATH)};
  ScopedEnv json{L"LANES_FAKE_YTDLP_JSON", fs::path(LANES_FIXTURES_DIR) / "ytdlp" / "video.json"};
  ScopedEnv log{L"LANES_FAKE_YTDLP_LOG", dir.path() / "ytdlp-log.txt"};
  ScopedEnv source{L"LANES_FAKE_YTDLP_SOURCE", media_files().dir.path() / "combined.mp4"};
  ScopedEnv data{L"LANES_DATA_DIR", dir.path() / "data"};
  std::unique_ptr<ScopedEnv> base;

  Fixture() {
    server.add_directory("/yt", media_files().dir.path());
    server.start();
    base = std::make_unique<ScopedEnv>(L"LANES_FAKE_BASE", server.url(""));
  }
  engine::DownloadOptions options(const std::string &cookies = "") const {
    engine::DownloadOptions o;
    o.request.url = "https://www.youtube.com/watch?v=lanes0test1";
    o.request.cookies = cookies;
    o.directory = dir.path() / "out";
    o.connections = 4;
    o.min_segment_size = 16 << 10;
    return o;
  }
  std::string calls() const { return read_file(dir.path() / "ytdlp-log.txt"); }
};

#define REQUIRE_MEDIA()                                                                            \
  do {                                                                                             \
    if (!media_files().ok) {                                                                       \
      SKIP("ffmpeg.exe not found (tools/fetch-third-party.ps1)");                                  \
    }                                                                                              \
  } while (false)

} // namespace

TEST_CASE("yt-dlp reads the page with the browser's cookies", "[extract][video]") {
  Fixture f;
  auto info = extract::extract({.page = {.url = "https://www.youtube.com/watch?v=lanes0test1",
                                         .cookies = "SID=secret; HSID=x",
                                         .user_agent = "Mozilla/5.0 (test)"}});
  REQUIRE(info);
  CHECK(info->title == "Lanes Test Video: \"part 1\"");
  const std::string calls = f.calls();
  CHECK(calls.find("--ignore-config\n") != std::string::npos);
  CHECK(calls.find("-J\n--no-playlist\n--flat-playlist\n--\n"
                   "https://www.youtube.com/watch?v=lanes0test1\n") != std::string::npos);
  CHECK(calls.find("User-Agent:Mozilla/5.0 (test)\n") != std::string::npos);
  CHECK(calls.find("--cookies-file: # Netscape HTTP Cookie File") != std::string::npos);
  CHECK(calls.find("cookie-line: .youtube.com\tTRUE\t/\tFALSE\t2147483647\tSID\tsecret") !=
        std::string::npos);
  // The cookie file is gone afterwards.
  std::error_code ec;
  CHECK(fs::is_empty(f.dir.path() / "data" / "tmp", ec));
}

TEST_CASE("yt-dlp errors come back with its message", "[extract][video]") {
  Fixture f;
  ScopedEnv fail(L"LANES_FAKE_YTDLP_FAIL", std::string("[youtube] x: Sign in to confirm your age"));
  auto info = extract::extract({.page = {.url = "https://www.youtube.com/watch?v=x"}});
  REQUIRE_FALSE(info);
  CHECK(info.error().code == ErrorCode::AccessDenied);
  CHECK(info.error().message.find("Sign in to confirm your age") != std::string::npos);
}

TEST_CASE("the best quality is fetched by Lanes and joined into one MP4", "[extract][video]") {
  REQUIRE_MEDIA();
  Fixture f;
  extract::VideoRequest request{.format = "best", .subtitles = {"en", "xx"}, .thumbnail = true};
  int progress_calls = 0;
  auto options = f.options();
  options.on_progress = [&](const engine::DownloadProgress &) { ++progress_calls; };
  auto result = extract::download_video(options, request);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK(result->path.extension() == ".mp4");
  CHECK(platform::path_to_utf8(result->path.filename()).starts_with("Lanes Test Video"));
  const std::string streams = probe(result->path);
  CHECK(streams.find("Video: mpeg4") != std::string::npos);
  CHECK(streams.find("Audio: aac") != std::string::npos);
  CHECK(progress_calls > 0);
  // Subtitles (VTT preferred) and thumbnail next to it; the parts are gone.
  fs::path subtitle = result->path;
  subtitle.replace_extension(".en.vtt");
  CHECK(fs::exists(subtitle));
  fs::path thumbnail = result->path;
  thumbnail.replace_extension(".jpg");
  CHECK(fs::exists(thumbnail));
  int files = 0;
  for ([[maybe_unused]] const auto &entry : fs::directory_iterator(f.dir.path() / "out")) {
    ++files;
  }
  CHECK(files == 3);
  // yt-dlp only read the page: the formats came through Lanes.
  CHECK(f.calls().find("\n-f\n") == std::string::npos);
}

TEST_CASE("audio, MP3 and HLS formats", "[extract][video]") {
  REQUIRE_MEDIA();
  Fixture f;
  auto audio = extract::download_video(f.options(), {.format = "audio"});
  REQUIRE(audio);
  CHECK(audio->path.extension() == ".m4a");
  CHECK(probe(audio->path).find("Audio: aac") != std::string::npos);

  auto mp3 = extract::download_video(f.options(), {.format = "mp3"});
  INFO((mp3 ? std::string() : mp3.error().message));
  REQUIRE(mp3);
  CHECK(mp3->path.extension() == ".mp3");
  CHECK(probe(mp3->path).find("Audio: mp3") != std::string::npos);

  auto hls = extract::download_video(f.options(), {.format = "95"});
  INFO((hls ? std::string() : hls.error().message));
  REQUIRE(hls);
  CHECK(hls->path.extension() == ".mp4");
  CHECK(probe(hls->path).find("Video: mpeg4") != std::string::npos);
}

TEST_CASE("a format the site refuses to Lanes goes through yt-dlp's own downloader",
          "[extract][video]") {
  REQUIRE_MEDIA();
  Fixture f;
  int progress_calls = 0;
  auto options = f.options("SID=secret");
  options.on_progress = [&](const engine::DownloadProgress &p) {
    progress_calls += p.downloaded > 0 ? 1 : 0;
  };
  auto result = extract::download_video(options, {.format = "248+140"}); // 248 answers 403
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK(result->path.extension() == ".mkv");
  const std::string calls = f.calls();
  CHECK(calls.find("-f\n248+140\n") != std::string::npos);
  CHECK(calls.find("--merge-output-format\nmkv\n") != std::string::npos);
  CHECK(calls.find("--cookies-file: # Netscape") != std::string::npos);
  CHECK(progress_calls >= 2); // yt-dlp's progress lines became Lanes progress
}

TEST_CASE("the queue engine downloads video pages into the Video folder", "[extract][video]") {
  REQUIRE_MEDIA();
  Fixture f;
  auto engine = Engine::open({.database = f.dir.path() / "lanes.db", .tick = 20ms});
  REQUIRE(engine);
  REQUIRE((*engine)->set_setting("download_dir", platform::path_to_utf8(f.dir.path() / "dl")));
  NewDownload n;
  n.request.url = "https://www.youtube.com/watch?v=lanes0test1";
  n.video = extract::to_json({.format = "360p"});
  auto id = (*engine)->add(n);
  REQUIRE(id);
  CHECK((*engine)->get(*id)->video);
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < deadline &&
         (*engine)->get(*id)->state != DownloadState::Completed &&
         (*engine)->get(*id)->state != DownloadState::Failed) {
    std::this_thread::sleep_for(20ms);
  }
  const auto info = (*engine)->get(*id);
  INFO(info->error);
  REQUIRE(info->state == DownloadState::Completed);
  CHECK(info->directory == f.dir.path() / "dl" / "Video");
  CHECK(fs::exists(info->directory / platform::path_from_utf8(info->filename)));
  CHECK((*engine)
            ->add(NewDownload{.request = {.url = "https://e.com/v"}, .video = "{bad"})
            .error()
            .code == ErrorCode::InvalidArgument);
  (*engine)->shutdown();
}

TEST_CASE("the yt-dlp updater checks, verifies, swaps and rolls back", "[extract][update]") {
  TestServer server;
  TempDir dir;
  ScopedEnv ytdlp(L"LANES_YTDLP", fs::path(LANES_FAKE_YTDLP_PATH)); // version 2000.01.01
  const std::string binary = read_file(LANES_FAKE_YTDLP_PATH);
  const std::string hash = lanes::testserver::sha256_hex(binary);
  server.add_static("/dl/yt-dlp.exe", binary, "application/octet-stream");
  server.add_static("/dl/SHA2-256SUMS",
                    std::string(64, '0') + "  yt-dlp_linux\n" + hash + "  yt-dlp.exe\n",
                    "text/plain");
  server.add_static("/dl/BAD-SUMS", std::string(64, 'a') + "  yt-dlp.exe\n", "text/plain");
  const auto release = [&](const std::string &tag, const std::string &sums) {
    return R"({"tag_name":")" + tag +
           R"(","assets":[{"name":"yt-dlp.exe","browser_download_url":")" +
           server.url("/dl/yt-dlp.exe") + R"("},{"name":"SHA2-256SUMS","browser_download_url":")" +
           server.url(sums) + R"("}]})";
  };
  server.start();
  server.add_static("/new", release("2099.01.01", "/dl/SHA2-256SUMS"), "application/json");
  server.add_static("/old", release("1999.01.01", "/dl/SHA2-256SUMS"), "application/json");
  server.add_static("/bad", release("2099.01.01", "/dl/BAD-SUMS"), "application/json");
  const fs::path bin = dir.path() / "bin";

  // Older than what runs: nothing to do.
  auto same =
      extract::update_ytdlp({.releases_url = server.url("/old"), .folder = bin, .force = true});
  REQUIRE(same);
  CHECK(same->checked);
  CHECK_FALSE(same->updated);
  CHECK_FALSE(fs::exists(bin / "yt-dlp.exe"));

  // A wrong checksum never replaces anything.
  auto bad =
      extract::update_ytdlp({.releases_url = server.url("/bad"), .folder = bin, .force = true});
  REQUIRE_FALSE(bad);
  CHECK(bad.error().code == ErrorCode::ChecksumMismatch);
  CHECK_FALSE(fs::exists(bin / "yt-dlp.exe"));

  auto updated =
      extract::update_ytdlp({.releases_url = server.url("/new"), .folder = bin, .force = true});
  INFO((updated ? std::string() : updated.error().message));
  REQUIRE(updated);
  CHECK(updated->updated);
  CHECK(updated->version == "2000.01.01"); // what the new binary says
  CHECK(read_file(bin / "yt-dlp.exe") == binary);

  // Checked a moment ago: the next startup does not ask again.
  auto again = extract::update_ytdlp({.releases_url = server.url("/new"), .folder = bin});
  REQUIRE(again);
  CHECK_FALSE(again->checked);

  // A second update keeps the first as .old; rollback brings it back, then the bundled one.
  REQUIRE(
      extract::update_ytdlp({.releases_url = server.url("/new"), .folder = bin, .force = true}));
  CHECK(fs::exists(bin / "yt-dlp.exe.old"));
  REQUIRE(extract::rollback_ytdlp(bin));
  CHECK_FALSE(fs::exists(bin / "yt-dlp.exe.old"));
  CHECK(fs::exists(bin / "yt-dlp.exe"));
  REQUIRE(extract::rollback_ytdlp(bin));
  CHECK_FALSE(fs::exists(bin / "yt-dlp.exe"));
  CHECK(extract::rollback_ytdlp(bin).error().code == ErrorCode::NotFound);
}
