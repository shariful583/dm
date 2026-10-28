// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "temp_dir.hpp"

using namespace lanes::core;
using namespace lanes::core::platform;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kChild = path_from_utf8(LANES_TEST_CHILD_PATH);

ProcessOptions child(std::vector<std::string> args) {
  ProcessOptions options;
  options.executable = kChild;
  options.args = std::move(args);
  return options;
}

std::string hex(const std::string &s) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const char c : s) {
    const auto b = static_cast<unsigned char>(c);
    out += kDigits[b >> 4];
    out += kDigits[b & 0x0F];
  }
  return out;
}

bool process_gone(DWORD pid, std::chrono::milliseconds timeout) {
  const HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (h == nullptr) {
    return true; // already exited and reaped
  }
  const bool exited =
      ::WaitForSingleObject(h, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
  ::CloseHandle(h);
  return exited;
}

} // namespace

TEST_CASE("arguments reach the child unchanged", "[platform][process]") {
  const std::vector<std::string> tricky = {
      "plain",
      "with space",
      "",
      "quote\"inside",
      "\"fully quoted\"",
      "trailing\\",
      "trailing space and backslash \\",
      "\\\\server\\share\\",
      "back\\\\\"slash-quote",
      "C:\\Program Files\\x\\",
      "& | < > ^ % ! ( ) ; , = `",
      "--output=%(title)s.%(ext)s",
      "r\xC3\xA9sum\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80",
      "line1\nline2\ttab",
  };
  std::vector<std::string> args = {"args"};
  args.insert(args.end(), tricky.begin(), tricky.end());

  const auto result = run_capture(child(args), 10s);
  REQUIRE(result);
  CHECK(result->exit_code == 0);
  std::string expected = "argc=" + std::to_string(tricky.size());
  for (const auto &a : tricky) {
    expected += "\n" + hex(a);
  }
  CHECK(result->out == expected);
}

TEST_CASE("windows_command_line quoting rules", "[platform][process]") {
  CHECK(windows_command_line(L"C:\\t\\a.exe", {"x", "a b", ""}) ==
        "\"C:\\t\\a.exe\" x \"a b\" \"\"");
  CHECK(windows_command_line(L"a.exe", {"a\\\"b"}) == "\"a.exe\" \"a\\\\\\\"b\"");
  CHECK(windows_command_line(L"a.exe", {"dir\\ x\\"}) == "\"a.exe\" \"dir\\ x\\\\\"");
  CHECK(windows_command_line(L"a.exe", {"no\\space\\"}) == "\"a.exe\" no\\space\\");
}

TEST_CASE("exit codes and stderr", "[platform][process]") {
  const auto exited = run_capture(child({"exit", "42"}), 10s);
  REQUIRE(exited);
  CHECK(exited->exit_code == 42);

  const auto err = run_capture(child({"stderr", "something failed"}), 10s);
  REQUIRE(err);
  CHECK(err->out.empty());
  CHECK(err->err == "something failed");

  const auto partial = run_capture(child({"partial"}), 10s);
  REQUIRE(partial);
  CHECK(partial->out == "no newline at end");
}

TEST_CASE("large output on both streams does not deadlock", "[platform][process]") {
  std::atomic<int> out_lines{0};
  std::atomic<int> err_lines{0};
  auto options = child({"spam", "100000"});
  options.on_stdout_line = [&](std::string_view line) {
    if (line.starts_with("line ")) {
      ++out_lines;
    }
  };
  options.on_stderr_line = [&](std::string_view) { ++err_lines; };
  auto process = Process::start(std::move(options));
  REQUIRE(process);
  CHECK(process->wait(30s) == 0);
  CHECK(out_lines == 100000);
  CHECK(err_lines == 100000);
}

TEST_CASE("environment overrides and working directory", "[platform][process]") {
  auto options = child({"env", "LANES_TEST_VALUE"});
  options.env = {{"LANES_TEST_VALUE", "d\xC3\xA9j\xC3\xA0 vu"}, {"Path", "C:\\nowhere"}};
  const auto env = run_capture(options, 10s);
  REQUIRE(env);
  CHECK(env->out == "d\xC3\xA9j\xC3\xA0 vu");

  TempDir dir;
  auto in_dir = child({"cwd"});
  in_dir.working_directory = dir.path();
  const auto cwd = run_capture(in_dir, 10s);
  REQUIRE(cwd);
  CHECK(std::filesystem::equivalent(path_from_utf8(cwd->out), dir.path()));
}

TEST_CASE("timeout kills the process", "[platform][process]") {
  const auto start = std::chrono::steady_clock::now();
  const auto result = run_capture(child({"sleep", "30000"}), 300ms);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == ErrorCode::Timeout);
  CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("kill ends the whole process tree", "[platform][process]") {
  std::atomic<DWORD> grandchild{0};
  auto options = child({"spawn-sleeper", "30000"});
  options.on_stdout_line = [&](std::string_view line) {
    grandchild = static_cast<DWORD>(std::stoul(std::string(line)));
  };
  auto process = Process::start(std::move(options));
  REQUIRE(process);

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (grandchild == 0 && std::chrono::steady_clock::now() < deadline) {
    Sleep(10);
  }
  REQUIRE(grandchild != 0);
  CHECK_FALSE(process_gone(grandchild, 0ms));

  process->kill();
  CHECK(process->wait(5s).has_value());
  CHECK(process_gone(grandchild, 5s));
}

TEST_CASE("missing executable is NotFound", "[platform][process]") {
  ProcessOptions options;
  options.executable = L"C:\\definitely\\not\\here.exe";
  const auto process = Process::start(options);
  REQUIRE_FALSE(process);
  CHECK(process.error().code == ErrorCode::NotFound);
}
