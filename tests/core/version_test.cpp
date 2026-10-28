// SPDX-License-Identifier: GPL-3.0-or-later
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "core/version.hpp"

TEST_CASE("version string matches version components", "[core][version]") {
  using namespace lanes::core;
  const std::string expected = std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) +
                               "." + std::to_string(kVersionPatch);
  CHECK(kVersionString == expected);
}
