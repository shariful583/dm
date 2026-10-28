// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::platform {

struct ProcessOptions {
  // Full path of the program. Never searched on PATH and never run through a shell.
  std::filesystem::path executable;
  // argv[1..], UTF-8. Each element reaches the child as exactly one argument, whatever it
  // contains (spaces, quotes, backslashes, shell metacharacters).
  std::vector<std::string> args;
  std::filesystem::path working_directory; // empty = inherit
  // Variables to add or replace (UTF-8) on top of the inherited environment.
  std::vector<std::pair<std::string, std::string>> env;
  // Called with each output line, without the line ending, on a reader thread. A final line
  // without a newline is delivered at end of output. May be empty (output is then discarded).
  std::function<void(std::string_view line)> on_stdout_line;
  std::function<void(std::string_view line)> on_stderr_line;
};

// A child process with stdin connected to the null device and stdout/stderr read by background
// threads. On Windows the child and everything it starts run in a job object, so kill() and the
// destructor end the whole process tree (e.g. yt-dlp and the ffmpeg it launched).
//
// Thread safety: kill() and pid() may be called from any thread, concurrently with wait().
// Other members must not be called concurrently. Output callbacks run on reader threads.
class Process {
public:
  static Result<Process> start(ProcessOptions options);

  Process();
  ~Process(); // kills the process tree if still running, then joins the reader threads
  Process(Process &&) noexcept;
  Process &operator=(Process &&) noexcept;
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;

  // Waits for the process to exit; returns its exit code, or nullopt on timeout. When it returns
  // an exit code, all output callbacks have finished. Any processes it left running are killed.
  std::optional<int> wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt);

  // Terminates the process and every process it started. Safe to call more than once.
  void kill();

  std::uint32_t pid() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct CapturedOutput {
  int exit_code = 0;
  std::string out; // stdout lines joined with '\n'
  std::string err; // stderr lines joined with '\n'
};

// Runs `options` to completion, capturing stdout and stderr (any line callbacks in `options` are
// replaced). On timeout the process tree is killed and a Timeout error returned. Thread-safe.
Result<CapturedOutput> run_capture(ProcessOptions options, std::chrono::milliseconds timeout);

// The command line CreateProcessW receives for `executable` + `args`, quoted so the child's
// CommandLineToArgvW / C runtime parses back exactly `args`. Exposed for tests. UTF-8.
std::string windows_command_line(const std::filesystem::path &executable,
                                 const std::vector<std::string> &args);

} // namespace lanes::core::platform
