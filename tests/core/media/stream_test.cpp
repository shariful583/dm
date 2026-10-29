// SPDX-License-Identifier: GPL-3.0-or-later
// HLS and DASH end to end: real (tiny) streams made by ffmpeg, served by the test server, fetched
// and joined into MP4.
#include <chrono>
#include <fstream>
#include <map>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "content.hpp"
#include "core/engine/engine.hpp"
#include "core/media/fetch.hpp"
#include "core/media/ffmpeg.hpp"
#include "core/media/hls.hpp"
#include "core/media/stream.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "hls_fixture.hpp"
#include "test_server.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace lanes::core;
using lanes::testserver::TestServer;

namespace {

// Streams generated on first use, shared by the tests of this process.
class Fixtures {
public:
  static Fixtures &get() {
    static Fixtures f;
    return f;
  }
  bool available() const { return ffmpeg_.has_value(); }
  const fs::path &root() const { return dir_.path(); }

  // Makes `name` (once): "hls_ts", "hls_aes", "hls_fmp4", "hls_360", "hls_video", "hls_audio",
  // "dash". Returns false if ffmpeg failed.
  bool make(const std::string &name) {
    if (made_.contains(name)) {
      return made_[name];
    }
    const fs::path out = dir_.path() / name;
    fs::create_directories(out);
    const bool big = name == "hls_360";
    std::vector<std::string> args = {"-hide_banner",
                                     "-loglevel",
                                     "error",
                                     "-y",
                                     "-f",
                                     "lavfi",
                                     "-i",
                                     big ? "testsrc2=size=640x360:rate=25:duration=3"
                                         : "testsrc2=size=320x180:rate=25:duration=3",
                                     "-f",
                                     "lavfi",
                                     "-i",
                                     "sine=frequency=440:duration=3",
                                     "-c:v",
                                     "mpeg4",
                                     "-q:v",
                                     "8",
                                     "-g",
                                     "25",
                                     "-c:a",
                                     "aac",
                                     "-b:a",
                                     "48k"};
    const auto p = [&](const char *file) { return platform::path_to_utf8(out / file); };
    if (name == "hls_video") {
      args.push_back("-an");
    } else if (name == "hls_audio") {
      args.push_back("-vn");
    }
    if (name != "hls_audio") {
      // MPEG-TS carries no global headers: repeat the video header in every keyframe, as real
      // HLS encoders do (otherwise the segments have no picture size).
      args.insert(args.end(), {"-bsf:v", "dump_extra"});
    }
    if (name == "dash") {
      args.insert(args.end(), {"-f", "dash", "-seg_duration", "1", "-use_template", "1",
                               "-use_timeline", "1", p("manifest.mpd")});
    } else {
      args.insert(args.end(), {"-f", "hls", "-hls_time", "1", "-hls_playlist_type", "vod"});
      if (name == "hls_fmp4") {
        args.insert(args.end(), {"-hls_segment_type", "fmp4", "-hls_fmp4_init_filename", "init.mp4",
                                 "-hls_segment_filename", p("seg%03d.m4s")});
      } else {
        args.insert(args.end(), {"-hls_segment_filename", p("seg%03d.ts")});
      }
      if (name == "hls_aes") {
        std::ofstream(out / "key.bin", std::ios::binary) << std::string(16, '\x2A');
        std::ofstream(out / "keyinfo.txt") << "key.bin\n"
                                           << p("key.bin") << "\n"
                                           << "0123456789abcdef0123456789abcdef\n";
        args.insert(args.end(), {"-hls_key_info_file", p("keyinfo.txt")});
      }
      args.push_back(p("index.m3u8"));
    }
    platform::ProcessOptions o{*ffmpeg_, args};
    o.working_directory = out; // fMP4 init and DASH fragments are written relative to it
    const auto r = platform::run_capture(o, 60s);
    made_[name] = r && r->exit_code == 0;
    return made_[name];
  }

