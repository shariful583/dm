// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "../platform/temp_dir.hpp"
#include "core/service/service.hpp"

using namespace lanes::core;
using nlohmann::json;

TEST_CASE("activate reaches the window owner, or is unsupported without one", "[service]") {
  TempDir dir;
  auto engine = Engine::open({.database = dir.path() / "lanes.db"});
  REQUIRE(engine);
  service::EngineService service(**engine, {});

  const auto reply = [&](const json &request) {
    return json::parse(service.handle(request.dump()));
  };
  const json request = {{"v", 1}, {"cmd", "activate"}, {"args", {"https://example.com/a.zip", 7}}};
  const json refused = reply(request);
  CHECK_FALSE(refused["ok"].get<bool>());
  CHECK(refused["error"]["code"] == "unsupported");

  std::vector<std::string> got;
  service.set_on_activate([&](std::vector<std::string> args) { got = std::move(args); });
  CHECK(reply(request)["ok"].get<bool>());
  CHECK(got == std::vector<std::string>{"https://example.com/a.zip"}); // non-strings dropped
}
