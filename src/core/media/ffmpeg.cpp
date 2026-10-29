// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/ffmpeg.hpp"

#include <chrono>
#include <string>
#include <system_error>

#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "core/platform/system.hpp"

namespace lanes::core::media {

namespace fs = std::filesystem;

Result<fs::path> find_ffmpeg() {
  std::error_code ec;
  if (const auto custom = platform::environment_variable("LANES_FFMPEG");
      custom && !custom->empty()) {
    const fs::path p = platform::path_from_utf8(*custom);
    if (fs::exists(p, ec)) {
      return p;
    }
  }
  const fs::path beside = platform::current_executable().parent_path() / "ffmpeg.exe";
  if (fs::exists(beside, ec)) {
    return beside;
  }
  return make_error(ErrorCode::NotFound,
                    "ffmpeg.exe was not found next to Lanes; it is needed to join video "
                    "streams (developers: run tools\\fetch-third-party.ps1)");
}

Result<void> mux(const std::vector<std::vector<fs::path>> &tracks, const fs::path &output,
                 std::stop_token stop) {
  auto ffmpeg = find_ffmpeg();
  if (!ffmpeg) {
    return make_error(ffmpeg.error());
  }
  std::vector<std::string> args = {"-nostdin", "-hide_banner", "-loglevel", "error", "-y"};
  std::vector<fs::path> lists;
  for (std::size_t t = 0; t < tracks.size(); ++t) {
    // One file per line; "file:" so a drive letter is never read as a protocol name.
    std::string list;
    for (const auto &f : tracks[t]) {
      std::string p = platform::path_to_utf8(f);
      for (char &c : p) {
        if (c == '\\') {
          c = '/';
        }
      }
      list += "file:" + p + "\n";
    }
    fs::path list_path = output;
    list_path += ".track" + std::to_string(t) + ".txt";
    if (auto ok = platform::write_file_atomic(list_path, list); !ok) {
      return ok;
    }
    lists.push_back(list_path);
    args.insert(args.end(), {"-protocol_whitelist", "file,concatf", "-i",
                             "concatf:" + platform::path_to_utf8(list_path)});
  }
  for (std::size_t t = 0; t < tracks.size(); ++t) {
    args.insert(args.end(), {"-map", std::to_string(t)});
  }
  // ffmpeg picks the muxer from the extension: keep it on the temporary name.
  fs::path temp = output.parent_path() /
                  platform::path_from_utf8(platform::path_to_utf8(output.stem()) + ".lanes-mux" +
                                           platform::path_to_utf8(output.extension()));
  args.insert(args.end(), {"-c", "copy", "-movflags", "+faststart", platform::path_to_utf8(temp)});

  platform::ProcessOptions options;
  options.executable = *ffmpeg;
  options.args = std::move(args);
  std::string errors;
  options.on_stderr_line = [&errors](std::string_view line) {
    if (errors.size() < 2000) {
      errors.append(line).push_back('\n');
    }
  };
  auto process = platform::Process::start(std::move(options));
  if (!process) {
    return make_error(process.error());
  }
  std::optional<int> exit_code;
  while (!(exit_code = process->wait(std::chrono::milliseconds(100)))) {
    if (stop.stop_requested()) {
      process->kill();
      process->wait();
      (void)platform::remove_file(temp);
      for (const auto &l : lists) {
        (void)platform::remove_file(l);
      }
      return make_error(ErrorCode::Cancelled, "download cancelled");
    }
  }
  for (const auto &l : lists) {
    (void)platform::remove_file(l);
  }
  if (*exit_code != 0) {
    (void)platform::remove_file(temp);
    while (!errors.empty() && errors.back() == '\n') {
      errors.pop_back();
    }
    return make_error(ErrorCode::Io,
                      "ffmpeg could not join the stream: " +
                          (errors.empty() ? "exit code " + std::to_string(*exit_code) : errors));
  }
  return platform::atomic_replace(temp, output);
}

} // namespace lanes::core::media
