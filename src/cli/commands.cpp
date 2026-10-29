// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli/commands.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <system_error>

#include "cli/format.hpp"
#include "core/engine/download.hpp"
#include "core/media/stream.hpp"
#include "core/net/probe.hpp"
#include "core/platform/console.hpp"
#include "core/platform/path.hpp"
#include "core/verify/checksum.hpp"

namespace lanes::cli {

namespace {

namespace fs = std::filesystem;
using core::ErrorCode;

core::net::Credentials split_credentials(const std::string &text) {
  const auto colon = text.find(':');
  if (colon == std::string::npos) {
    return {text, ""};
  }
  return {text.substr(0, colon), text.substr(colon + 1)};
}

void print_error(const core::Error &error) {
  std::cerr << "error: " << error.message << '\n';
  switch (error.code) {
  case ErrorCode::LinkExpired:
    std::cerr << "hint: get a fresh link and run: lanes get <new-url> -o <same file>\n";
    break;
  case ErrorCode::RemoteChanged:
    std::cerr << "hint: add --restart to download the new version from the beginning\n";
    break;
  case ErrorCode::NoSpace:
  case ErrorCode::FileTooLarge:
    std::cerr << "hint: choose another folder with -o\n";
    break;
  default:
    break;
  }
}

// Clears the live progress line.
void clear_line(int columns) {
  std::cerr << '\r' << std::string(static_cast<std::size_t>(std::max(columns - 1, 1)), ' ') << '\r'
            << std::flush;
}

} // namespace

core::Result<core::net::RequestOptions> build_request(const RequestArgs &args) {
  core::net::RequestOptions r;
  r.url = args.url;
  if (auto valid = core::net::validate_url(r.url); !valid) {
    return core::make_error(valid.error());
  }
  for (const auto &header : args.headers) {
    const auto colon = header.find(':');
    if (colon == std::string::npos || colon == 0) {
      return core::make_error(ErrorCode::InvalidArgument,
                              "header must look like \"Name: value\": " + header);
    }
    std::string value = header.substr(colon + 1);
    value.erase(0, value.find_first_not_of(' '));
    r.headers.emplace_back(header.substr(0, colon), value);
  }
  r.cookies = args.cookie;
  r.referer = args.referer;
  r.user_agent = args.user_agent;
  if (!args.user.empty()) {
    r.auth = split_credentials(args.user);
  }
  r.proxy.url = args.proxy;
  if (!args.proxy_user.empty()) {
    r.proxy.credentials = split_credentials(args.proxy_user);
  }
  if (!args.data.empty()) {
    r.method = "POST";
    r.body = args.data;
  }
  return r;
}

int run_get(const GetArgs &args) {
  core::platform::use_utf8_console();

  core::engine::DownloadOptions options;
  auto request = build_request(args.request);
  if (!request) {
    std::cerr << "error: " << request.error().message << '\n';
    return kExitUsage;
  }
  options.request = std::move(*request);
  options.connections = args.connections;
  options.resume = !args.no_resume;
  options.restart_if_changed = args.restart;
  if (!args.limit.empty()) {
    auto limit = parse_size(args.limit);
    if (!limit) {
      std::cerr << "error: " << limit.error().message << '\n';
      return kExitUsage;
    }
    options.speed_limit = *limit;
  }
  if (!args.sha256.empty() || !args.md5.empty()) {
    auto checksum =
        args.sha256.empty()
            ? core::verify::make_checksum(core::verify::HashAlgorithm::Md5, args.md5)
            : core::verify::make_checksum(core::verify::HashAlgorithm::Sha256, args.sha256);
    if (!checksum) {
      std::cerr << "error: " << checksum.error().message << '\n';
      return kExitUsage;
    }
    options.checksum = *checksum;
  }

  // -o: an existing folder or one ending in a separator receives the server's name.
  std::error_code ec;
  if (args.output.empty()) {
    options.directory = fs::current_path(ec);
  } else {
    const fs::path out = core::platform::path_from_utf8(args.output);
    const char last = args.output.back();
    if (last == '/' || last == '\\' || fs::is_directory(out, ec)) {
      options.directory = out;
    } else {
      options.directory = out.has_parent_path() ? out.parent_path() : fs::current_path(ec);
      options.filename = core::platform::path_to_utf8(out.filename());
    }
  }
  // An existing file: overwrite one the user named with -o; keep it and pick a free name when
  // the name came from the server.
  if (args.if_exists == "overwrite") {
    options.if_file_exists = core::engine::ExistingFile::Overwrite;
  } else if (args.if_exists == "rename") {
    options.if_file_exists = core::engine::ExistingFile::Rename;
  } else if (args.if_exists == "fail") {
    options.if_file_exists = core::engine::ExistingFile::Fail;
  } else {
    options.if_file_exists = options.filename.empty() ? core::engine::ExistingFile::Rename
                                                      : core::engine::ExistingFile::Overwrite;
  }

  if (args.max_height > 0) {
    options.max_video_height = args.max_height;
  }
  const bool live = !args.quiet && core::platform::stderr_is_terminal();
  std::string shown_name = options.filename;
  // The real name is known once the server answered (Content-Disposition, stream name, ...).
  options.on_target = [&shown_name](const fs::path &path) {
    shown_name = core::platform::path_to_utf8(path.filename());
  };
  if (live) {
    options.on_progress = [&](const core::engine::DownloadProgress &p) {
      std::cerr << '\r'
                << progress_line(p, shown_name.empty() ? "..." : shown_name,
                                 core::platform::terminal_columns())
                << std::flush;
    };
  }

  std::stop_source stop;
  core::platform::on_console_interrupt([&stop] { stop.request_stop(); });

  const auto started = std::chrono::steady_clock::now();
  // Files and HLS/DASH streams alike.
  auto result = core::media::download_any(options, stop.get_token());
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  if (live) {
    clear_line(core::platform::terminal_columns());
  }

  if (!result) {
    if (result.error().code == ErrorCode::Cancelled) {
      std::cerr << "Paused. Run the same command again to resume.\n";
      return kExitInterrupted;
    }
    print_error(result.error());
    return kExitError;
  }
  if (!args.quiet) {
    const std::uint64_t fetched = result->size - result->resumed_from;
    std::cout << "Saved " << core::platform::path_to_utf8(result->path) << " ("
              << format_bytes(result->size) << ") in " << format_duration(seconds) << ", "
              << format_speed(seconds > 0 ? static_cast<double>(fetched) / seconds : 0) << ", "
              << result->connections << (result->connections == 1 ? " connection" : " connections");
    if (result->resumed_from > 0) {
      std::cout << ", resumed from " << format_bytes(result->resumed_from);
    }
    if (options.checksum) {
      std::cout << ", " << core::verify::to_string(options.checksum->algorithm) << " OK";
    }
    std::cout << '\n';
  }
  return kExitOk;
}

int run_info(const RequestArgs &args) {
  core::platform::use_utf8_console();
  auto request = build_request(args);
  if (!request) {
    std::cerr << "error: " << request.error().message << '\n';
    return kExitUsage;
  }
  if (request->method != "GET") {
    std::cerr << "error: info works only for GET downloads (no --data)\n";
    return kExitUsage;
  }
  auto info = core::net::probe(*request);
  if (!info) {
    print_error(info.error());
    return kExitError;
  }
  if (core::media::looks_like_stream(info->final_url, info->content_type)) {
    auto stream = core::media::inspect_stream(*request);
    if (!stream) {
      print_error(stream.error());
      return kExitError;
    }
    std::cout << "Stream:        "
              << (stream->kind == core::media::StreamKind::Hls ? "HLS" : "DASH") << '\n'
              << "File name:     " << stream->filename << '\n';
    for (const auto &c : stream->choices) {
      std::cout << "  " << c.kind;
      if (c.height > 0) {
        std::cout << "  " << c.width << "x" << c.height;
      }
      if (c.bandwidth > 0) {
        std::cout << "  " << format_speed(static_cast<double>(c.bandwidth) / 8);
      }
      if (!c.codecs.empty()) {
        std::cout << "  " << c.codecs;
      }
      if (!c.language.empty()) {
        std::cout << "  [" << c.language << "]";
      }
      std::cout << '\n';
    }
    return kExitOk;
  }
  std::cout << "File name:     " << info->filename << '\n';
  if (info->size) {
    std::cout << "Size:          " << format_bytes(*info->size) << " (" << *info->size
              << " bytes)\n";
  } else {
    std::cout << "Size:          unknown\n";
  }
  std::cout << "Resumable:     "
            << (info->accepts_ranges && info->size ? "yes (parallel connections possible)"
                                                   : "no (single connection, restarts on failure)")
            << '\n';
  if (!info->content_type.empty()) {
    std::cout << "Type:          " << info->content_type << '\n';
  }
  if (!info->last_modified.empty()) {
    std::cout << "Last modified: " << info->last_modified << '\n';
  }
  if (!info->etag.empty()) {
    std::cout << "ETag:          " << info->etag << '\n';
  }
  if (info->final_url != request->url) {
    std::cout << "Redirected:    yes\n"; // the final URL may carry tokens: not printed
  }
  return kExitOk;
}

} // namespace lanes::cli
