// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>
#include <sstream>
#include <thread>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "../core/platform/temp_dir.hpp"
#include "bridge/bridge.hpp"
#include "core/platform/ipc.hpp"
#include "core/platform/registry.hpp"
#include "core/service/service.hpp"

using namespace lanes;
using nlohmann::json;

namespace {

std::string frame(std::string_view body) {
  std::ostringstream out;
  REQUIRE(bridge::write_message(out, body));
  return out.str();
}

json valid(const json &message) {
  auto r = bridge::validate(message.dump());
  INFO((r ? std::string() : r.error().message));
  REQUIRE(r);
  return json::parse(*r);
}

std::string invalid(const json &message) {
  auto r = bridge::validate(message.dump());
  REQUIRE_FALSE(r);
  return r.error().message;
}

// Sets an environment variable for one test.
struct ScopedEnv {
  std::wstring name;
  ScopedEnv(const wchar_t *n, const std::wstring &value) : name(n) {
    ::SetEnvironmentVariableW(n, value.c_str());
  }
  ~ScopedEnv() { ::SetEnvironmentVariableW(name.c_str(), nullptr); }
};

} // namespace

TEST_CASE("native messages are framed with a little-endian length", "[bridge]") {
  const std::string framed = frame(R"({"type":"ping"})");
  CHECK(framed.substr(0, 4) == std::string("\x0f\x00\x00\x00", 4));
  std::istringstream in(framed + frame("{}"));
  CHECK(*bridge::read_message(in) == R"({"type":"ping"})");
  CHECK(*bridge::read_message(in) == "{}");
  CHECK(bridge::read_message(in).error().code == core::ErrorCode::NotFound); // clean end

  std::istringstream truncated(framed.substr(0, framed.size() - 2));
  CHECK(bridge::read_message(truncated).error().code == core::ErrorCode::InvalidArgument);
  std::istringstream huge(std::string("\xff\xff\xff\x7f", 4));
  CHECK(bridge::read_message(huge).error().code == core::ErrorCode::InvalidArgument);
  std::ostringstream out;
  CHECK_FALSE(bridge::write_message(out, std::string(bridge::kMaxOutgoing + 1, 'x')));
}

TEST_CASE("browser messages are validated and rebuilt from known fields", "[bridge]") {
  const json download = valid({{"v", 1},
                               {"type", "capture_download"},
                               {"url", "https://example.com/a.zip"},
                               {"fileSize", 1234},
                               {"cookies", "sid=1; theme=dark"},
                               {"referrer", "https://example.com/page"},
                               {"evil", "dropped"}});
  CHECK(download["url"] == "https://example.com/a.zip");
  CHECK(download["method"] == "GET");
  CHECK(download["fileSize"] == 1234);
  CHECK_FALSE(download.contains("evil"));
  CHECK_FALSE(download.contains("body")); // only for POST

  const json post = valid({{"type", "capture_download"},
                           {"url", "https://example.com/export"},
                           {"method", "POST"},
                           {"body", "a=1&b=2"}});
  CHECK(post["body"] == "a=1&b=2");

  const json media = valid({{"type", "capture_media"},
                            {"items", {{{"url", "https://cdn.example.com/v.m3u8"}, {"size", 5}}}},
                            {"pageUrl", "https://example.com/watch"}});
  CHECK(media["items"].size() == 1);
  CHECK(valid({{"type", "ping"}})["type"] == "ping");
  CHECK(valid({{"type", "video_page"}, {"pageUrl", "https://example.com/v"}})["type"] ==
        "video_page");

  CHECK(
      invalid({{"type", "capture_download"}, {"url", "file:///C:/Windows/win.ini"}}).find("url") !=
      std::string::npos);
  CHECK_FALSE(invalid({{"type", "capture_download"}, {"url", "javascript:alert(1)"}}).empty());
  CHECK(invalid({{"type", "capture_download"}}).find("missing") != std::string::npos);
  // Header injection through cookies or the user agent.
  CHECK(invalid({{"type", "capture_download"},
                 {"url", "https://e.com/a"},
                 {"cookies", "a=1\r\nX-Evil: 1"}})
            .find("control") != std::string::npos);
  CHECK_FALSE(invalid({{"type", "capture_download"}, {"url", "https://e.com/a"}, {"method", "PUT"}})
                  .empty());
  CHECK_FALSE(invalid({{"type", "capture_download"}, {"url", "https://e.com/a"}, {"fileSize", -1}})
                  .empty());
  CHECK_FALSE(invalid({{"type", "capture_media"}, {"items", json::array()}}).empty());
  CHECK(invalid({{"type", "run_this"}}).find("unknown") != std::string::npos);
  CHECK(invalid({{"type", "ping"}, {"v", 99}}).find("version") != std::string::npos);
  CHECK_FALSE(bridge::validate("not json"));
  CHECK_FALSE(bridge::validate("[1,2]"));
}

TEST_CASE("the bridge answers even when Lanes cannot be reached", "[bridge]") {
  const auto accepted = [](const std::string &) -> core::Result<std::string> {
    return std::string(R"({"result":"accepted"})");
  };
  const auto unreachable = [](const std::string &) -> core::Result<std::string> {
    return core::make_error(core::ErrorCode::NotFound, "no pipe");
  };
  CHECK(json::parse(bridge::handle(R"({"type":"ping"})", accepted))["result"] == "accepted");
  // The browser keeps the download when Lanes is not there.
  CHECK(json::parse(bridge::handle(R"({"type":"ping"})", unreachable))["result"] == "rejected");
  CHECK(json::parse(bridge::handle(R"({"type":"nope"})", accepted))["result"] == "error");
}

