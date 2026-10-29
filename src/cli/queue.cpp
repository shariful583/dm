// SPDX-License-Identifier: GPL-3.0-or-later
// Commands that work through the background engine: add, list, pause, resume, remove, run,
// config, engine.
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <system_error>
#include <thread>

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "core/platform/console.hpp"
#include "core/platform/path.hpp"
#include "core/platform/system.hpp"
#include "core/queue/schedule.hpp"
#include "core/service/service.hpp"

namespace lanes::cli {

namespace {

namespace fs = std::filesystem;
using core::DownloadState;
using core::ErrorCode;
using core::service::EngineClient;

core::Result<EngineClient> connect_engine(bool start = true) {
  core::service::ClientOptions options;
  options.engine_executable = core::platform::current_executable();
  options.start_engine = start;
  return EngineClient::connect(options);
}

int report(const core::Error &error) {
  std::cerr << "error: " << error.message << '\n';
  return error.code == ErrorCode::InvalidArgument ? kExitUsage : kExitError;
}

std::optional<core::DownloadId> parse_id(const std::string &text) {
  core::DownloadId id = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
  if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size() || id <= 0) {
    return std::nullopt;
  }
  return id;
}

std::string percent(const core::DownloadInfo &d) {
  if (!d.size || *d.size == 0) {
    return d.state == DownloadState::Completed ? "100.0%" : "   ?  ";
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%5.1f%%",
                100.0 * static_cast<double>(std::min(d.downloaded, *d.size)) /
                    static_cast<double>(*d.size));
  return buf;
}

std::string pad(std::string text, std::size_t width) {
  const std::size_t w = display_width(text);
  if (w < width) {
    text.append(width - w, ' ');
  }
  return text;
}

std::string state_label(const core::DownloadInfo &d) {
  std::string label(core::to_string(d.state));
  if (d.state == DownloadState::Scheduled && d.start_at) {
    label += " " + core::queue::format_local_datetime(*d.start_at);
  } else if (d.state == DownloadState::Queued && !d.queue.empty() && !d.start_now) {
    label += " [" + d.queue + "]";
  }
  return label;
}

} // namespace

int run_add(const AddArgs &args) {
  core::NewDownload n;
  auto request = build_request(args.request);
  if (!request) {
    std::cerr << "error: " << request.error().message << '\n';
    return kExitUsage;
  }
  n.request = std::move(*request);
  // The engine runs in another process with another working folder: send absolute paths.
  std::error_code ec;
  if (!args.output.empty()) {
    const fs::path out = fs::absolute(core::platform::path_from_utf8(args.output), ec);
    const char last = args.output.back();
    if (last == '/' || last == '\\' || fs::is_directory(out, ec)) {
      n.directory = out;
    } else {
      n.directory = out.parent_path();
      n.filename = core::platform::path_to_utf8(out.filename());
    }
  }
  n.queue = args.queue;
  n.connections = args.connections;
  if (!args.at.empty()) {
    auto at = core::queue::parse_local_datetime(args.at);
    if (!at) {
      std::cerr << "error: " << at.error().message << '\n';
      return kExitUsage;
    }
    n.start_at = *at;
  }
  if (!args.limit.empty()) {
    auto limit = parse_size(args.limit);
    if (!limit) {
      std::cerr << "error: " << limit.error().message << '\n';
      return kExitUsage;
    }
    n.speed_limit = *limit;
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
    n.checksum = *checksum;
  }

  auto client = connect_engine();
  if (!client) {
    return report(client.error());
  }
  auto id = client->add(n);
  if (!id) {
    return report(id.error());
  }
  std::cout << "Added #" << *id;
  if (n.start_at) {
    std::cout << ", starts at " << core::queue::format_local_datetime(*n.start_at);
  } else if (!n.queue.empty()) {
    std::cout << " to queue '" << n.queue << "' (start it with: lanes run --queue " << n.queue
              << ")";
  } else {
    std::cout << ", downloading in the background (see: lanes list)";
  }
  std::cout << '\n';
  return kExitOk;
}

int run_list(const std::string &queue) {
  core::platform::use_utf8_console();
  auto client = connect_engine();
  if (!client) {
    return report(client.error());
  }
  auto downloads = client->list();
  if (!downloads) {
    return report(downloads.error());
  }
  std::size_t shown = 0;
  for (const auto &d : *downloads) {
    if (!queue.empty() && d.queue != queue) {
      continue;
    }
    if (shown++ == 0) {
      std::cout << "  ID  STATE                 DONE    SIZE         SPEED        NAME\n";
    }
    char id[16];
    std::snprintf(id, sizeof(id), "%4lld", static_cast<long long>(d.id));
    std::cout << id << "  " << pad(state_label(d), 20) << "  " << percent(d) << "  "
              << pad(d.size ? format_bytes(*d.size) : "?", 11) << "  "
              << pad(d.state == DownloadState::Running ? format_speed(d.bytes_per_second) : "", 11)
              << "  " << d.filename << '\n';
    if (!d.error.empty()) {
      std::cout << "      " << d.error << '\n';
    }
  }
  if (shown == 0) {
    std::cout << "No downloads" << (queue.empty() ? "" : " in queue '" + queue + "'") << ".\n";
  }
  return kExitOk;
}

int run_pause(const std::string &target) {
  const auto id = parse_id(target);
  if (!id && target != "all") {
    std::cerr << "error: expected a download id or \"all\"\n";
    return kExitUsage;
  }
  auto client = connect_engine();
  if (!client) {
    return report(client.error());
  }
  if (auto ok = client->pause(id); !ok) {
    return report(ok.error());
  }
  return kExitOk;
}

