// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/net/probe.hpp"
#include "core/net/request.hpp"

using namespace lanes::core;
using namespace lanes::core::net;

TEST_CASE("Content-Disposition file names (RFC 6266)", "[net][disposition]") {
  CHECK(filename_from_content_disposition("attachment; filename=\"report.pdf\"") == "report.pdf");
  CHECK(filename_from_content_disposition("attachment; filename=plain.txt") == "plain.txt");
  CHECK(filename_from_content_disposition("ATTACHMENT; FILENAME=\"Upper.ZIP\"") == "Upper.ZIP");
  CHECK(filename_from_content_disposition("inline; filename=\"a \\\"quoted\\\" name.txt\"") ==
        "a \"quoted\" name.txt");
  CHECK(filename_from_content_disposition("attachment; filename=\"semi;colon.txt\"; size=10") ==
        "semi;colon.txt");

  // filename* wins, in either order.
  CHECK(filename_from_content_disposition(
            "attachment; filename=\"fallback.pdf\"; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf") ==
        "r\xC3\xA9sum\xC3\xA9.pdf");
  CHECK(filename_from_content_disposition(
            "attachment; filename*=utf-8''%E6%97%A5%E6%9C%AC.txt; filename=\"x.txt\"") ==
        "\xE6\x97\xA5\xE6\x9C\xAC.txt");
  CHECK(filename_from_content_disposition("attachment; filename*=ISO-8859-1'en'caf%E9.txt") ==
        "caf\xC3\xA9.txt");

  // Broken filename* falls back to filename; raw Latin-1 bytes are converted.
  CHECK(filename_from_content_disposition(
            "attachment; filename*=UTF-8''bad%FF.txt; filename=\"good.txt\"") == "good.txt");
  CHECK(filename_from_content_disposition("attachment; filename=\"caf\xE9.txt\"") ==
        "caf\xC3\xA9.txt");

  CHECK_FALSE(filename_from_content_disposition("attachment"));
  CHECK_FALSE(filename_from_content_disposition("inline; filename=\"\""));
}

TEST_CASE("file name from URL path", "[net][url]") {
  CHECK(filename_from_url("https://example.com/files/setup.exe") == "setup.exe");
  CHECK(filename_from_url("https://example.com/a/b%20c.zip?token=1#frag") == "b c.zip");
  CHECK(filename_from_url("https://example.com/dir/") == "");
  CHECK(filename_from_url("https://example.com") == "");
  CHECK(filename_from_url("https://example.com/r%C3%A9sum%C3%A9.pdf") ==
        "r\xC3\xA9sum\xC3\xA9.pdf");
  CHECK(filename_from_url("https://example.com/a%2Fb.txt") == "a/b.txt"); // sanitized later
  CHECK(filename_from_url("not a url") == "");
}

TEST_CASE("suggest_filename prefers Content-Disposition and sanitizes", "[net][url]") {
  CHECK(suggest_filename("attachment; filename=\"x:y.txt\"", "https://h/ignored.bin") == "x_y.txt");
  CHECK(suggest_filename("", "https://h/path/file.iso?x=1") == "file.iso");
  CHECK(suggest_filename("", "https://h/") == "download");
  CHECK(suggest_filename("", "https://h/a%2Fb.txt") == "a_b.txt");
}

TEST_CASE("suggest_filename adds the extension the content type stands for", "[net][url]") {
  CHECK(suggest_filename("", "https://h/watch/365197", "video/mp4") == "365197.mp4");
  CHECK(suggest_filename("", "https://h/", "Video/MP4; charset=binary") == "download.mp4");
  CHECK(suggest_filename("", "https://h/get.php?id=4", "application/zip") == "get.zip");
  CHECK(suggest_filename("", "https://h/clip.mkv", "video/mp4") == "clip.mkv"); // kept
  CHECK(suggest_filename("", "https://h/365197", "application/octet-stream") == "365197");
  CHECK(suggest_filename("", "https://h/v1.2%20final", "audio/mpeg") == "v1.2 final.mp3");
  CHECK(suggest_filename("attachment; filename=report", "https://h/x", "application/pdf") ==
        "report.pdf");
  CHECK(extension_for_content_type("text/html") == "");
}

TEST_CASE("only http, https and ftp URLs are accepted", "[net][url]") {
  CHECK(validate_url("http://example.com/a"));
  CHECK(validate_url("HTTPS://example.com/a"));
  CHECK(validate_url("ftp://example.com/a"));
  CHECK(validate_url("ftps://example.com/a"));
  const auto file = validate_url("file:///C:/Windows/win.ini");
  REQUIRE_FALSE(file);
  CHECK(file.error().code == ErrorCode::InvalidArgument);
  CHECK_FALSE(validate_url("javascript:alert(1)"));
  CHECK_FALSE(validate_url("example.com/no-scheme"));
  CHECK_FALSE(validate_url(""));
}
