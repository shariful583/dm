// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/media/dash.hpp"
#include "core/media/hls.hpp"
#include "core/media/stream.hpp"

using namespace lanes::core;
using namespace lanes::core::media;

TEST_CASE("relative URLs resolve like a browser", "[media][hls]") {
  CHECK(hls::resolve_url("https://cdn.x/v/1/master.m3u8", "low/index.m3u8") ==
        "https://cdn.x/v/1/low/index.m3u8");
  CHECK(hls::resolve_url("https://cdn.x/v/1/master.m3u8", "../key.bin") ==
        "https://cdn.x/v/key.bin");
  CHECK(hls::resolve_url("https://cdn.x/v/1/master.m3u8", "/abs/k") == "https://cdn.x/abs/k");
  CHECK(hls::resolve_url("https://cdn.x/a.m3u8", "https://other.y/s.ts") == "https://other.y/s.ts");
}

TEST_CASE("HLS master playlists list variants and renditions", "[media][hls]") {
  const std::string text = "#EXTM3U\n"
                           "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"English\","
                           "LANGUAGE=\"en\",DEFAULT=YES,URI=\"audio/en.m3u8\"\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,"
                           "CODECS=\"avc1.4d401e,mp4a.40.2\",AUDIO=\"aud\"\n"
                           "low/index.m3u8\n"
                           "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720\n"
                           "https://cdn2.x/high.m3u8\n";
  REQUIRE(hls::is_master(text));
  const auto m = hls::parse_master(text, "https://cdn.x/v/master.m3u8");
  REQUIRE(m);
  REQUIRE(m->variants.size() == 2);
  CHECK(m->variants[0].uri == "https://cdn.x/v/low/index.m3u8");
  CHECK(m->variants[0].height == 360);
  CHECK(m->variants[0].codecs == "avc1.4d401e,mp4a.40.2"); // comma inside quotes
  CHECK(m->variants[0].audio_group == "aud");
  CHECK(m->variants[1].uri == "https://cdn2.x/high.m3u8");
  CHECK(m->variants[1].bandwidth == 2'500'000);
  REQUIRE(m->renditions.size() == 1);
  CHECK(m->renditions[0].uri == "https://cdn.x/v/audio/en.m3u8");
  CHECK(m->renditions[0].is_default);
  CHECK(m->renditions[0].language == "en");
}

TEST_CASE("HLS media playlists: segments, keys, init, byte ranges", "[media][hls]") {
  const std::string text = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-MEDIA-SEQUENCE:10\n"
                           "#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=\"720@0\"\n"
                           "#EXTINF:4,\nseg0.m4s\n"
                           "#EXT-X-KEY:METHOD=AES-128,URI=\"https://k.x/key\",IV=0x1\n"
                           "#EXTINF:4,\n#EXT-X-BYTERANGE:1000@200\nall.ts\n"
                           "#EXTINF:4,\n#EXT-X-BYTERANGE:500\nall.ts\n"
                           "#EXT-X-KEY:METHOD=NONE\n"
                           "#EXTINF:4,\nlast.ts\n#EXT-X-ENDLIST\n";
  const auto t = hls::parse_media(text, "https://cdn.x/p/index.m3u8");
  REQUIRE(t);
  REQUIRE(t->init);
  CHECK(t->init->url == "https://cdn.x/p/init.mp4");
  CHECK(t->init->range == ByteRange{0, 720});
  REQUIRE(t->parts.size() == 4);
  CHECK(t->parts[0].key == -1);
  CHECK(t->parts[0].sequence == 10);
  CHECK(t->parts[1].key == 0);
  CHECK(t->parts[1].range == ByteRange{200, 1000});
  CHECK(t->parts[2].range == ByteRange{1200, 500}); // continues after the previous range
  CHECK(t->parts[3].key == -1);
  CHECK(t->parts[3].sequence == 13);
  REQUIRE(t->keys.size() == 1);
  CHECK(t->keys[0].uri == "https://k.x/key");
  REQUIRE(t->keys[0].iv);
  CHECK(t->keys[0].iv->size() == 16);
  CHECK((*t->keys[0].iv)[15] == 1);
}