int run_resume(const std::string &target, const std::string &url) {
  const auto id = parse_id(target);
  if (!id && target != "all") {
    std::cerr << "error: expected a download id or \"all\"\n";
    return kExitUsage;
  }
  if (!id && !url.empty()) {
    std::cerr << "error: --url needs a single download id\n";
    return kExitUsage;
  }
  auto client = connect_engine();
  if (!client) {
    return report(client.error());
  }
  if (auto ok = client->resume(id, url.empty() ? std::nullopt : std::optional(url)); !ok) {
    return report(ok.error());
  }
  return kExitOk;
}

int run_remove(const std::string &target, bool delete_file) {
  const auto id = parse_id(target);
  if (!id) {
    std::cerr << "error: expected a download id\n";
    return kExitUsage;
  }
  auto client = connect_engine();
  if (!client) {
    return report(client.error());
  }
  if (auto ok = client->remove(*id, delete_file); !ok) {
    return report(ok.error());
  }
  return kExitOk;
}

int run_queue(const RunArgs &args) {
  core::platform::use_utf8_console();
  std::optional<std::chrono::local_seconds> until;
  if (!args.until.empty()) {
    auto t = core::queue::parse_time_of_day(args.until);
    if (!t) {
      std::cerr << "error: " << t.error().message << '\n';
      return kExitUsage;
    }
    // The next occurrence of that time of day.
    const auto now = core::queue::local_now();
    auto at = std::chrono::floor<std::chrono::days>(now) + std::chrono::hours(t->hour) +
              std::chrono::minutes(t->minute);
    if (at <= now) {
      at += std::chrono::days(1);
    }
    until = at;
  }
  auto client = connect_engine();
  if (!client) {
    return report(client.error());
  }
  if (auto ok =
          client->start_queue(args.queue, args.max > 0 ? std::optional(args.max) : std::nullopt);
      !ok) {
    return report(ok.error());
  }

  std::stop_source stop;
  core::platform::on_console_interrupt([&stop] { stop.request_stop(); });
  const bool live = core::platform::stderr_is_terminal();
  int done = 0;
  int failed = 0;
  for (;;) {
    auto downloads = client->list();
    if (!downloads) {
      return report(downloads.error());
    }
    int running = 0;
    int waiting = 0;
    done = 0;
    failed = 0;
    double speed = 0;
    for (const auto &d : *downloads) {
      if (d.queue != args.queue) {
        continue;
      }
      switch (d.state) {
      case DownloadState::Running:
        ++running;
        speed += d.bytes_per_second;
        break;
      case DownloadState::Queued:
      case DownloadState::Scheduled:
        ++waiting;
        break;
      case DownloadState::Completed:
        ++done;
        break;
      case DownloadState::Failed:
      case DownloadState::NeedsAddress:
        ++failed;
        break;
      default:
        break;
      }
    }
    if (live) {
      std::cerr << "\rqueue " << args.queue << ": " << running << " running, " << waiting
                << " waiting, " << done << " done, " << failed << " failed  " << format_speed(speed)
                << "      " << std::flush;
    }
    const bool finished = running == 0 && waiting == 0;
    const bool interrupted = stop.stop_requested();
    const bool out_of_time = until && core::queue::local_now() >= *until;
    if (finished || interrupted || out_of_time) {
      (void)client->stop_queue(args.queue); // later additions wait for the next run
      if (live) {
        std::cerr << '\n';
      }
      if (interrupted) {
        std::cerr << "Queue stopped. Run the same command to continue.\n";
        return kExitInterrupted;
      }
      if (out_of_time && !finished) {
        std::cout << "Queue '" << args.queue << "' stopped at " << args.until << " with "
                  << running + waiting << " download(s) left.\n";
        return kExitOk;
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  std::cout << "Queue '" << args.queue << "' finished: " << done << " done, " << failed
            << " failed.\n";
  return failed == 0 ? kExitOk : kExitError;
}

int run_config(const std::vector<std::string> &args) {
  auto client = connect_engine();
  if (!client) {
    return report(client.error());
  }
  if (args.size() >= 2) {
    std::string value = args[1];
    if (args[0] == "speed_limit") {
      auto bytes = parse_size(value); // accept "2M" as well as plain bytes
      if (!bytes) {
        std::cerr << "error: " << bytes.error().message << '\n';
        return kExitUsage;
      }
      value = std::to_string(*bytes);
    } else if (args[0] == "download_dir") {
      std::error_code ec;
      value = core::platform::path_to_utf8(fs::absolute(core::platform::path_from_utf8(value), ec));
    }
    if (auto ok = client->set_setting(args[0], value); !ok) {
      return report(ok.error());
    }
    return kExitOk;
  }
  auto settings = client->settings();
  if (!settings) {
    return report(settings.error());
  }
  bool found = args.empty();
  for (const auto &[key, value] : *settings) {
    if (args.empty()) {
      std::cout << key << " = " << value << '\n';
    } else if (key == args[0]) {
      std::cout << value << '\n';
      found = true;
    }
  }
  if (!found) {
    std::cerr << "error: unknown setting '" << args[0] << "'\n";
    return kExitUsage;
  }
  return kExitOk;
}

int run_engine(bool stop) {
  if (stop) {
    auto client = connect_engine(false);
    if (!client) {
      std::cout << "No Lanes engine is running.\n";
      return kExitOk;
    }
    if (auto ok = client->shutdown_engine(); !ok) {
      return report(ok.error());
    }
    std::cout << "Lanes engine stopping.\n";
    return kExitOk;
  }
  int idle_seconds = 60;
  if (const auto custom = core::platform::environment_variable("LANES_ENGINE_IDLE_SECONDS")) {
    std::from_chars(custom->data(), custom->data() + custom->size(), idle_seconds);
  }
  return core::service::run_engine_host(std::chrono::seconds(idle_seconds));
}

} // namespace lanes::cli