TEST_CASE("the bridge forwards to the Lanes window over the pipe", "[bridge]") {
  TempDir dir;
  const std::wstring pipe = L"lanes-bridge-test-" + std::to_wstring(::GetCurrentProcessId());
  ScopedEnv env(L"LANES_PIPE", pipe);
  auto engine = core::Engine::open({.database = dir.path() / "lanes.db"});
  REQUIRE(engine);
  core::service::EngineService service(**engine, {});
  std::string seen;
  service.set_on_browser([&seen](std::string message) -> core::Result<std::string> {
    seen = std::move(message);
    return std::string(R"({"result":"accepted","message":"ok"})");
  });
  auto server = core::platform::PipeServer::create(core::platform::default_pipe_name());
  REQUIRE(server);
  std::jthread serving(
      [&](std::stop_token stop) { core::service::serve(std::move(*server), service, stop); });

  const std::string message = *bridge::validate(
      R"({"type":"capture_download","url":"https://example.com/a.zip","cookies":"sid=1"})");
  // The GUI path is never used: a Lanes window already answers.
  auto reply = bridge::forward_to_lanes(message, dir.path() / "missing-lanes-gui.exe");
  REQUIRE(reply);
  CHECK(json::parse(*reply)["result"] == "accepted");
  CHECK(json::parse(seen)["cookies"] == "sid=1");
  serving.request_stop();
}

namespace {

// Runs lanes-bridge.exe like a browser does: framed messages on its stdin, replies on stdout.
std::string run_bridge(const std::string &input) {
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
  REQUIRE(::CreatePipe(&in_read, &in_write, &inherit, 0));
  REQUIRE(::CreatePipe(&out_read, &out_write, &inherit, 0));
  ::SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_read;
  si.hStdOutput = out_write;
  si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  std::wstring cmd =
      L"\"" + std::filesystem::path(LANES_BRIDGE_PATH).wstring() + L"\" chrome-extension://test/";
  REQUIRE(::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi));
  ::CloseHandle(in_read);
  ::CloseHandle(out_write);
  DWORD written = 0;
  ::WriteFile(in_write, input.data(), static_cast<DWORD>(input.size()), &written, nullptr);
  ::CloseHandle(in_write); // end of input: the bridge exits after answering
  std::string output;
  char buffer[4096];
  DWORD got = 0;
  while (::ReadFile(out_read, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    output.append(buffer, got);
  }
  ::CloseHandle(out_read);
  ::WaitForSingleObject(pi.hProcess, 10000);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return output;
}

} // namespace

TEST_CASE("lanes-bridge.exe answers the browser over stdin and stdout", "[bridge]") {
  TempDir dir;
  const std::wstring pipe = L"lanes-bridge-exe-test-" + std::to_wstring(::GetCurrentProcessId());
  ScopedEnv env(L"LANES_PIPE", pipe); // inherited by the bridge process
  auto engine = core::Engine::open({.database = dir.path() / "lanes.db"});
  REQUIRE(engine);
  core::service::EngineService service(**engine, {});
  service.set_on_browser([](std::string) -> core::Result<std::string> {
    return std::string(R"({"result":"accepted","message":"0.0.0"})");
  });
  auto server = core::platform::PipeServer::create(core::platform::default_pipe_name());
  REQUIRE(server);
  std::jthread serving(
      [&](std::stop_token stop) { core::service::serve(std::move(*server), service, stop); });

  std::istringstream replies(
      run_bridge(frame(R"({"v":1,"type":"ping"})") + frame(R"({"type":"format_c"})")));
  const json first = json::parse(*bridge::read_message(replies));
  CHECK(first["result"] == "accepted");
  CHECK(first["v"] == 1);
  CHECK(json::parse(*bridge::read_message(replies))["result"] == "error");
  CHECK(bridge::read_message(replies).error().code == core::ErrorCode::NotFound);
  serving.request_stop();
}

TEST_CASE("the native host registers for Chrome and Edge", "[bridge]") {
  TempDir dir;
  ScopedEnv env(L"LANES_REGISTRY_PREFIX", L"Software\\LanesTest");
  const auto exe = dir.path() / "lanes-bridge.exe";
  REQUIRE(bridge::register_host(exe, {"abcdefghijklmnopabcdefghijklmnop"}));
  const auto manifest_path = dir.path() / "com.lanes.bridge.json";
  for (const auto &key : bridge::host_registry_keys()) {
    CHECK(key.starts_with("Software\\LanesTest\\"));
    auto value = core::platform::user_default_value(key);
    REQUIRE(value);
    REQUIRE(value->has_value());
    CHECK(**value == manifest_path.string());
  }
  std::ifstream in(manifest_path);
  const json manifest = json::parse(in);
  in.close();
  CHECK(manifest["name"] == "com.lanes.bridge");
  CHECK(manifest["type"] == "stdio");
  CHECK(manifest["path"] == exe.string());
  CHECK(manifest["allowed_origins"][0] == "chrome-extension://abcdefghijklmnopabcdefghijklmnop/");

  REQUIRE(bridge::unregister_host(exe));
  for (const auto &key : bridge::host_registry_keys()) {
    CHECK_FALSE(core::platform::user_default_value(key)->has_value());
  }
  CHECK_FALSE(std::filesystem::exists(manifest_path));
  REQUIRE(core::platform::delete_user_key("Software\\LanesTest"));
}
