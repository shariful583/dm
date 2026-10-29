// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/category/category.hpp"
#include "core/engine/download.hpp"
#include "core/engine/download_state.hpp"
#include "core/error.hpp"
#include "core/net/request.hpp"
#include "core/queue/schedule.hpp"
#include "core/verify/checksum.hpp"

namespace lanes::core {

// What add() does when a download with the same URL is already in the list.
enum class DuplicateAction {
  Ask,       // AlreadyExists error naming the existing download (the front-end asks the user)
  Rename,    // add anyway, saving as "name (2).ext"
  Overwrite, // remove the existing one (and its files), then add
  Resume,    // resume the existing one instead; add() returns its id
};

// A download to add. Secrets in `request` (cookies, credentials, POST body, Cookie /
// Authorization headers, proxy login) are kept in memory only and lost when the engine restarts.
struct NewDownload {
  net::RequestOptions request;
  std::filesystem::path directory;      // empty = the download_dir setting
  std::string filename;                 // empty = the server's name
  std::string queue;                    // empty = start now; otherwise wait for that queue
  std::optional<std::int64_t> start_at; // unix seconds: not before then
  int connections = 0;                  // 0 = the connections setting
  std::uint64_t speed_limit = 0;        // bytes/s, 0 = none (the global limit still applies)
  std::optional<verify::Checksum> checksum;
  DuplicateAction if_duplicate = DuplicateAction::Ask;
  // A file of the same name already on disk: replace it, or (default) save under a free name.
  bool overwrite_file = false;
};

// Snapshot of one download for front-ends.
struct DownloadInfo {
  DownloadId id = 0;
  std::string url;
  std::string referer;  // the page the download came from (for refreshing an expired address)
  std::string filename; // the server's / given name; a guess from the URL until known
  std::filesystem::path directory; // empty until known when the file's category picks it
  std::string category;            // "Video", ..., or "General"
  std::string queue;
  DownloadState state = DownloadState::Queued;
  bool start_now = false;
  std::optional<std::int64_t> start_at;
  std::optional<std::uint64_t> size;
  std::uint64_t downloaded = 0;
  double bytes_per_second = 0;
  std::string error;
  int connections = 0;
  std::uint64_t speed_limit = 0;
  std::int64_t added_at = 0;
  std::optional<std::int64_t> completed_at;
  std::vector<engine::SegmentProgress> segments; // live while running
};

struct EngineEvent {
  enum class Kind { Added, Changed, Progress, Removed };
  Kind kind = Kind::Changed;
  DownloadId id = 0;
};

struct EngineConfig {
  std::filesystem::path database;      // created if missing
  std::chrono::milliseconds tick{250}; // scheduler interval
  std::chrono::milliseconds progress_interval{500};
};

// The download manager (front-ends talk to core only through this and its events).
// Owns the database, the queues and scheduler, the global speed limit, and one worker thread per
// running download. Settings: download_dir, connections (8), max_active (4, all downloads),
// speed_limit (0 = none, bytes/s), category_folders (1: downloads without a folder go to their
// category's folder), system_proxy (1: use the Windows proxy settings when none is given). File
// categories are edited with categories() / set_categories().
//
// Thread safety: every member may be called from any thread. Event callbacks run on engine or
// download threads, without engine locks held; they must not block for long.
class Engine {
public:
  static Result<std::unique_ptr<Engine>> open(EngineConfig config);
  ~Engine(); // shutdown()
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  Result<DownloadId> add(NewDownload download);
  // Pausing keeps the partial file; resuming starts at once (ignoring queue gating). A new `url`
  // on resume replaces an expired address (IDM's "Refresh download address").
  Result<void> pause(DownloadId id);
  Result<void> resume(DownloadId id, std::optional<std::string> new_url = std::nullopt);
  // Caps one download (bytes/s, 0 = no own cap; the global limit still applies). Takes effect at
  // once, also while it runs.
  Result<void> set_speed_limit(DownloadId id, std::uint64_t bytes_per_second);
  Result<void> remove(DownloadId id, bool delete_files);
  void pause_all();
  void resume_all();

  std::vector<DownloadInfo> list() const;
  std::optional<DownloadInfo> get(DownloadId id) const;
  std::optional<DownloadId> find_by_url(const std::string &url) const;

  std::vector<category::Category> categories() const;
  Result<void> set_categories(std::vector<category::Category> categories);

  std::vector<queue::QueueSettings> queues() const;
  Result<void> save_queue(const queue::QueueSettings &queue); // create or change
  Result<void> start_queue(const std::string &name);
  // Stops a queue: its running downloads go back to waiting (not paused).
  Result<void> stop_queue(const std::string &name);

  std::vector<std::pair<std::string, std::string>> settings() const;
  Result<void> set_setting(const std::string &key, const std::string &value);

  // True when nothing is running or waiting to start on its own (queued for a started queue or
  // "start now", scheduled, or in a queue with a start time).
  bool idle() const;

  // Returns a subscription id for unsubscribe().
  int subscribe(std::function<void(const EngineEvent &)> callback);
  void unsubscribe(int subscription);

  // Stops all downloads (they resume on the next start) and the scheduler. Idempotent.
  void shutdown();

private:
  Engine();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lanes::core
