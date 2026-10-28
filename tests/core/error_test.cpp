// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "core/error.hpp"

namespace {

lanes::core::Result<int> parse_positive(int value) {
  if (value <= 0) {
    return lanes::core::make_error(lanes::core::ErrorCode::InvalidArgument, "not positive");
  }
  return value;
}

} // namespace

TEST_CASE("Result carries a value or an Error", "[core][error]") {
  using namespace lanes::core;

  const auto ok = parse_positive(5);
  REQUIRE(ok.has_value());
  CHECK(*ok == 5);

  const auto bad = parse_positive(-1);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code == ErrorCode::InvalidArgument);
  CHECK(bad.error().message == "not positive");
}

TEST_CASE("ErrorCode names are stable", "[core][error]") {
  using lanes::core::ErrorCode;
  using lanes::core::to_string;
  CHECK(to_string(ErrorCode::InvalidArgument) == "invalid_argument");
  CHECK(to_string(ErrorCode::Http) == "http");
  CHECK(to_string(ErrorCode::Cancelled) == "cancelled");
}