TEST_CASE("DRM, live and broken HLS playlists are refused clearly", "[media][hls]") {
  const auto drm = hls::parse_media(
      "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\"\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n",
      "https://x/i.m3u8");
  REQUIRE_FALSE(drm);
  CHECK(drm.error().code == ErrorCode::Unsupported);
  CHECK(drm.error().message.find("DRM") != std::string::npos);

  const auto widevine = hls::parse_media(
      "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\",KEYFORMAT=\"urn:uuid:edef8ba9\"\n"
      "#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n",
      "https://x/i.m3u8");
  CHECK(widevine.error().code == ErrorCode::Unsupported);

  const auto live = hls::parse_media("#EXTM3U\n#EXTINF:4,\na.ts\n", "https://x/i.m3u8");
  REQUIRE_FALSE(live);
  CHECK(live.error().message.find("live") != std::string::npos);

  CHECK(hls::parse_media("<html>", "https://x/i.m3u8").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("ISO 8601 durations", "[media][dash]") {
  CHECK(dash::parse_iso_duration("PT1H2M3.5S") == Catch::Approx(3723.5));
  CHECK(dash::parse_iso_duration("PT0.96S") == Catch::Approx(0.96));
  CHECK(dash::parse_iso_duration("P1DT1S") == Catch::Approx(86401));
  CHECK(dash::parse_iso_duration("1H") < 0);
}

TEST_CASE("DASH SegmentTemplate with $Number$ and a duration", "[media][dash]") {
  const std::string mpd = R"(<?xml version="1.0"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT9.5S">
 <BaseURL>media/</BaseURL>
 <Period>
  <AdaptationSet mimeType="video/mp4">
   <SegmentTemplate initialization="$RepresentationID$/init.mp4" media="$RepresentationID$/$Number%05d$.m4s" startNumber="1" timescale="1000" duration="4000"/>
   <Representation id="v720" bandwidth="2000000" width="1280" height="720" codecs="avc1.64001f"/>
  </AdaptationSet>
  <AdaptationSet contentType="audio" lang="en">
   <Representation id="a1" bandwidth="128000" mimeType="audio/mp4">
    <SegmentTemplate initialization="a/init.mp4" media="a/$Number$.m4s" timescale="1" duration="4"/>
   </Representation>
  </AdaptationSet>
 </Period>
</MPD>)";
  const auto m = dash::parse_mpd(mpd, "https://cdn.x/show/manifest.mpd");
  REQUIRE(m);
  CHECK(m->duration_seconds == Catch::Approx(9.5));
  REQUIRE(m->representations.size() == 2);
  const auto &v = m->representations[0];
  CHECK(v.kind == "video");
  CHECK(v.height == 720);
  REQUIRE(v.track.init);
  CHECK(v.track.init->url == "https://cdn.x/show/media/v720/init.mp4");
  REQUIRE(v.track.parts.size() == 3); // ceil(9.5 / 4)
  CHECK(v.track.parts[0].url == "https://cdn.x/show/media/v720/00001.m4s");
  CHECK(v.track.parts[2].url == "https://cdn.x/show/media/v720/00003.m4s");
  const auto &a = m->representations[1];
  CHECK(a.kind == "audio");
  CHECK(a.language == "en");
  CHECK(a.track.parts[1].url == "https://cdn.x/show/media/a/2.m4s");
}

TEST_CASE("DASH SegmentTimeline with repeats and $Time$", "[media][dash]") {
  const std::string mpd = R"(<MPD type="static" mediaPresentationDuration="PT10S"><Period>
  <AdaptationSet mimeType="video/mp4"><Representation id="v" bandwidth="1">
   <SegmentTemplate media="v_$Time$.m4s" initialization="v_init.mp4" timescale="10">
    <SegmentTimeline><S t="0" d="20" r="2"/><S d="30"/><S d="10" r="-1"/></SegmentTimeline>
   </SegmentTemplate>
  </Representation></AdaptationSet></Period></MPD>)";
  const auto m = dash::parse_mpd(mpd, "https://x/m.mpd");
  REQUIRE(m);
  const auto &parts = m->representations[0].track.parts;
  // 0,20,40 (r=2) then 60 (d=30) then 90..99 at d=10 until the 10 s end: 90.
  REQUIRE(parts.size() == 5);
  CHECK(parts[0].url == "https://x/v_0.m4s");
  CHECK(parts[2].url == "https://x/v_40.m4s");
  CHECK(parts[3].url == "https://x/v_60.m4s");
  CHECK(parts[4].url == "https://x/v_90.m4s");
}

