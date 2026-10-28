// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "core/platform/path.hpp"

using lanes::core::platform::path_from_utf8;
using lanes::core::platform::path_to_utf8;
using lanes::core::platform::sanitize_filename;
using lanes::core::platform::to_long_path;

TEST_CASE("sanitize_filename keeps ordinary names", "[platform][path]") {
  CHECK(sanitize_filename("report.pdf") == "report.pdf");
  CHECK(sanitize_filename("Ubuntu 24.04 (amd64).iso") == "Ubuntu 24.04 (amd64).iso");
  CHECK(sanitize_filename("r\xC3\xA9sum\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.txt") ==
        "r\xC3\xA9sum\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.txt"); // résumé 日本.txt
  CHECK(sanitize_filename(".gitignore") == ".gitignore");
}

TEST_CASE("sanitize_filename replaces forbidden characters", "[platform][path]") {
  CHECK(sanitize_filename("a<b>c:d\"e/f\\g|h?i*j") == "a_b_c_d_e_f_g_h_i_j");
  CHECK(sanitize_filename(std::string("tab\there\x01") + "x") == "tab_here_x");
  CHECK(sanitize_filename("bad\xFF\xFEutf8") == "bad__utf8");
  CHECK(sanitize_filename("over\xC0\xAFlong") == "over__long"); // overlong '/'
}

TEST_CASE("sanitize_filename trims dots and spaces", "[platform][path]") {
  CHECK(sanitize_filename("  name.txt. . ") == "name.txt");
  CHECK(sanitize_filename("...") == "download");
  CHECK(sanitize_filename("..") == "download");
  CHECK(sanitize_filename("") == "download");
  CHECK(sanitize_filename("   ", "file.bin") == "file.bin");
}

TEST_CASE("sanitize_filename escapes reserved device names", "[platform][path]") {
  CHECK(sanitize_filename("CON") == "_CON");
  CHECK(sanitize_filename("con.txt") == "_con.txt");
  CHECK(sanitize_filename("Nul.tar.gz") == "_Nul.tar.gz");
  CHECK(sanitize_filename("COM1") == "_COM1");
  CHECK(sanitize_filename("lpt9.log") == "_lpt9.log");
  CHECK(sanitize_filename("COM\xC2\xB9") == "_COM\xC2\xB9"); // COM¹
  CHECK(sanitize_filename("AUX .txt") == "_AUX .txt");
  CHECK(sanitize_filename("CONSOLE.txt") == "CONSOLE.txt");
  CHECK(sanitize_filename("COM10") == "COM10");
}

TEST_CASE("sanitize_filename limits length and keeps the extension", "[platform][path]") {
  const std::string long_name = std::string(300, 'a') + ".mp4";
  const std::string cut = sanitize_filename(long_name);
  CHECK(cut.size() == 255);
  CHECK(cut.ends_with("a.mp4"));

  // Limit counts UTF-16 units: an emoji is 2 units, and is never split.
  std::string emoji;
  for (int i = 0; i < 10; ++i) {
    emoji += "\xF0\x9F\x98\x80"; // 😀
  }
  CHECK(sanitize_filename(emoji + ".txt", "x", 9) == "\xF0\x9F\x98\x80\xF0\x9F\x98\x80.txt");
  CHECK(sanitize_filename(std::string(20, 'b'), "x", 10) == std::string(10, 'b'));
}

TEST_CASE("UTF-8 path round trip", "[platform][path]") {
  const std::string name = "dir/\xE6\x97\xA5\xE6\x9C\xAC/r\xC3\xA9sum\xC3\xA9.pdf";
  CHECK(path_to_utf8(path_from_utf8(name)) == name);
}

#ifdef _WIN32
TEST_CASE("to_long_path adds the \\\\?\\ prefix", "[platform][path]") {
  CHECK(to_long_path(L"C:\\dir\\..\\file.txt").native() == L"\\\\?\\C:\\file.txt");
  CHECK(to_long_path(L"C:/a/b/c.txt").native() == L"\\\\?\\C:\\a\\b\\c.txt");
  CHECK(to_long_path(L"\\\\server\\share\\f.bin").native() == L"\\\\?\\UNC\\server\\share\\f.bin");
  CHECK(to_long_path(L"\\\\?\\D:\\x").native() == L"\\\\?\\D:\\x");
  CHECK(to_long_path(L"relative.txt").native().starts_with(L"\\\\?\\"));
}
#endif
