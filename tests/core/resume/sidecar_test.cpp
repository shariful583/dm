// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "core/net/request.hpp"
#include "core/platform/fs.hpp"
#include "core/resume/sidecar.hpp"

using namespace lanes::core;
using namespace lanes::core::resume;

namespace {

ResumeState sample() {
  ResumeState s;
  s.url = "https://example.com/a.bin?sig=1";
  s.final_url = "https://cdn.example.com/a.bin";
  s.filename = "a.bin";
  s.size = 1000;
  s.etag = "\"abc\"";
  s.last_modified = "Wed, 01 Jan 2025 00:00:00 GMT";
  s.segments = {{0, 499, 500}, {500, 749, 10}, {750, 999, 0}};
  return s;
}

} // namespace

TEST_CASE("sidecar JSON round trip", "[resume]") {
  const ResumeState s = sample();
  CHECK(s.done_bytes() == 510);
  const auto parsed = from_json(to_json(s));
  REQUIRE(parsed);
  CHECK(parsed->url == s.url);
  CHECK(parsed->final_url == s.final_url);
  CHECK(parsed->filename == "a.bin");
  CHECK(parsed->size == 1000);
  CHECK(parsed->etag == s.etag);
  CHECK(parsed->last_modified == s.last_modified);
  REQUIRE(parsed->segments.size() == 3);
  CHECK(parsed->segments[1].start == 500);
  CHECK(parsed->segments[1].end == 749);
  CHECK(parsed->segments[1].done == 10);
}

TEST_CASE("sidecar validation rejects anything inconsistent", "[resume]") {
  const auto rejects = [](const ResumeState &s) {
    const auto r = from_json(to_json(s));
    return !r && r.error().code == ErrorCode::InvalidArgument;
  };
  ResumeState gap = sample();
  gap.segments[1].start = 501; // hole at 500
  CHECK(rejects(gap));
  ResumeState overlap = sample();
  overlap.segments[1].start = 400;
  CHECK(rejects(overlap));
  ResumeState short_cover = sample();
  short_cover.segments.pop_back();
  CHECK(rejects(short_cover));
  ResumeState too_much = sample();
  too_much.segments[1].done = 251;
  CHECK(rejects(too_much));
  ResumeState beyond = sample();
  beyond.segments[2].end = 1000;
  CHECK(rejects(beyond));

  CHECK_FALSE(from_json("not json"));
  CHECK_FALSE(from_json("{}"));
  CHECK_FALSE(from_json(R"({"version":99})"));
}

TEST_CASE("sidecar save and load", "[resume]") {
  TempDir dir;
  const auto path = sidecar_path(dir.path() / "a.bin");
  CHECK(path.filename() == "a.bin.lanes.json");

  const auto missing = load(path);
  REQUIRE(missing);
  CHECK_FALSE(*missing);

  REQUIRE(save(path, sample()));
  const auto loaded = load(path);
  REQUIRE(loaded);
  REQUIRE(*loaded);
  CHECK((*loaded)->done_bytes() == 510);

  REQUIRE(platform::write_file_atomic(path, "{ truncated"));
  const auto corrupt = load(path);
  REQUIRE_FALSE(corrupt);
  CHECK(corrupt.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("stored URLs drop embedded credentials", "[resume][net]") {
  CHECK(net::strip_credentials("https://user:s3cret@example.com/a.bin?x=1") ==
        "https://example.com/a.bin?x=1");
  CHECK(net::strip_credentials("ftp://anon@example.com/f") == "ftp://example.com/f");
  CHECK(net::strip_credentials("https://example.com/plain") == "https://example.com/plain");
}
