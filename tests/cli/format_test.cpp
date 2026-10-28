// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "cli/format.hpp"

using namespace lanes::cli;
using lanes::core::engine::DownloadProgress;
using lanes::core::engine::SegmentProgress;

TEST_CASE("parse_size accepts common spellings", "[cli][format]") {
  CHECK(*parse_size("100") == 100);
  CHECK(*parse_size("500K") == 500 * 1024);
  CHECK(*parse_size("500k") == 500 * 1024);
  CHECK(*parse_size("2M") == 2 * 1024 * 1024);
  CHECK(*parse_size("2MB") == 2 * 1024 * 1024);
  CHECK(*parse_size("2 MiB") == 2 * 1024 * 1024);
  CHECK(*parse_size("1.5G") == 1536ull * 1024 * 1024);
  CHECK(*parse_size("64B") == 64);
  CHECK_FALSE(parse_size(""));
  CHECK_FALSE(parse_size("M"));
  CHECK_FALSE(parse_size("5X"));
  CHECK_FALSE(parse_size("-1M"));
}

TEST_CASE("human-readable numbers", "[cli][format]") {
  CHECK(format_bytes(0) == "0 B");
  CHECK(format_bytes(1023) == "1023 B");
  CHECK(format_bytes(1536) == "1.5 KiB");
  CHECK(format_bytes(273ull << 20) == "273.0 MiB");
  CHECK(format_speed(12.3 * 1024 * 1024) == "12.3 MiB/s");
  CHECK(format_duration(0.04) == "0.0s");
  CHECK(format_duration(3.25) == "3.2s");
  CHECK(format_duration(45) == "45s");
  CHECK(format_duration(125) == "2:05");
  CHECK(format_duration(3723) == "1:02:03");
  CHECK(format_duration(-1) == "--");
}

TEST_CASE("segment map shows done, partial and empty cells", "[cli][format]") {
  // 4 cells over 400 bytes: [0,100) done, [100,200) half, [200,300) empty, [300,400) done.
  const std::vector<SegmentProgress> segments = {{0, 99, 100}, {100, 299, 50}, {300, 399, 100}};
  CHECK(segment_map(segments, 400, 4) == "\xE2\x96\x88\xE2\x96\x92\xE2\x96\x91\xE2\x96\x88");
  CHECK(display_width(segment_map(segments, 400, 30)) == 30);
  CHECK(segment_map(segments, 0, 10).empty());
}

TEST_CASE("progress line fits the console", "[cli][format]") {
  DownloadProgress p;
  p.downloaded = 100 << 20;
  p.total = 400ull << 20;
  p.bytes_per_second = 10 << 20;
  p.segments = {{0, (200ull << 20) - 1, 60 << 20}, {200ull << 20, (400ull << 20) - 1, 40 << 20}};
  for (const int columns : {40, 80, 120, 200}) {
    const std::string line = progress_line(p, "a-rather-long-file-name-for-testing.iso", columns);
    CHECK(display_width(line) <= static_cast<std::size_t>(columns - 1));
  }
  const std::string wide = progress_line(p, "file.iso", 120);
  CHECK(wide.find("file.iso") != std::string::npos);
  CHECK(wide.find(" 25.0%") != std::string::npos);
  CHECK(wide.find("10.0 MiB/s") != std::string::npos);
  CHECK(wide.find("ETA 30s") != std::string::npos);
  CHECK(wide.find("2 conn") != std::string::npos);
  CHECK(wide.find("\xE2\x96\x88") != std::string::npos); // the map is there

  DownloadProgress unknown; // chunked: no total
  unknown.downloaded = 5 << 20;
  const std::string plain = progress_line(unknown, "stream", 80);
  CHECK(plain.find("5.0 MiB") != std::string::npos);
  CHECK(plain.find('%') == std::string::npos);
}
