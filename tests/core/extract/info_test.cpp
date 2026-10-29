// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "core/extract/info.hpp"
#include "core/extract/updater.hpp"

using namespace lanes::core;
using namespace lanes::core::extract;

namespace {

std::string fixture(const char *name) {
  std::ifstream in(std::string(LANES_FIXTURES_DIR) + "/ytdlp/" + name, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

VideoInfo video() {
  auto info = parse_info(fixture("video.json"));
  REQUIRE(info);
  return *info;
}

std::string ids(const Selection &s) {
  std::string out = s.video != nullptr ? s.video->id : "";
  if (s.audio != nullptr) {
    out += (out.empty() ? "" : "+") + s.audio->id;
  }
  return out;
}

std::string pick(const VideoInfo &info, std::string_view choice) {
  auto s = select_formats(info, choice);
  INFO(choice);
  REQUIRE(s);
  return ids(*s) + " " + s->container;
}

} // namespace

TEST_CASE("yt-dlp video information is parsed", "[extract]") {
  const VideoInfo info = video();
  CHECK(info.title == "Lanes Test Video: \"part 1\"");
  CHECK(info.extractor == "youtube");
  CHECK(info.duration == 2);
  CHECK(info.thumbnail.ends_with("/yt/thumb.jpg?sqp=1")); // the largest JPEG, not WebP
  CHECK_FALSE(info.is_live);
  CHECK(info.formats.size() == 13);
  const auto find = [&](std::string_view id) {
    return *std::find_if(info.formats.begin(), info.formats.end(),
                         [&](const Format &f) { return f.id == id; });
  };
  CHECK(find("137").height == 1080);
  CHECK(find("137").http_headers.size() == 2);
  CHECK(find("140").filesize == 31000u);
  CHECK(find("140").size_is_estimate);
  CHECK(find("drm-2160").has_drm);
  CHECK(find("773").has_audio());
  CHECK_FALSE(find("773").has_video()); // codecs unknown, but video_ext says "none"
  REQUIRE(info.subtitles.size() == 2);  // uploaded subtitles only, not automatic captions
  CHECK(info.subtitles[0].first == "de-DE");
  CHECK(info.subtitles[1].second.size() == 2);

  auto playlist = parse_info(fixture("playlist.json"));
  REQUIRE(playlist);
  CHECK(playlist->is_playlist);
  REQUIRE(playlist->entries.size() == 3);
  CHECK(playlist->entries[1].url == "https://www.youtube.com/watch?v=b2");
  CHECK(playlist->entries[2].title == "Third");

  CHECK_FALSE(parse_info("not json"));
  CHECK(parse_info(R"({"id":"x","title":"no formats"})").error().code == ErrorCode::NotFound);
}

TEST_CASE("quality choices pick the fastest, most compatible formats", "[extract]") {
  const VideoInfo info = video();
  // 1080p: AVC beats VP9 for MP4; AAC beats Opus, AC-3 and the DRC copy; no DRM 2160p.
  CHECK(pick(info, "best") == "137+140 mp4");
  CHECK(select_formats(info, "best")->size == 64000u + 31000u);
  CHECK(pick(info, "1080p") == "137+140 mp4");
  CHECK(pick(info, "720p") == "136+140 mp4"); // HTTPS (segmented) before HLS at the same height
  CHECK(pick(info, "360p") == "18 mp4");      // a format with both streams needs no audio
  CHECK(pick(info, "100p") == "18 mp4");      // nothing that small: the smallest there is
  CHECK(pick(info, "audio") == "140 m4a");
  CHECK(select_formats(info, "mp3")->to_mp3);
  CHECK(pick(info, "mp3") == "380 mp3"); // the best audio of any kind, converted anyway
  CHECK(pick(info, "95") == "95 mp4");   // HLS ends up as MP4
  CHECK(pick(info, "248+251") == "248+251 webm");
  CHECK(pick(info, "137+251") == "137+251 mkv"); // Opus does not go into MP4
  CHECK(select_formats(info, "999").error().code == ErrorCode::NotFound);
  CHECK(select_formats(info, "drm-2160").error().code == ErrorCode::Unsupported);

  std::vector<std::string> labels;
  for (const auto &[label, s] : quality_choices(info)) {
    labels.push_back(label);
  }
  CHECK(labels == std::vector<std::string>{"1080p", "720p", "360p", "audio", "mp3"});

  VideoInfo live = info;
  live.is_live = true;
  CHECK(select_formats(live, "best").error().code == ErrorCode::Unsupported);
  VideoInfo drm_only;
  drm_only.formats = {info.formats[11]};
  CHECK(select_formats(drm_only, "best").error().code == ErrorCode::Unsupported);
}

TEST_CASE("cookies become a Netscape cookie file for the site", "[extract]") {
  const std::string file = netscape_cookies("SID=abc; theme=dark mode; bad\tname=1; =x",
                                            "https://www.youtube.com:443/watch?v=1");
  CHECK(file.starts_with("# Netscape HTTP Cookie File\n"));
  CHECK(file.find(".youtube.com\tTRUE\t/\tFALSE\t2147483647\tSID\tabc\n") != std::string::npos);
  CHECK(file.find("\ttheme\tdark mode\n") != std::string::npos);
  CHECK(file.find("bad") == std::string::npos); // tabs would break the format
}

TEST_CASE("yt-dlp progress lines and versions are understood", "[extract]") {
  auto p = parse_progress_line("[download] LANES|1024|4096|NA|512.5");
  REQUIRE(p);
  CHECK(p->downloaded == 1024);
  CHECK(p->total == 4096u);
  CHECK(p->speed == 512.5);
  auto estimate = parse_progress_line("LANES|10|NA|200.0|NA");
  REQUIRE(estimate);
  CHECK(estimate->total == 200u);
  CHECK(estimate->speed == 0);
  CHECK_FALSE(parse_progress_line("[download]  12.5% of 3.00MiB"));
  CHECK_FALSE(parse_progress_line("LANES|NA|NA|NA|NA"));

  CHECK(newer_version("2026.10.26", "2026.09.30"));
  CHECK(newer_version("2026.10.26.1", "2026.10.26"));
  CHECK_FALSE(newer_version("2026.10.26", "2026.10.26"));
  CHECK_FALSE(newer_version("2025.12.31", "2026.01.01"));
}
