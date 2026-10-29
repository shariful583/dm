// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/store/store.hpp"

#include <nlohmann/json.hpp>

#include "core/platform/path.hpp"

namespace lanes::core::store {

namespace {

// 1: initial. 2: downloads.overwrite.
constexpr int kSchemaVersion = 2;

constexpr const char *kSchema = R"sql(
CREATE TABLE IF NOT EXISTS downloads(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  url TEXT NOT NULL,
  method TEXT NOT NULL DEFAULT 'GET',
  referer TEXT NOT NULL DEFAULT '',
  user_agent TEXT NOT NULL DEFAULT '',
  headers TEXT NOT NULL DEFAULT '[]',
  proxy TEXT NOT NULL DEFAULT '',
  directory TEXT NOT NULL,
  filename TEXT NOT NULL DEFAULT '',
  queue TEXT NOT NULL DEFAULT '',
  state TEXT NOT NULL,
  start_now INTEGER NOT NULL DEFAULT 0,
  start_at INTEGER,
  size INTEGER,
  downloaded INTEGER NOT NULL DEFAULT 0,
  connections INTEGER NOT NULL DEFAULT 0,
  speed_limit INTEGER NOT NULL DEFAULT 0,
  checksum TEXT NOT NULL DEFAULT '',
  error TEXT NOT NULL DEFAULT '',
  added_at INTEGER NOT NULL,
  completed_at INTEGER,
  overwrite INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS queues(
  name TEXT PRIMARY KEY,
  max_active INTEGER NOT NULL,
  enabled INTEGER NOT NULL,
  start_time TEXT,
  stop_time TEXT,
  days INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS settings(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL);
)sql";

constexpr const char *kColumns =
    "url, method, referer, user_agent, headers, proxy, directory, filename, queue, state, "
    "start_now, start_at, size, downloaded, connections, speed_limit, checksum, error, added_at, "
    "completed_at, overwrite";

std::string headers_json(const std::vector<std::pair<std::string, std::string>> &headers) {
  nlohmann::json j = nlohmann::json::array();
  for (const auto &[name, value] : headers) {
    j.push_back({name, value});
  }
  return j.dump();
}

std::vector<std::pair<std::string, std::string>> parse_headers(const std::string &text) {
  std::vector<std::pair<std::string, std::string>> out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_array()) {
    for (const auto &h : j) {
      if (h.is_array() && h.size() == 2 && h[0].is_string() && h[1].is_string()) {
        out.emplace_back(h[0].get<std::string>(), h[1].get<std::string>());
      }
    }
  }
  return out;
}

std::optional<std::int64_t> to_optional_int(const std::optional<std::uint64_t> &v) {
  return v ? std::optional<std::int64_t>(static_cast<std::int64_t>(*v)) : std::nullopt;
}

// Binds the record's columns as parameters 1..21, in kColumns order.
void bind_record(Statement &s, const DownloadRecord &r) {
  s.bind(1, r.url)
      .bind(2, r.method)
      .bind(3, r.referer)
      .bind(4, r.user_agent)
      .bind(5, headers_json(r.headers))
      .bind(6, r.proxy)
      .bind(7, platform::path_to_utf8(r.directory))
      .bind(8, r.filename)
      .bind(9, r.queue)
      .bind(10, to_string(r.state))
      .bind(11, std::int64_t{r.start_now ? 1 : 0})
      .bind(12, r.start_at)
      .bind(13, to_optional_int(r.size))
      .bind(14, static_cast<std::int64_t>(r.downloaded))
      .bind(15, std::int64_t{r.connections})
      .bind(16, static_cast<std::int64_t>(r.speed_limit))
      .bind(17, r.checksum)
      .bind(18, r.error)
      .bind(19, r.added_at)
      .bind(20, r.completed_at)
      .bind(21, std::int64_t{r.overwrite ? 1 : 0});
}

std::optional<queue::TimeOfDay> time_column(const Statement &s, int column) {
  const std::string text = s.text(column);
  if (text.empty()) {
    return std::nullopt;
  }
  auto t = queue::parse_time_of_day(text);
  return t ? std::optional(*t) : std::nullopt;
}

} // namespace

Result<Store> Store::open(const std::filesystem::path &path) {
  auto db = Database::open(path);
  if (!db) {
    return make_error(db.error());
  }
  // WAL + NORMAL: durable enough for a download list (sidecars hold the byte progress) and far
  // fewer disk flushes than the default.
  for (const char *pragma :
       {"PRAGMA journal_mode=WAL;", "PRAGMA synchronous=NORMAL;", "PRAGMA foreign_keys=ON;"}) {
    if (auto ok = db->exec(pragma); !ok) {
      return make_error(ok.error());
    }
  }
  auto version_query = db->prepare("PRAGMA user_version;");
  if (!version_query) {
    return make_error(version_query.error());
  }
  auto has_row = version_query->step();
  if (!has_row) {
    return make_error(has_row.error());
  }
  const std::int64_t version = *has_row ? version_query->int64(0) : 0;
  version_query = Statement(); // finalize before changing the schema
  if (version > kSchemaVersion) {
    return make_error(ErrorCode::Unsupported,
                      "the download list was written by a newer Lanes; update Lanes");
  }
  // A new database gets the current schema; older ones are migrated step by step.
  if (auto ok = db->exec(kSchema); !ok) {
    return make_error(ok.error());
  }
  if (version == 1) {
    if (auto ok =
            db->exec("ALTER TABLE downloads ADD COLUMN overwrite INTEGER NOT NULL DEFAULT 0;");
        !ok) {
      return make_error(ok.error());
    }
  }
  if (auto ok = db->exec("PRAGMA user_version=" + std::to_string(kSchemaVersion) + ";"); !ok) {
    return make_error(ok.error());
  }
  Store store;
  store.db_ = std::move(*db);
  return store;
}

Result<DownloadId> Store::insert(DownloadRecord record) {
  auto s = db_.prepare(std::string("INSERT INTO downloads(") + kColumns +
                       ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,"
                       "?19,?20,?21)");
  if (!s) {
    return make_error(s.error());
  }
  bind_record(*s, record);
  if (auto ok = s->run(); !ok) {
    return make_error(ok.error());
  }
  return db_.last_insert_id();
}

Result<void> Store::update(const DownloadRecord &r) {
  auto s = db_.prepare(
      "UPDATE downloads SET url=?1, method=?2, referer=?3, user_agent=?4, headers=?5, proxy=?6, "
      "directory=?7, filename=?8, queue=?9, state=?10, start_now=?11, start_at=?12, size=?13, "
      "downloaded=?14, connections=?15, speed_limit=?16, checksum=?17, error=?18, added_at=?19, "
      "completed_at=?20, overwrite=?21 WHERE id=?22");
  if (!s) {
    return make_error(s.error());
  }
  bind_record(*s, r);
  s->bind(22, r.id);
  return s->run();
}

Result<void> Store::remove(DownloadId id) {
  auto s = db_.prepare("DELETE FROM downloads WHERE id=?1");
  if (!s) {
    return make_error(s.error());
  }
  s->bind(1, id);
  return s->run();
}

Result<std::vector<DownloadRecord>> Store::downloads() {
  auto s = db_.prepare(std::string("SELECT id, ") + kColumns + " FROM downloads ORDER BY id");
  if (!s) {
    return make_error(s.error());
  }
  std::vector<DownloadRecord> out;
  for (;;) {
    auto row = s->step();
    if (!row) {
      return make_error(row.error());
    }
    if (!*row) {
      break;
    }
    DownloadRecord r;
    r.id = s->int64(0);
    r.url = s->text(1);
    r.method = s->text(2);
    r.referer = s->text(3);
    r.user_agent = s->text(4);
    r.headers = parse_headers(s->text(5));
    r.proxy = s->text(6);
    r.directory = platform::path_from_utf8(s->text(7));
    r.filename = s->text(8);
    r.queue = s->text(9);
    r.state = parse_download_state(s->text(10)).value_or(DownloadState::Paused);
    r.start_now = s->int64(11) != 0;
    r.start_at = s->optional_int64(12);
    if (const auto size = s->optional_int64(13)) {
      r.size = static_cast<std::uint64_t>(*size);
    }
    r.downloaded = static_cast<std::uint64_t>(s->int64(14));
    r.connections = static_cast<int>(s->int64(15));
    r.speed_limit = static_cast<std::uint64_t>(s->int64(16));
    r.checksum = s->text(17);
    r.error = s->text(18);
    r.added_at = s->int64(19);
    r.completed_at = s->optional_int64(20);
    r.overwrite = s->int64(21) != 0;
    out.push_back(std::move(r));
  }
  return out;
}

Result<std::vector<queue::QueueSettings>> Store::queues() {
  auto s = db_.prepare(
      "SELECT name, max_active, enabled, start_time, stop_time, days FROM queues ORDER BY name");
  if (!s) {
    return make_error(s.error());
  }
  std::vector<queue::QueueSettings> out;
  for (;;) {
    auto row = s->step();
    if (!row) {
      return make_error(row.error());
    }
    if (!*row) {
      break;
    }
    queue::QueueSettings q;
    q.name = s->text(0);
    q.max_active = static_cast<int>(s->int64(1));
    q.enabled = s->int64(2) != 0;
    q.start_time = time_column(*s, 3);
    q.stop_time = time_column(*s, 4);
    q.days = static_cast<std::uint8_t>(s->int64(5));
    out.push_back(std::move(q));
  }
  return out;
}

Result<void> Store::save_queue(const queue::QueueSettings &q) {
  auto s = db_.prepare("INSERT INTO queues(name, max_active, enabled, start_time, stop_time, days) "
                       "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(name) DO UPDATE SET "
                       "max_active=?2, enabled=?3, start_time=?4, stop_time=?5, days=?6");
  if (!s) {
    return make_error(s.error());
  }
  s->bind(1, q.name)
      .bind(2, std::int64_t{q.max_active})
      .bind(3, std::int64_t{q.enabled ? 1 : 0})
      .bind(4, q.start_time ? queue::to_string(*q.start_time) : std::string())
      .bind(5, q.stop_time ? queue::to_string(*q.stop_time) : std::string())
      .bind(6, std::int64_t{q.days});
  return s->run();
}

Result<std::vector<std::pair<std::string, std::string>>> Store::settings() {
  auto s = db_.prepare("SELECT key, value FROM settings ORDER BY key");
  if (!s) {
    return make_error(s.error());
  }
  std::vector<std::pair<std::string, std::string>> out;
  for (;;) {
    auto row = s->step();
    if (!row) {
      return make_error(row.error());
    }
    if (!*row) {
      break;
    }
    out.emplace_back(s->text(0), s->text(1));
  }
  return out;
}

Result<void> Store::set_setting(const std::string &key, const std::string &value) {
  auto s = db_.prepare("INSERT INTO settings(key, value) VALUES(?1, ?2) "
                       "ON CONFLICT(key) DO UPDATE SET value=?2");
  if (!s) {
    return make_error(s.error());
  }
  s->bind(1, key).bind(2, value);
  return s->run();
}

} // namespace lanes::core::store