  // What ffmpeg says about a media file (its stream list on stderr).
  std::string describe(const fs::path &file) const {
    platform::ProcessOptions o{*ffmpeg_, {"-hide_banner", "-i", platform::path_to_utf8(file)}};
    const auto r = platform::run_capture(o, 30s);
    return r ? r->err : std::string();
  }

private:
  Fixtures() {
    if (auto f = media::find_ffmpeg()) {
      ffmpeg_ = *f;
    }
  }
  TempDir dir_;
  std::optional<fs::path> ffmpeg_;
  std::map<std::string, bool> made_;
};

#define REQUIRE_FIXTURES(...)                                                                      \
  do {                                                                                             \
    if (!Fixtures::get().available()) {                                                            \
      SKIP("ffmpeg.exe not found (run tools/fetch-third-party.ps1)");                              \
    }                                                                                              \
    for (const char *name : {__VA_ARGS__}) {                                                       \
      REQUIRE(Fixtures::get().make(name));                                                         \
    }                                                                                              \
  } while (false)

struct Served {
  TestServer server;
  TempDir out;
  Served() {
    server.add_directory("/media", Fixtures::get().root());
    server.start();
  }
  engine::DownloadOptions options(const std::string &path) const {
    engine::DownloadOptions o;
    o.request.url = server.url(path);
    o.directory = out.path();
    o.connections = 4;
    return o;
  }
};

void check_av(const fs::path &file, const std::string &resolution = "320x180") {
  const std::string info = Fixtures::get().describe(file);
  INFO(info);
  CHECK(info.find("Video: mpeg4") != std::string::npos);
  CHECK(info.find(resolution) != std::string::npos);
  CHECK(info.find("Audio: aac") != std::string::npos);
  CHECK(info.find("Duration: 00:00:0") != std::string::npos);
}

} // namespace

TEST_CASE("segments are fetched in parallel and AES-128 decrypted", "[media][stream]") {
  // No ffmpeg needed: the test server's synthetic encrypted fixture.
  TestServer server;
  const auto fx =
      lanes::testserver::make_hls_fixture("/hls", {.segments = 5, .segment_size = 30000});
  server.add_hls_fixture("/hls", fx);
  server.start();
  TempDir dir;
  for (const char *variant : {"low", "high", "fmp4"}) { // explicit IV, sequence IV, init + clear
    const std::string playlist = fx.files.at(std::string(variant) + "/index.m3u8").body;
    const auto track =
        media::hls::parse_media(playlist, server.url("/hls/") + variant + "/index.m3u8");
    REQUIRE(track);
    const auto files = media::fetch_tracks({*track}, dir.path() / variant, {});
    REQUIRE(files);
    const auto &list = files->front();
    std::size_t i = 0;
    if (track->init) {
      CHECK(*platform::read_small_file(list[i++]) ==
            fx.plaintext.at(std::string(variant) + "/init.mp4"));
    }
    for (std::size_t s = 0; s < track->parts.size(); ++s, ++i) {
      const std::string ext = std::string(variant) == "fmp4" ? ".m4s" : ".ts";
      CHECK(*platform::read_small_file(list[i]) ==
            fx.plaintext.at(std::string(variant) + "/seg" + std::to_string(s) + ext));
    }
  }
}

TEST_CASE("an HLS stream becomes one MP4", "[media][stream]") {
  REQUIRE_FIXTURES("hls_ts", "hls_aes", "hls_fmp4");
  Served s;
  for (const char *name : {"hls_ts", "hls_aes", "hls_fmp4"}) {
    INFO(name);
    const auto result =
        media::download_any(s.options(std::string("/media/") + name + "/index.m3u8"));
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result);
    CHECK(result->path == s.out.path() / (std::string(name) + ".mp4")); // not "index.mp4"
    check_av(result->path);
    fs::path parts = result->path;
    parts += ".parts";
    CHECK_FALSE(fs::exists(parts)); // cleaned up after the join
  }
}

TEST_CASE("an HLS master picks the quality and joins separate audio", "[media][stream]") {
  REQUIRE_FIXTURES("hls_ts", "hls_360", "hls_video", "hls_audio");
  Served s;
  s.server.add_static("/media/master.m3u8",
                      "#EXTM3U\n"
                      "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=320x180\nhls_ts/index.m3u8\n"
                      "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=640x360\nhls_360/index.m3u8\n",
                      "application/vnd.apple.mpegurl");
  s.server.add_static("/media/master_audio.m3u8",
                      "#EXTM3U\n"
                      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"Main\",DEFAULT=YES,"
                      "URI=\"hls_audio/index.m3u8\"\n"
                      "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=320x180,AUDIO=\"a\"\n"
                      "hls_video/index.m3u8\n",
                      "application/vnd.apple.mpegurl");

  auto best = s.options("/media/master.m3u8");
  best.filename = "best.mp4";
  const auto big = media::download_any(best);
  REQUIRE(big);
  check_av(big->path, "640x360");

  auto small = s.options("/media/master.m3u8");
  small.filename = "small.mp4";
  small.max_video_height = 200;
  const auto little = media::download_any(small);
  REQUIRE(little);
  check_av(little->path, "320x180");

  auto separate = s.options("/media/master_audio.m3u8");
  separate.filename = "joined.mp4";
  const auto joined = media::download_any(separate);
  REQUIRE(joined);
  check_av(joined->path); // video from one playlist, audio from another

  const auto info = media::inspect_stream({.url = s.server.url("/media/master.m3u8")});
  REQUIRE(info);
  CHECK(info->kind == media::StreamKind::Hls);
  CHECK(info->choices.size() == 2);
  CHECK(info->choices[1].height == 360);
}

