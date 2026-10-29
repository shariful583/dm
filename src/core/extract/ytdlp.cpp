// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/extract/ytdlp.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "core/media/ffmpeg.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "core/platform/system.hpp"

namespace lanes::core::extract {

namespace fs = std::filesystem;

namespace {

// A Netscape cookie file for one yt-dlp run, removed when it goes out of scope. It lives in the
// per-user app data folder, which only this user (and administrators) can read.
class CookieFile {
public:
  static Result<CookieFile> create(const PageRequest &page) {
    CookieFile f;
    if (page.cookies.empty()) {
      return f;
    }
    static std::atomic<int> counter{0};
    const fs::path dir = platform::app_data_dir() / "tmp";
    if (auto ok = platform::create_directories(dir); !ok) {
      return make_error(ok.error());
    }
    f.path_ = dir / ("cookies-" + std::to_string(platform::current_process_id()) + "-" +
                     std::to_string(++counter) + ".txt");
    if (auto ok = platform::write_file_atomic(f.path_, netscape_cookies(page.cookies, page.url));
        !ok) {
      return make_error(ok.error());
    }
    return f;
  }
  CookieFile() = default;
  CookieFile(CookieFile &&other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }
  CookieFile &operator=(CookieFile &&) = delete;
  CookieFile(const CookieFile &) = delete;
  ~CookieFile() {
    if (!path_.empty()) {
      (void)platform::remove_file(path_);
    }
  }
  const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

std::vector<std::string> common_args(const PageRequest &page, const CookieFile &cookies) {
  // --ignore-config: the user's own yt-dlp configuration must not change what Lanes gets.
  std::vector<std::string> args = {"--ignore-config", "--no-colors", "--no-progress"};
  if (auto deno = find_js_runtime()) {
    args.insert(args.end(), {"--js-runtimes", "deno:" + platform::path_to_utf8(*deno)});
  }
  if (auto ffmpeg = media::find_ffmpeg()) {
    args.insert(args.end(), {"--ffmpeg-location", platform::path_to_utf8(*ffmpeg)});
  }
  if (!cookies.path().empty()) {
    args.insert(args.end(), {"--cookies", platform::path_to_utf8(cookies.path())});
  }
  if (!page.user_agent.empty()) {
    args.insert(args.end(), {"--add-headers", "User-Agent:" + page.user_agent});
  }
  if (!page.referer.empty()) {
    args.insert(args.end(), {"--add-headers", "Referer:" + page.referer});
  }
  return args;
}

struct ToolRun {
  int exit_code = 0;
  std::string errors; // "ERROR:" lines, without the prefix
};

// Runs yt-dlp; stdout lines go to `on_line` (on a reader thread). Cancelled on `stop`.
Result<ToolRun> run_tool(std::vector<std::string> args,
                         std::function<void(std::string_view)> on_line, std::stop_token stop,
                         std::chrono::milliseconds timeout) {
  auto exe = find_ytdlp();
  if (!exe) {
    return make_error(exe.error());
  }
  platform::ProcessOptions options;
  options.executable = *exe;
  options.args = std::move(args);
  auto handler = std::make_shared<std::function<void(std::string_view)>>(std::move(on_line));
  options.on_stdout_line = [handler](std::string_view line) { (*handler)(line); };
  auto errors = std::make_shared<std::string>();
  auto mutex = std::make_shared<std::mutex>();
  options.on_stderr_line = [errors, mutex, handler](std::string_view line) {
    if (line.find("LANES|") != std::string_view::npos) {
      (*handler)(line); // progress lines may come on either stream
      return;
    }
    constexpr std::string_view prefix = "ERROR: ";
    if (line.substr(0, prefix.size()) == prefix) {
      std::lock_guard lock(*mutex);
      if (errors->size() < 2000) {
        *errors += (errors->empty() ? "" : "\n") + std::string(line.substr(prefix.size()));
      }
    }
  };
  auto process = platform::Process::start(std::move(options));
  if (!process) {
    return make_error(process.error());
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::optional<int> exit_code;
  while (!(exit_code = process->wait(std::chrono::milliseconds(100)))) {
    if (stop.stop_requested()) {
      process->kill();
      process->wait();
      return make_error(ErrorCode::Cancelled, "cancelled");
    }
    if (std::chrono::steady_clock::now() > deadline) {
      process->kill();
      process->wait();
      return make_error(ErrorCode::Timeout, "yt-dlp did not finish in time");
    }
  }
  std::lock_guard lock(*mutex);
  return ToolRun{*exit_code, *errors};
}

Error tool_error(const ToolRun &run, std::string_view what) {
  std::string message = run.errors.empty() ? std::string(what) + " failed (yt-dlp exit code " +
                                                 std::to_string(run.exit_code) + ")"
                                           : run.errors;
  ErrorCode code = ErrorCode::Unknown;
  if (message.find("Unsupported URL") != std::string::npos) {
    code = ErrorCode::Unsupported;
  } else if (message.find("DRM") != std::string::npos) {
    code = ErrorCode::Unsupported;
  } else if (message.find("Sign in") != std::string::npos ||
             message.find("login") != std::string::npos ||
             message.find("Private video") != std::string::npos) {
    code = ErrorCode::AccessDenied;
  }
  return Error{code, "yt-dlp: " + message, 0};
}

} // namespace

fs::path tools_dir() { return platform::app_data_dir() / "bin"; }

Result<fs::path> find_ytdlp() {
  std::error_code ec;
  if (const auto custom = platform::environment_variable("LANES_YTDLP");
      custom && !custom->empty()) {
    const fs::path p = platform::path_from_utf8(*custom);
    if (fs::exists(p, ec)) {
      return p;
    }
  }
  for (const fs::path &p :
       {tools_dir() / "yt-dlp.exe", platform::current_executable().parent_path() / "yt-dlp.exe"}) {
    if (fs::exists(p, ec)) {
      return p;
    }
  }
  return make_error(ErrorCode::NotFound,
                    "yt-dlp.exe was not found next to Lanes; it is needed for video sites "
                    "(developers: run tools\\fetch-third-party.ps1)");
}

std::optional<fs::path> find_js_runtime() {
  std::error_code ec;
  const fs::path deno = platform::current_executable().parent_path() / "deno.exe";
  return fs::exists(deno, ec) ? std::optional<fs::path>(deno) : std::nullopt;
}

Result<VideoInfo> extract(const ExtractOptions &options, std::stop_token stop) {
  auto cookies = CookieFile::create(options.page);
  if (!cookies) {
    return make_error(cookies.error());
  }
  std::vector<std::string> args = common_args(options.page, *cookies);
  // Playlists always come flat (entries are read one by one when downloaded). A video address
  // with a playlist attached ("watch?v=...&list=...") means the video unless `playlist`.
  args.insert(args.end(), {"-J", options.playlist ? "--yes-playlist" : "--no-playlist",
                           "--flat-playlist", "--", options.page.url});
  auto out = std::make_shared<std::string>();
  auto run = run_tool(
      std::move(args), [out](std::string_view line) { out->append(line).push_back('\n'); }, stop,
      options.timeout);
  if (!run) {
    return make_error(run.error());
  }
  if (run->exit_code != 0) {
    return make_error(tool_error(*run, "reading the video page"));
  }
  return parse_info(*out);
}

Result<void> download_with_ytdlp(const PageRequest &page, const std::string &format_ids,
                                 const fs::path &output,
                                 const std::function<void(const ToolProgress &)> &on_progress,
                                 std::stop_token stop) {
  auto cookies = CookieFile::create(page);
  if (!cookies) {
    return make_error(cookies.error());
  }
  std::vector<std::string> args = common_args(page, *cookies);
  args.erase(std::find(args.begin(), args.end(), "--no-progress")); // progress lines wanted
  args.insert(args.end(), {"--newline", "--no-playlist", "--no-mtime", "--progress-template",
                           std::string(kProgressTemplate), "-f", format_ids, "-o",
                           platform::path_to_utf8(output)});
  if (format_ids.find('+') != std::string::npos) {
    std::string ext = platform::path_to_utf8(output.extension());
    args.insert(args.end(), {"--merge-output-format", ext.empty() ? "mkv" : ext.substr(1)});
  }
  args.insert(args.end(), {"--", page.url});
  auto run = run_tool(
      std::move(args),
      [&on_progress](std::string_view line) {
        if (auto p = parse_progress_line(line); p && on_progress) {
          on_progress(*p);
        }
      },
      stop, std::chrono::hours(48));
  if (!run) {
    return make_error(run.error());
  }
  if (run->exit_code != 0) {
    return make_error(tool_error(*run, "downloading with yt-dlp"));
  }
  std::error_code ec;
  if (!fs::exists(platform::to_long_path(output), ec)) {
    return make_error(ErrorCode::Io, "yt-dlp finished but the file is missing");
  }
  return {};
}

Result<std::string> ytdlp_version(const fs::path &exe) {
  platform::ProcessOptions options;
  options.executable = exe;
  options.args = {"--version"};
  auto out = platform::run_capture(std::move(options), std::chrono::seconds(60));
  if (!out) {
    return make_error(out.error());
  }
  if (out->exit_code != 0) {
    return make_error(ErrorCode::Unknown, "yt-dlp --version failed");
  }
  std::string v = out->out;
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) {
    v.pop_back();
  }
  return v;
}

} // namespace lanes::core::extract
