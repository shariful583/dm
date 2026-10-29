// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/engine/download_state.hpp"
#include "core/error.hpp"
#include "core/queue/schedule.hpp"
#include "core/store/sqlite.hpp"

namespace lanes::core::store {

// A download as persisted. Holds no secrets: no cookies, credentials, POST bodies
// or Cookie/Authorization headers — those live only in the engine's memory.
struct DownloadRecord {
  DownloadId id = 0;
  std::string url;
  std::string method = "GET";
  std::string referer;
  std::string user_agent;
  std::vector<std::pair<std::string, std::string>> headers; // non-secret only
  std::string proxy;                                        // without credentials
  std::filesystem::path directory;
  std::string filename; // empty until known (given, or learned when the download starts)
  std::string queue;    // empty = not in a queue
  DownloadState state = DownloadState::Queued;
  bool start_now = false; // starts without waiting for its queue ("Download now" / resume)
  std::optional<std::int64_t> start_at; // unix seconds
  std::optional<std::uint64_t> size;
  std::uint64_t downloaded = 0;
  int connections = 0;           // 0 = engine default
  std::uint64_t speed_limit = 0; // bytes/s, 0 = none
  std::string checksum;          // "sha256:<hex>" / "md5:<hex>" / empty
  std::string error;
  std::int64_t added_at = 0;
  std::optional<std::int64_t> completed_at;
};

// The Lanes database: downloads, queues, settings. Uses WAL mode and NORMAL sync (few disk
// flushes). Not thread-safe: the engine serializes access.
class Store {
public:
  static Result<Store> open(const std::filesystem::path &path);

  Result<DownloadId> insert(DownloadRecord record); // sets id
  Result<void> update(const DownloadRecord &record);
  Result<void> remove(DownloadId id);
  Result<std::vector<DownloadRecord>> downloads();

  Result<std::vector<queue::QueueSettings>> queues();
  Result<void> save_queue(const queue::QueueSettings &queue);

  Result<std::vector<std::pair<std::string, std::string>>> settings();
  Result<void> set_setting(const std::string &key, const std::string &value);

private:
  Database db_;
};

} // namespace lanes::core::store
