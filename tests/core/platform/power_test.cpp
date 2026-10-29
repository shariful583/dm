// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "core/platform/power.hpp"
#include "core/platform/registry.hpp"
#include "temp_dir.hpp"

using namespace lanes::core;
using namespace lanes::core::platform;

TEST_CASE("the computer is kept awake on request", "[platform][power]") {
  KeepAwake awake;
  CHECK_FALSE(awake.active());
  awake.set(true);
  CHECK(awake.active());
  awake.set(true); // no change
  CHECK(awake.active());
  awake.set(false);
  CHECK_FALSE(awake.active());
}

TEST_CASE("finished files get the Mark of the Web", "[platform][motw]") {
  TempDir dir;
  const auto file = dir.path() / "report.txt";
  std::ofstream(file) << "just text\n";
  REQUIRE(mark_and_scan(file, "https://user:secret@example.com/files/report.txt",
                        "https://example.com/page"));
  CHECK(std::filesystem::exists(file)); // the antivirus had nothing against it
  // The zone lives in an alternate data stream, as browsers write it.
  std::ifstream zone(file.wstring() + L":Zone.Identifier");
  REQUIRE(zone);
  std::stringstream text;
  text << zone.rdbuf();
  const std::string z = text.str();
  INFO(z);
  CHECK(z.find("[ZoneTransfer]") != std::string::npos);
  CHECK(z.find("ZoneId=3") != std::string::npos); // Internet
  CHECK(z.find("HostUrl=https://example.com/files/report.txt") != std::string::npos);
  CHECK(z.find("ReferrerUrl=https://example.com/page") != std::string::npos);
  CHECK(z.find("secret") == std::string::npos); // credentials never written
}

TEST_CASE("named registry values", "[platform][registry]") {
  const std::string key = "Software\\LanesTest\\Values";
  REQUIRE(set_user_value(key, "Lanes", "\"C:\\Lanes\\lanes-gui.exe\" --tray"));
  auto value = user_value(key, "Lanes");
  REQUIRE(value);
  REQUIRE(value->has_value());
  CHECK(**value == "\"C:\\Lanes\\lanes-gui.exe\" --tray");
  REQUIRE(delete_user_value(key, "Lanes"));
  CHECK_FALSE(user_value(key, "Lanes")->has_value());
  CHECK(delete_user_value(key, "Lanes")); // already gone: fine
  REQUIRE(delete_user_key("Software\\LanesTest"));
}