TEST_CASE("a DASH stream with separate audio becomes one MP4", "[media][stream]") {
  REQUIRE_FIXTURES("dash");
  Served s;
  const auto result = media::download_any(s.options("/media/dash/manifest.mpd"));
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  CHECK(result->path.filename() == "video.mp4"); // manifest, dash and media are all generic
  check_av(result->path);
}

TEST_CASE("a cancelled stream download resumes with the segments it has", "[media][stream]") {
  REQUIRE_FIXTURES("hls_ts");
  Served s;
  auto first = s.options("/media/hls_ts/index.m3u8");
  first.speed_limit = 60'000; // slow: cancel part-way
  first.connections = 1;
  std::stop_source stop;
  std::size_t total = 0;
  first.on_progress = [&](const engine::DownloadProgress &p) {
    total = p.parts_total;
    if (p.parts_done >= 1) {
      stop.request_stop();
    }
  };
  first.progress_interval = 20ms;
  const auto cancelled = media::download_any(first, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == ErrorCode::Cancelled);
  CHECK(fs::exists(s.out.path() / "hls_ts.mp4.parts"));
  REQUIRE(total >= 3);

  s.server.reset_stats();
  const auto resumed = media::download_any(s.options("/media/hls_ts/index.m3u8"));
  REQUIRE(resumed);
  check_av(resumed->path);
  // The playlist and the missing segments only, not every segment again.
  CHECK(s.server.stats().body_responses < total + 1);
}

TEST_CASE("DRM streams are refused before downloading anything", "[media][stream]") {
  TestServer server;
  server.add_static("/drm.m3u8",
                    "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://key\",KEYFORMAT="
                    "\"com.apple.streamingkeydelivery\"\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n",
                    "application/vnd.apple.mpegurl");
  server.start();
  TempDir dir;
  engine::DownloadOptions o;
  o.request.url = server.url("/drm.m3u8");
  o.directory = dir.path();
  const auto r = media::download_any(o);
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::Unsupported);
  CHECK(r.error().message.find("DRM") != std::string::npos);
  CHECK(fs::is_empty(dir.path()));
}

TEST_CASE("the queue engine downloads streams into the Video folder", "[media][stream][engine]") {
  REQUIRE_FIXTURES("hls_ts");
  Served s;
  auto engine = Engine::open({.database = s.out.path() / "lanes.db", .tick = 20ms});
  REQUIRE(engine);
  REQUIRE((*engine)->set_setting("download_dir", platform::path_to_utf8(s.out.path() / "dl")));
  NewDownload n;
  n.request.url = s.server.url("/media/hls_ts/index.m3u8");
  const auto id = (*engine)->add(n);
  REQUIRE(id);
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while ((*engine)->get(*id)->state != DownloadState::Completed &&
         (*engine)->get(*id)->state != DownloadState::Failed &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
  }
  const auto info = (*engine)->get(*id);
  INFO(info->error);
  REQUIRE(info->state == DownloadState::Completed);
  CHECK(info->category == "Video");
  check_av(s.out.path() / "dl" / "Video" / "hls_ts.mp4");
}

TEST_CASE("lanes info lists a stream's qualities", "[media][stream][cli]") {
  REQUIRE_FIXTURES("hls_ts", "hls_360");
  Served s;
  s.server.add_static("/media/master.m3u8",
                      "#EXTM3U\n"
                      "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=320x180\nhls_ts/index.m3u8\n"
                      "#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=640x360\nhls_360/index.m3u8\n",
                      "application/vnd.apple.mpegurl");
  platform::ProcessOptions o{platform::path_from_utf8(LANES_CLI_PATH),
                             {"info", s.server.url("/media/master.m3u8")}};
  const auto r = platform::run_capture(o, 30s);
  REQUIRE(r);
  CHECK(r->exit_code == 0);
  CHECK(r->out.find("Stream:        HLS") != std::string::npos);
  CHECK(r->out.find("640x360") != std::string::npos);
  CHECK(r->out.find("320x180") != std::string::npos);
}