TEST_CASE("DASH SegmentList and single-file representations", "[media][dash]") {
  const std::string mpd = R"(<MPD type="static" mediaPresentationDuration="PT8S"><Period>
  <AdaptationSet mimeType="video/mp4">
   <Representation id="v" bandwidth="1"><BaseURL>video.mp4</BaseURL>
    <SegmentList><Initialization range="0-99"/><SegmentURL mediaRange="100-199"/><SegmentURL mediaRange="200-299"/></SegmentList>
   </Representation></AdaptationSet>
  <AdaptationSet mimeType="audio/mp4"><Representation id="a" bandwidth="1"><BaseURL>https://other.x/audio.m4a</BaseURL><SegmentBase indexRange="0-50"/></Representation></AdaptationSet>
  </Period></MPD>)";
  const auto m = dash::parse_mpd(mpd, "https://x/dir/m.mpd");
  REQUIRE(m);
  const auto &v = m->representations[0].track;
  REQUIRE(v.init);
  CHECK(v.init->url == "https://x/dir/video.mp4");
  CHECK(v.init->range == ByteRange{0, 100});
  REQUIRE(v.parts.size() == 2);
  CHECK(v.parts[1].range == ByteRange{200, 100});
  const auto &a = m->representations[1].track;
  REQUIRE(a.parts.size() == 1); // the whole file
  CHECK(a.parts[0].url == "https://other.x/audio.m4a");
  CHECK_FALSE(a.parts[0].range);
}

TEST_CASE("DRM and live DASH are refused clearly", "[media][dash]") {
  const auto drm = dash::parse_mpd(R"(<MPD type="static" mediaPresentationDuration="PT8S"><Period>
   <AdaptationSet mimeType="video/mp4"><ContentProtection schemeIdUri="urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"/>
   <Representation id="v" bandwidth="1"><BaseURL>v.mp4</BaseURL></Representation></AdaptationSet></Period></MPD>)",
                                   "https://x/m.mpd");
  REQUIRE_FALSE(drm);
  CHECK(drm.error().code == ErrorCode::Unsupported);
  CHECK(drm.error().message.find("DRM") != std::string::npos);

  const auto live = dash::parse_mpd(R"(<MPD type="dynamic"><Period/></MPD>)", "https://x/m.mpd");
  CHECK(live.error().code == ErrorCode::Unsupported);
  CHECK(dash::parse_mpd("not xml", "https://x/m.mpd").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("streams are recognised by type or extension", "[media]") {
  CHECK(looks_like_stream("https://x/a/master.m3u8?token=1", ""));
  CHECK(looks_like_stream("https://x/manifest.MPD", ""));
  CHECK(looks_like_stream("https://x/play", "application/vnd.apple.mpegurl; charset=utf-8"));
  CHECK(looks_like_stream("https://x/play", "application/dash+xml"));
  CHECK_FALSE(looks_like_stream("https://x/video.mp4", "video/mp4"));
  CHECK_FALSE(looks_like_stream("https://x/list.m3u8.zip", "application/zip"));
}
