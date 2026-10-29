// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/engine/engine.hpp"
#include "core/error.hpp"
#include "core/platform/ipc.hpp"

namespace lanes::core::service {

// The engine process and its front-ends (CLI, browser bridge) talk JSON over the local pipe
// {"v":1,"cmd":"add",...} → {"ok":true,...} or
// {"ok":false,"error":{"code":"not_found","message":"..."}}.
inline constexpr int kProtocolVersion = 1;

// Answers requests with an engine. Thread-safe (the engine is).
class EngineService {
public:
  EngineService(Engine &engine, std::function<void()> request_shutdown);
  // The GUI, which owns the engine, shows its window on "activate" (a second launch forwards its
  // command-line arguments, e.g. URLs). Without a handler "activate" answers Unsupported.
  // Set before serving.
  void set_on_activate(std::function<void(std::vector<std::string> args)> handler);
  // The GUI answers messages from the browser extension, which the bridge has validated
  // ("browser" requests): message JSON in, reply JSON out. Without a handler
  // "browser" answers Unsupported. Called on the client's thread. Set before serving.
  void set_on_browser(std::function<Result<std::string>(std::string message)> handler);
  // Never throws; malformed requests get an error response.
  std::string handle(std::string_view request);

private:
  Engine &engine_;
  std::function<void()> request_shutdown_;
  std::function<void(std::vector<std::string>)> on_activate_;
  std::function<Result<std::string>(std::string)> on_browser_;
};

// Serves `service` until `stop`, one thread per connected client; calls `on_request` for each
// request (activity tracking). Blocking.
void serve(platform::PipeServer server, EngineService &service, std::stop_token stop,
           std::function<void()> on_request = {});

// Runs a background engine for the current user (`lanes engine`): owns the pipe, opens the
// database in platform::app_data_dir(), serves clients, and exits after `idle_timeout` with
// nothing to do (or on a "shutdown" request). Returns 0 also when another engine already runs.
int run_engine_host(std::chrono::seconds idle_timeout);

struct ClientOptions {
  // Program started in the background when no engine is running (e.g. lanes.exe "engine").
  std::filesystem::path engine_executable;
  std::vector<std::string> engine_args{"engine"};
  bool start_engine = true;
  std::chrono::milliseconds start_timeout{10'000};
};

// Front-end side of the protocol. Not thread-safe: one request at a time.
class EngineClient {
public:
  static Result<EngineClient> connect(const ClientOptions &options);

  Result<std::uint32_t> ping(); // the engine's process id
  Result<DownloadId> add(const NewDownload &download);
  Result<std::vector<DownloadInfo>> list();
  Result<void> pause(std::optional<DownloadId> id); // nullopt = all
  Result<void> resume(std::optional<DownloadId> id, // nullopt = all
                      std::optional<std::string> new_url = std::nullopt);
  Result<void> remove(DownloadId id, bool delete_files);
  Result<std::vector<queue::QueueSettings>> queues();
  Result<void> start_queue(const std::string &name, std::optional<int> max_active = std::nullopt);
  Result<void> stop_queue(const std::string &name);
  Result<std::vector<std::pair<std::string, std::string>>> settings();
  Result<void> set_setting(const std::string &key, const std::string &value);
  Result<std::vector<category::Category>> categories();
  Result<void> set_categories(const std::vector<category::Category> &categories);
  Result<void> shutdown_engine();
  // Asks the GUI owning the engine to show its window and handle `args`. Unsupported when the
  // owner is the windowless `lanes engine`.
  Result<void> activate(const std::vector<std::string> &args);
  // Passes a validated browser message (JSON) to the GUI; returns its reply (JSON). Unsupported
  // when the owner is the windowless `lanes engine`.
  Result<std::string> browser(const std::string &message);

  // Raw request/response (JSON text), for tools and tests.
  Result<std::string> call(const std::string &request);

private:
  platform::PipeConnection connection_;
};

} // namespace lanes::core::service
