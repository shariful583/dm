// SPDX-License-Identifier: GPL-3.0-or-later
// Batch patterns, file categories and free file names.
#include <catch2/catch_test_macros.hpp>

#include "core/batch/batch.hpp"
#include "core/category/category.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "platform/temp_dir.hpp"

using namespace lanes::core;

TEST_CASE("URL patterns expand numeric and letter ranges", "[batch]") {
  const auto padded = batch::expand_pattern("https://x/img[001-003].jpg");
  REQUIRE(padded);
  CHECK(*padded == std::vector<std::string>{"https://x/img001.jpg", "https://x/img002.jpg",
                                            "https://x/img003.jpg"});

  const auto plain = batch::expand_pattern("https://x/p[9-11].html");
  REQUIRE(plain);
  CHECK(*plain ==
        std::vector<std::string>{"https://x/p9.html", "https://x/p10.html", "https://x/p11.html"});

  const auto letters = batch::expand_pattern("https://x/[a-c].png");
  REQUIRE(letters);
  CHECK(letters->size() == 3);
  CHECK(letters->back() == "https://x/c.png");

  const auto down = batch::expand_pattern("https://x/v[3-1]");
  REQUIRE(down);
  CHECK(down->front() == "https://x/v3");

  // Several groups: the last varies fastest.
  const auto grid = batch::expand_pattern("https://x/[a-b]/[1-2].bin");
  REQUIRE(grid);
  CHECK(*grid == std::vector<std::string>{"https://x/a/1.bin", "https://x/a/2.bin",
                                          "https://x/b/1.bin", "https://x/b/2.bin"});
}

TEST_CASE("brackets that are not ranges stay as they are", "[batch]") {
  CHECK_FALSE(batch::has_pattern("http://[::1]:8080/file.bin"));
  CHECK(*batch::expand_pattern("http://[::1]:8080/f[1-2]") ==
        std::vector<std::string>{"http://[::1]:8080/f1", "http://[::1]:8080/f2"});
  CHECK_FALSE(batch::has_pattern("https://x/[a-9]"));
  CHECK_FALSE(batch::has_pattern("https://x/[A-z]")); // mixed case is not a range
  CHECK(batch::expand_pattern("https://x/plain")->size() == 1);
}

TEST_CASE("oversized patterns are refused", "[batch]") {
  CHECK_FALSE(batch::expand_pattern("https://x/[1-20000]"));
  CHECK_FALSE(batch::expand_pattern("https://x/[1-200]/[1-200]"));
  CHECK(batch::expand_pattern("https://x/[1-10000]")->size() == 10000);
}

TEST_CASE("URL lists skip blanks and comments", "[batch]") {
  const auto urls = batch::parse_url_list("\xEF\xBB\xBFhttps://a/1\r\n\n  # comment\n\t"
                                          "https://a/2  \r\nhttps://a/3");
  CHECK(urls == std::vector<std::string>{"https://a/1", "https://a/2", "https://a/3"});
  CHECK(batch::parse_url_list("").empty());
}

TEST_CASE("files are matched to categories by extension", "[category]") {
  const auto cats = category::default_categories();
  CHECK(category::category_name(cats, "movie.MKV") == "Video");
  CHECK(category::category_name(cats, "song.flac") == "Music");
  CHECK(category::category_name(cats, "setup.exe") == "Programs");
  CHECK(category::category_name(cats, "report.pdf") == "Documents");
  CHECK(category::category_name(cats, "backup.tar.gz") == "Compressed");
  CHECK(category::category_name(cats, "README") == "General");
  CHECK(category::category_name(cats, "odd.xyz") == "General");

  const std::filesystem::path base = "D:\\Downloads";
  CHECK(category::folder_for(cats, "a.mp4", base) == base / "Video");
  CHECK(category::folder_for(cats, "a.xyz", base) == base);

  auto custom = cats;
  custom[0].folder = "Archives";  // relative
  custom[4].folder = "E:\\Films"; // absolute
  CHECK(category::folder_for(custom, "a.zip", base) == base / "Archives");
  CHECK(category::folder_for(custom, "a.mp4", base) == std::filesystem::path("E:\\Films"));
}

TEST_CASE("categories round-trip through JSON", "[category]") {
  const auto cats = category::default_categories();
  const auto parsed = category::from_json(category::to_json(cats));
  REQUIRE(parsed);
  REQUIRE(parsed->size() == cats.size());
  CHECK((*parsed)[4].name == "Video");
  CHECK((*parsed)[4].extensions == cats[4].extensions);
  CHECK_FALSE(category::from_json("{}"));
  CHECK_FALSE(category::from_json(R"([{"extensions":[]}])"));
  CHECK(category::parse_extensions(" .MP4, mkv;webm,mkv ") ==
        std::vector<std::string>{"mp4", "mkv", "webm"});
}

TEST_CASE("unique_filename picks the first free name", "[platform][path]") {
  TempDir dir;
  CHECK(platform::unique_filename(dir.path(), "a.bin") == "a.bin");
  REQUIRE(platform::write_file_atomic(dir.path() / "a.bin", "x"));
  CHECK(platform::unique_filename(dir.path(), "a.bin") == "a (2).bin");
  REQUIRE(platform::write_file_atomic(dir.path() / "a (2).bin.part", "x")); // in progress
  CHECK(platform::unique_filename(dir.path(), "a.bin") == "a (3).bin");
  REQUIRE(platform::write_file_atomic(dir.path() / "README", "x"));
  CHECK(platform::unique_filename(dir.path(), "README") == "README (2)");
}
