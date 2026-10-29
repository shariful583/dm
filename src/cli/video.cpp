// SPDX-License-Identifier: GPL-3.0-or-later
// `lanes video`: video sites through yt-dlp.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <stop_token>
#include <system_error>

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "core/extract/updater.hpp"
#include "core/extract/video.hpp"
#include "core/extract/ytdlp.hpp"
#include "core/platform/console.hpp"
#include "core/platform/path.hpp"
#include "core/platform/power.hpp"
#include "core/platform/system.hpp"
#include "core/service/service.hpp"

namespace lanes::cli {

namespace {

namespace fs = std::filesystem;
using core::ErrorCode;
namespace extract = core::extract;

int fail(const core::Error &e) {
  std::cerr << "error: " << e.message << '\n';
  if (e.code == ErrorCode::AccessDenied) {
    std::cerr << "hint: pass the site's cookies with --cookie \"name=value; ...\" (from the "
                 "browser), or use the browser extension\n";
  } else if (e.code == ErrorCode::Unknown || e.code == ErrorCode::NotFound) {
    std::cerr << "hint: sites change often; try lanes video --update\n";
  }
  return e.code == ErrorCode::InvalidArgument ? kExitUsage : kExitError;
}

std::string codecs(const extract::Format &f) {
  std::string out;
  if (f.has_video()) {
    out = f.vcodec.substr(0, f.vcodec.find('.'));
  }
  if (f.has_audio()) {
    out += (out.empty() ? "" : "+") + f.acodec.substr(0, f.acodec.find('.'));
  }
  return out.empty() ? "?" : out;
}

std::string size_text(std::optional<std::uint64_t> size, bool estimate) {
  return size ? (estimate ? "~" : "") + format_bytes(*size) : "";
}

void print_formats(const extract::VideoInfo &info) {
  std::printf("%s\n", info.title.c_str());
  if (info.duration > 0) {
    std::printf("Duration: %s\n", format_duration(info.duration).c_str());
  }
  std::printf("\n%-10s %-5s %-11s %4s %-14s %10s  %-11s %s\n", "ID", "EXT", "RESOLUTION", "FPS",
              "CODECS", "SIZE", "PROTOCOL", "NOTE");
  for (const auto &f : info.formats) {
    const std::string resolution =
        f.has_video()
            ? (f.width > 0 ? std::to_string(f.width) + "x" : "") + std::to_string(f.height)
            : "audio only";
    std::string note = f.note;
    if (f.has_drm) {
      note += note.empty() ? "DRM" : ", DRM";
    }
    std::printf("%-10s %-5s %-11s %4s %-14s %10s  %-11s %s\n", f.id.c_str(), f.ext.c_str(),
                resolution.c_str(), f.fps > 0 ? std::to_string(std::lround(f.fps)).c_str() : "",
                codecs(f).c_str(), size_text(f.filesize, f.size_is_estimate).c_str(),
                f.protocol.c_str(), note.c_str());
  }
  std::printf("\nChoices for -f:\n");
  for (const auto &[choice, s] : extract::quality_choices(info)) {
    std::string ids = s.video != nullptr ? s.video->id : "";
    if (s.audio != nullptr) {
      ids += (ids.empty() ? "" : "+") + s.audio->id;
    }
    std::printf("  %-7s -> %-9s %-4s %s\n", choice.c_str(), ids.c_str(), s.container.c_str(),
                s.size ? format_bytes(*s.size).c_str() : "");
  }
  if (!info.subtitles.empty()) {
    std::string langs;
    for (const auto &[lang, files] : info.subtitles) {
      langs += (langs.empty() ? "" : ", ") + lang;
    }
    std::printf("\nSubtitles (--subs): %s\n", langs.c_str());
  }
}

// "1-3,7" → {1, 2, 3, 7} (1-based); empty → all.
core::Result<std::set<std::size_t>> parse_items(const std::string &text, std::size_t count) {
  std::set<std::size_t> out;
  if (text.empty()) {
    for (std::size_t i = 1; i <= count; ++i) {
      out.insert(i);
    }
    return out;
  }
  std::size_t pos = 0;
  while (pos < text.size()) {
    auto end = text.find(',', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string piece = text.substr(pos, end - pos);
    pos = end + 1;
    const auto dash = piece.find('-');
    try {
      const std::size_t a = std::stoul(piece.substr(0, dash));
      const std::size_t b = dash == std::string::npos ? a : std::stoul(piece.substr(dash + 1));
      for (std::size_t i = a; i <= b && i <= count; ++i) {
        out.insert(i);
      }
    } catch (const std::exception &) {
      return core::make_error(ErrorCode::InvalidArgument,
                              "bad --items '" + text + "' (e.g. 1-3,7)");
    }
  }
  return out;
}

std::vector<std::string> split_commas(const std::string &text) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos <= text.size() && !text.empty()) {
    auto end = text.find(',', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    if (end > pos) {
      out.push_back(text.substr(pos, end - pos));
    }
    pos = end + 1;
  }
  return out;
}

int run_update(bool rollback) {
  if (rollback) {
    if (auto ok = extract::rollback_ytdlp(); !ok) {
      return fail(ok.error());
    }
    std::cout << "Went back to the previous yt-dlp.\n";
    return kExitOk;
  }
  std::cerr << "Checking for a new yt-dlp...\n";
  auto r = extract::update_ytdlp({.force = true});
  if (!r) {
    return fail(r.error());
  }
  std::cout << (r->updated ? "Updated yt-dlp to " : "yt-dlp is up to date: ") << r->version << '\n';
  return kExitOk;
}

} // namespace

int run_video(const VideoArgs &args) {
  core::platform::use_utf8_console();
  if (args.update || args.rollback) {
    return run_update(args.rollback);
  }
  if (args.request.url.empty()) {
    std::cerr << "error: give the address of a video page\n";
    return kExitUsage;
  }
  auto request = build_request(args.request);
  if (!request) {
    return fail(request.error());
  }
  const extract::PageRequest page{request->url, request->cookies, request->user_agent,
                                  request->referer};
  const bool live = !args.quiet && core::platform::stderr_is_terminal();
  if (live) {
    std::cerr << "Reading the page..." << std::flush;
  }
  std::stop_source stop;
  core::platform::on_console_interrupt([&stop] { stop.request_stop(); });
  auto info = extract::extract({.page = page, .playlist = args.playlist}, stop.get_token());
  if (live) {
    std::cerr << '\r' << std::string(24, ' ') << '\r' << std::flush;
  }
  if (!info) {
    return fail(info.error());
  }

  extract::VideoRequest wanted;
  wanted.format = args.format;
  wanted.subtitles = split_commas(args.subs);
  wanted.thumbnail = args.thumbnail;

  // The pages to download: this one, or the chosen playlist entries.
  std::vector<std::string> pages;
  if (info->is_playlist) {
    if (args.list) {
      std::printf("Playlist: %s (%zu videos)\n", info->title.c_str(), info->entries.size());
      for (std::size_t i = 0; i < info->entries.size(); ++i) {
        const auto &e = info->entries[i];
        std::printf("%4zu  %-8s %s\n", i + 1,
                    e.duration > 0 ? format_duration(e.duration).c_str() : "",
                    e.title.empty() ? e.url.c_str() : e.title.c_str());
      }
      return kExitOk;
    }
    auto items = parse_items(args.items, info->entries.size());
    if (!items) {
      return fail(items.error());
    }
    for (const std::size_t i : *items) {
      pages.push_back(info->entries[i - 1].url);
    }
  } else {
    if (args.list) {
      print_formats(*info);
      return kExitOk;
    }
    pages.push_back(request->url);
  }

  // --add: the background engine downloads them (queues, restarts, the GUI's list).
  if (args.add) {
    core::service::ClientOptions co;
    co.engine_executable = core::platform::current_executable();
    auto client = core::service::EngineClient::connect(co);
    if (!client) {
      return fail(client.error());
    }
    for (const auto &url : pages) {
      core::NewDownload n;
      n.request = *request;
      n.request.url = url;
      n.video = extract::to_json(wanted);
      n.queue = args.queue;
      if (!args.output.empty()) {
        n.directory = core::platform::path_from_utf8(args.output);
      }
      n.if_duplicate = core::DuplicateAction::Rename;
      auto id = client->add(n);
      if (!id) {
        return fail(id.error());
      }
      std::cout << "Added #" << *id << ": " << url << '\n';
    }
    return kExitOk;
  }

  core::engine::DownloadOptions options;
  options.request = *request;
  options.connections = args.connections;
  if (!args.limit.empty()) {
    auto limit = parse_size(args.limit);
    if (!limit) {
      return fail(limit.error());
    }
    options.speed_limit = *limit;
  }
  std::error_code ec;
  options.directory =
      args.output.empty() ? fs::current_path(ec) : core::platform::path_from_utf8(args.output);
  options.if_file_exists = core::engine::ExistingFile::Rename;
  std::string shown;
  options.on_target = [&shown](const fs::path &p) {
    shown = core::platform::path_to_utf8(p.filename());
  };
  if (live) {
    options.on_progress = [&](const core::engine::DownloadProgress &p) {
      std::cerr << '\r'
                << progress_line(p, shown.empty() ? "..." : shown,
                                 core::platform::terminal_columns())
                << std::flush;
    };
  }
  int exit_code = kExitOk;
  for (const auto &url : pages) {
    options.request.url = url;
    shown.clear();
    const extract::VideoInfo *known = info->is_playlist ? nullptr : &*info;
    auto result = extract::download_video(options, wanted, stop.get_token(), known);
    if (live) {
      std::cerr << '\r'
                << std::string(static_cast<std::size_t>(
                                   std::max(core::platform::terminal_columns() - 1, 1)),
                               ' ')
                << '\r' << std::flush;
    }
    if (!result) {
      if (result.error().code == ErrorCode::Cancelled) {
        std::cerr << "Paused. Run the same command again to resume.\n";
        return kExitInterrupted;
      }
      std::cerr << url << ":\n";
      exit_code = fail(result.error());
      continue; // the other playlist entries still get their turn
    }
    if (auto checked = core::platform::mark_and_scan(result->path, url, request->referer);
        !checked && checked.error().code == ErrorCode::AccessDenied) {
      exit_code = fail(checked.error());
      continue;
    }
    if (!args.quiet) {
      std::cout << "Saved " << core::platform::path_to_utf8(result->path) << " ("
                << format_bytes(result->size) << ")\n";
    }
  }
  return exit_code;
}

} // namespace lanes::cli
