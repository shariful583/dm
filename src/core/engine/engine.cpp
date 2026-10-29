// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/engine.hpp"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "core/limiter/rate_limiter.hpp"
#include "core/media/stream.hpp"
#include "core/net/probe.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "core/platform/system.hpp"
#include "core/resume/sidecar.hpp"
#include "core/store/store.hpp"

namespace lanes::core {

namespace {

namespace fs = std::filesystem;

constexpr const char *kMainQueue = "main";
constexpr int kMaxConnections = 32;

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string lower(std::string s) {
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return s;
}

bool is_secret_header(const std::string &name) {
  const std::string n = lower(name);
  return n == "cookie" || n == "authorization" || n == "proxy-authorization";
}

std::optional<std::uint64_t> parse_uint(const std::string &text) {
  std::uint64_t v = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return v;
}

// What the database must not hold. Lost when the engine restarts.
struct Secrets {
  std::string cookies;
  std::optional<net::Credentials> auth;
  std::optional<std::string> body;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string proxy_url; // with embedded credentials, if any
  std::optional<net::Credentials> proxy_credentials;
};

void delete_download_files(const fs::path &final_path) {
  if (final_path.filename().empty()) {
    return;
  }
  fs::path part = final_path;
  part += ".part";
  (void)platform::remove_file(final_path);
  (void)platform::remove_file(part);
  (void)platform::remove_file(resume::sidecar_path(final_path));
}

} // namespace

struct Engine::Impl {
  struct Worker {
    std::jthread thread;
    bool finished = false;
    bool removed = false;
    bool delete_files = false;
    fs::path final_path;                             // for delete_files after a removal
    std::shared_ptr<limiter::RateLimiter> own_limit; // this download's cap, changeable live
  };
  struct Live {
    std::uint64_t downloaded = 0;
    double speed = 0;
    std::vector<engine::SegmentProgress> segments;
  };

  EngineConfig config;

  mutable std::mutex mutex; // guards everything below
  store::Store store;
  std::map<DownloadId, store::DownloadRecord> records;
  std::map<DownloadId, Secrets> secrets;
  std::map<DownloadId, Live> live;
  std::map<std::string, queue::QueueSettings> queues;
  std::map<std::string, std::string> settings;
  std::vector<category::Category> categories = category::default_categories();
  std::map<DownloadId, std::unique_ptr<Worker>> workers;
  std::vector<std::unique_ptr<Worker>> graveyard; // finished workers to join without the lock
  std::vector<EngineEvent> pending_events;
  bool wake_requested = false;
  bool shut_down = false;
  std::chrono::local_seconds last_tick_local{};

  std::shared_ptr<limiter::RateLimiter> global_limit = std::make_shared<limiter::RateLimiter>();
  std::condition_variable_any wake;

  std::mutex events_mutex;
  std::vector<std::pair<int, std::function<void(const EngineEvent &)>>> subscribers;
  int next_subscription = 1;

  std::jthread scheduler; // last: started after everything above exists

  // --- helpers; "locked" ones expect `mutex` held ---

  void emit_locked(EngineEvent::Kind kind, DownloadId id) { pending_events.push_back({kind, id}); }

  void publish() {
    std::vector<EngineEvent> events;
    {
      std::lock_guard lock(mutex);
      events.swap(pending_events);
    }
    if (events.empty()) {
      return;
    }
    std::lock_guard lock(events_mutex);
    for (const auto &e : events) {
      for (const auto &[id, callback] : subscribers) {
        callback(e);
      }
    }
  }

  void wake_scheduler_locked() {
    wake_requested = true;
    wake.notify_all();
  }

  void persist_locked(const store::DownloadRecord &r) { (void)store.update(r); }

  std::uint64_t setting_uint_locked(const std::string &key, std::uint64_t fallback) const {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      return fallback;
    }
    return parse_uint(it->second).value_or(fallback);
  }

  fs::path default_directory_locked() const {
    const auto it = settings.find("download_dir");
    if (it != settings.end() && !it->second.empty()) {
      return platform::path_from_utf8(it->second);
    }
    return platform::downloads_dir();
  }

  bool worker_active_locked(DownloadId id) const {
    const auto it = workers.find(id);
    return it != workers.end() && !it->second->finished;
  }

  bool category_folders_locked() const { return setting_uint_locked("category_folders", 1) != 0; }

  fs::path folder_for_locked(const std::string &filename, const fs::path &directory) const {
    if (!directory.empty()) {
      return directory;
    }
    return category_folders_locked()
               ? category::folder_for(categories, filename, default_directory_locked())
               : default_directory_locked();
  }

  // `base`, or "stem (2).ext", ...: not used by another download in the list nor taken on disk.
  std::string free_name_locked(const std::string &base, const fs::path &directory) const {
    const fs::path folder = folder_for_locked(base, directory);
    const auto used = [&](const std::string &name) {
      return std::any_of(records.begin(), records.end(),
                         [&](const auto &entry) { return entry.second.filename == name; });
    };
    std::string candidate = platform::unique_filename(folder, base);
    const auto dot = base.rfind('.');
    const std::string stem = dot != std::string::npos && dot > 0 ? base.substr(0, dot) : base;
    const std::string ext = dot != std::string::npos && dot > 0 ? base.substr(dot) : "";
    for (int k = 2; used(candidate) && k < 10000; ++k) {
      candidate = platform::unique_filename(folder, stem + " (" + std::to_string(k) + ")" + ext);
    }
    return candidate;
  }

  DownloadInfo info_locked(const store::DownloadRecord &r) const {
    DownloadInfo info;
    info.id = r.id;
    info.url = r.url;
    info.filename = r.filename.empty() ? net::suggest_filename("", r.url) : r.filename;
    info.directory = r.directory;
    info.category = category::category_name(categories, info.filename);
    info.queue = r.queue;
    info.state = r.state;
    info.start_now = r.start_now;
    info.start_at = r.start_at;
    info.size = r.size;
    info.downloaded = r.downloaded;
    info.error = r.error;
    info.connections = r.connections;
    info.speed_limit = r.speed_limit;
    info.referer = r.referer;
    info.added_at = r.added_at;
    info.completed_at = r.completed_at;
    if (const auto it = live.find(r.id); it != live.end()) {
      info.downloaded = it->second.downloaded;
      info.bytes_per_second = it->second.speed;
      info.segments = it->second.segments;
    }
    return info;
  }

  engine::DownloadOptions options_locked(const store::DownloadRecord &r) const {
    engine::DownloadOptions o;
    o.request.url = r.url;
    o.request.method = r.method;
    o.request.referer = r.referer;
    o.request.user_agent = r.user_agent;
    o.request.headers = r.headers;
    o.request.proxy.url = r.proxy;
    if (const auto it = secrets.find(r.id); it != secrets.end()) {
      const Secrets &s = it->second;
      o.request.cookies = s.cookies;
      o.request.auth = s.auth;
      o.request.body = s.body;
      o.request.headers.insert(o.request.headers.end(), s.headers.begin(), s.headers.end());
      if (!s.proxy_url.empty()) {
        o.request.proxy.url = s.proxy_url;
      }
      o.request.proxy.credentials = s.proxy_credentials;
    }
    o.directory = r.directory;
    if (o.directory.empty()) {
      // Decided once the real file name is known (Content-Disposition, redirects).
      const fs::path base = default_directory_locked();
      if (category_folders_locked()) {
        o.choose_directory = [cats = categories, base](const std::string &name) {
          return category::folder_for(cats, name, base);
        };
      } else {
        o.directory = base;
      }
    }
    o.filename = r.filename;
    o.if_file_exists = r.overwrite ? engine::ExistingFile::Overwrite : engine::ExistingFile::Rename;
    o.connections = static_cast<int>(
        std::clamp<std::uint64_t>(r.connections > 0 ? static_cast<std::uint64_t>(r.connections)
                                                    : setting_uint_locked("connections", 8),
                                  1, kMaxConnections));
    o.limiters = {global_limit}; // start_locked adds the download's own, changeable bucket
    o.request.use_system_proxy = setting_uint_locked("system_proxy", 1) != 0;
    if (const auto colon = r.checksum.find(':'); colon != std::string::npos) {
      const auto algorithm = r.checksum.substr(0, colon) == "md5" ? verify::HashAlgorithm::Md5
                                                                  : verify::HashAlgorithm::Sha256;
      if (auto c = verify::make_checksum(algorithm, r.checksum.substr(colon + 1))) {
        o.checksum = *c;
      }
    }
    o.progress_interval = config.progress_interval;
    return o;
  }

  void start_locked(store::DownloadRecord &r) {
    if (r.method == "POST" && !secrets.contains(r.id)) {
      r.state = DownloadState::Failed;
      r.error = "the POST data of this download was not kept after a restart; add it again";
      persist_locked(r);
      emit_locked(EngineEvent::Kind::Changed, r.id);
      return;
    }
    engine::DownloadOptions o = options_locked(r);
    const DownloadId id = r.id;
    o.on_progress = [this, id](const engine::DownloadProgress &p) { on_progress(id, p); };
    o.on_target = [this, id](const fs::path &path) { on_target(id, path); };

    r.state = DownloadState::Running;
    r.error.clear();
    persist_locked(r);
    live[id] = Live{r.downloaded, 0, {}};
    emit_locked(EngineEvent::Kind::Changed, id);

    auto worker = std::make_unique<Worker>();
    worker->own_limit = std::make_shared<limiter::RateLimiter>(r.speed_limit);
    o.limiters.push_back(worker->own_limit);
    worker->thread = std::jthread([this, id, o = std::move(o)](std::stop_token stop) {
      auto result = media::download_any(std::move(o), stop); // files and HLS/DASH streams
      on_finished(id, std::move(result));
    });
    workers[id] = std::move(worker);
  }

  // --- callbacks from download threads ---

  void on_progress(DownloadId id, const engine::DownloadProgress &p) {
    {
      std::lock_guard lock(mutex);
      auto &l = live[id];
      l.downloaded = p.downloaded;
      l.speed = p.bytes_per_second;
      l.segments = p.segments;
      if (const auto it = records.find(id); it != records.end() && p.total) {
        it->second.size = p.total;
      }
      emit_locked(EngineEvent::Kind::Progress, id);
    }
    publish();
  }

  // The download decided its file: remember it, so a resume uses the same file (also after a
  // rename to "name (2).ext" or a category folder).
  void on_target(DownloadId id, const fs::path &path) {
    std::lock_guard lock(mutex);
    if (const auto it = records.find(id); it != records.end()) {
      auto &r = it->second;
      const std::string name = platform::path_to_utf8(path.filename());
      if (r.filename != name || r.directory != path.parent_path()) {
        r.filename = name;
        r.directory = path.parent_path();
        persist_locked(r);
        emit_locked(EngineEvent::Kind::Changed, id);
      }
    }
  }

  void on_finished(DownloadId id, Result<engine::DownloadResult> result) {
    fs::path delete_path;
    {
      std::lock_guard lock(mutex);
      const auto wit = workers.find(id);
      if (wit == workers.end()) {
        return;
      }
      Worker &w = *wit->second;
      w.finished = true;
      if (w.removed) {
        if (w.delete_files) {
          delete_path = w.final_path; // the files are closed now
        }
      } else if (const auto rit = records.find(id); rit != records.end()) {
        auto &r = rit->second;
        if (const auto lit = live.find(id); lit != live.end()) {
          r.downloaded = lit->second.downloaded;
        }
        if (result) {
          r.state = DownloadState::Completed;
          r.size = result->size;
          r.downloaded = result->size;
          r.filename = platform::path_to_utf8(result->path.filename());
          r.directory = result->path.parent_path();
          r.completed_at = unix_now();
          r.error.clear();
        } else if (result.error().code == ErrorCode::Cancelled) {
          // Stopped on purpose: whoever stopped it already set the new state (paused, queued).
          if (r.state == DownloadState::Running) {
            r.state = DownloadState::Paused;
          }
        } else {
          r.state = result.error().code == ErrorCode::LinkExpired ? DownloadState::NeedsAddress
                                                                  : DownloadState::Failed;
          r.error = result.error().message;
        }
        persist_locked(r);
        emit_locked(EngineEvent::Kind::Changed, id);
      }
      live.erase(id);
      wake_scheduler_locked();
    }
    if (!delete_path.empty()) {
      delete_download_files(delete_path);
    }
    publish();
  }

  // --- scheduler ---

  void stop_queue_locked(queue::QueueSettings &q) {
    q.enabled = false;
    (void)store.save_queue(q);
    for (auto &[id, r] : records) {
      if (r.queue == q.name && !r.start_now && r.state == DownloadState::Running &&
          worker_active_locked(id)) {
        r.state = DownloadState::Queued; // back to waiting, not paused
        persist_locked(r);
        workers[id]->thread.request_stop();
        emit_locked(EngineEvent::Kind::Changed, id);
      }
    }
  }

  void tick_locked() {
    // Workers that ended: join them later, without the lock.
    for (auto it = workers.begin(); it != workers.end();) {
      if (it->second->finished) {
        graveyard.push_back(std::move(it->second));
        it = workers.erase(it);
      } else {
        ++it;
      }
    }

    // Queue start/stop times.
    const auto now_local = queue::local_now();
    if (last_tick_local.time_since_epoch().count() != 0) {
      for (auto &[name, q] : queues) {
        if (q.start_time && !q.enabled &&
            queue::crossed(*q.start_time, q.days, last_tick_local, now_local)) {
          q.enabled = true;
          (void)store.save_queue(q);
        }
        if (q.stop_time && q.enabled &&
            queue::crossed(*q.stop_time, q.days, last_tick_local, now_local)) {
          stop_queue_locked(q);
        }
      }
    }
    last_tick_local = now_local;

    // Start times.
    const std::int64_t now = unix_now();
    for (auto &[id, r] : records) {
      if (r.state == DownloadState::Scheduled && (!r.start_at || *r.start_at <= now)) {
        r.state = DownloadState::Queued;
        persist_locked(r);
        emit_locked(EngineEvent::Kind::Changed, id);
      } else if (r.state == DownloadState::Queued && r.start_at && *r.start_at > now) {
        r.state = DownloadState::Scheduled;
        persist_locked(r);
        emit_locked(EngineEvent::Kind::Changed, id);
      }
    }

    // Free slots: global max_active, and each queue's max_active.
    std::size_t running = 0;
    std::map<std::string, int> per_queue;
    for (const auto &[id, w] : workers) {
      if (!w->finished && !w->removed) {
        ++running;
        if (const auto it = records.find(id); it != records.end()) {
          ++per_queue[it->second.queue];
        }
      }
    }
    const auto max_active = static_cast<std::size_t>(setting_uint_locked("max_active", 4));
    for (auto &[id, r] : records) {
      if (running >= max_active) {
        break;
      }
      if (r.state != DownloadState::Queued || workers.contains(id)) {
        continue; // a previous run of it is still stopping
      }
      if (!r.start_now) {
        const auto q = queues.find(r.queue);
        if (q == queues.end() || !q->second.enabled || per_queue[r.queue] >= q->second.max_active) {
          continue;
        }
      }
      start_locked(r);
      ++running;
      ++per_queue[r.queue];
    }
  }

  void scheduler_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::vector<std::unique_ptr<Worker>> finished;
      {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, config.tick, [this] { return wake_requested; });
        wake_requested = false;
        if (stop.stop_requested() || shut_down) {
          break;
        }
        tick_locked();
        finished.swap(graveyard);
      }
      finished.clear(); // joins: their threads already ran on_finished
      publish();
    }
  }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() { shutdown(); }

Result<std::unique_ptr<Engine>> Engine::open(EngineConfig config) {
  if (config.database.has_parent_path()) {
    if (auto ok = platform::create_directories(config.database.parent_path()); !ok) {
      return make_error(ok.error());
    }
  }
  auto store = store::Store::open(config.database);
  if (!store) {
    return make_error(store.error());
  }
  std::unique_ptr<Engine> engine(new Engine());
  Impl &d = *engine->impl_;
  d.config = std::move(config);
  d.store = std::move(*store);

  auto settings = d.store.settings();
  auto queues = d.store.queues();
  auto downloads = d.store.downloads();
  if (!settings || !queues || !downloads) {
    return make_error(!settings ? settings.error()
                                : (!queues ? queues.error() : downloads.error()));
  }
  for (auto &[key, value] : *settings) {
    if (key == "categories") {
      if (auto parsed = category::from_json(value)) {
        d.categories = std::move(*parsed);
      }
    } else {
      d.settings[key] = value;
    }
  }
  for (auto &q : *queues) {
    d.queues[q.name] = q;
  }
  if (!d.queues.contains(kMainQueue)) {
    queue::QueueSettings main{.name = kMainQueue};
    (void)d.store.save_queue(main);
    d.queues[kMainQueue] = main;
  }
  for (auto &r : *downloads) {
    if (r.state == DownloadState::Running) {
      r.state = DownloadState::Queued; // the engine stopped mid-download: resume it
      r.start_now = true;
      (void)d.store.update(r);
    }
    d.records[r.id] = std::move(r);
  }
  d.global_limit->set_rate(d.setting_uint_locked("speed_limit", 0));
  d.scheduler = std::jthread([&d](std::stop_token stop) { d.scheduler_loop(stop); });
  return engine;
}

Result<DownloadId> Engine::add(NewDownload n) {
  if (auto valid = net::validate_url(n.request.url); !valid) {
    return make_error(valid.error());
  }
  if (n.connections < 0 || n.connections > kMaxConnections) {
    return make_error(ErrorCode::InvalidArgument, "connections must be between 1 and 32");
  }
  Impl &d = *impl_;

  // The same URL already in the list (duplicate detection).
  if (const auto existing = find_by_url(n.request.url)) {
    const auto info = get(*existing);
    switch (n.if_duplicate) {
    case DuplicateAction::Ask:
      return make_error(ErrorCode::AlreadyExists,
                        "already in the list as #" + std::to_string(*existing) + " (" +
                            std::string(to_string(info ? info->state : DownloadState::Queued)) +
                            ")");
    case DuplicateAction::Resume:
      if (info && info->state != DownloadState::Completed) {
        if (auto ok = resume(*existing); !ok) {
          return make_error(ok.error());
        }
      }
      return *existing;
    case DuplicateAction::Overwrite:
      if (auto ok = remove(*existing, true); !ok) {
        return make_error(ok.error());
      }
      n.overwrite_file = true;
      break;
    case DuplicateAction::Rename: {
      std::lock_guard lock(d.mutex);
      const std::string base = !n.filename.empty() ? platform::sanitize_filename(n.filename)
                               : info              ? info->filename
                                                   : net::suggest_filename("", n.request.url);
      n.filename = d.free_name_locked(base, n.directory);
      break;
    }
    }
  }

  DownloadId id = 0;
  {
    std::lock_guard lock(d.mutex);
    store::DownloadRecord r;
    r.url = n.request.url;
    r.method = n.request.method;
    r.referer = n.request.referer;
    r.user_agent = n.request.user_agent;
    Secrets s;
    for (auto &header : n.request.headers) {
      (is_secret_header(header.first) ? s.headers : r.headers).push_back(header);
    }
    s.cookies = n.request.cookies;
    s.auth = n.request.auth;
    s.body = n.request.body;
    s.proxy_credentials = n.request.proxy.credentials;
    if (!n.request.proxy.url.empty()) {
      r.proxy = net::strip_credentials(n.request.proxy.url);
      if (r.proxy != n.request.proxy.url) {
        s.proxy_url = n.request.proxy.url;
      }
    }
    r.directory = n.directory; // empty: the file's category decides when the download starts
    r.filename = n.filename.empty() ? std::string() : platform::sanitize_filename(n.filename);
    r.overwrite = n.overwrite_file;
    r.queue = n.queue;
    r.start_now = n.queue.empty();
    r.start_at = n.start_at;
    r.state =
        (n.start_at && *n.start_at > unix_now()) ? DownloadState::Scheduled : DownloadState::Queued;
    r.connections = n.connections;
    r.speed_limit = n.speed_limit;
    if (n.checksum) {
      r.checksum =
          std::string(n.checksum->algorithm == verify::HashAlgorithm::Md5 ? "md5:" : "sha256:") +
          n.checksum->hex;
    }
    r.added_at = unix_now();

    if (!r.queue.empty() && !d.queues.contains(r.queue)) {
      queue::QueueSettings q{.name = r.queue};
      if (auto ok = d.store.save_queue(q); !ok) {
        return make_error(ok.error());
      }
      d.queues[r.queue] = q;
    }
    auto inserted = d.store.insert(r);
    if (!inserted) {
      return make_error(inserted.error());
    }
    id = r.id = *inserted;
    const bool has_secrets = !s.cookies.empty() || s.auth || s.body || !s.headers.empty() ||
                             !s.proxy_url.empty() || s.proxy_credentials;
    if (has_secrets) {
      d.secrets[id] = std::move(s);
    }
    d.records[id] = std::move(r);
    d.emit_locked(EngineEvent::Kind::Added, id);
    d.wake_scheduler_locked();
  }
  d.publish();
  return id;
}

Result<void> Engine::pause(DownloadId id) {
  Impl &d = *impl_;
  {
    std::lock_guard lock(d.mutex);
    const auto it = d.records.find(id);
    if (it == d.records.end()) {
      return make_error(ErrorCode::NotFound, "no download #" + std::to_string(id));
    }
    auto &r = it->second;
    if (r.state == DownloadState::Completed) {
      return make_error(ErrorCode::InvalidArgument,
                        "download #" + std::to_string(id) + " is already complete");
    }
    if (r.state != DownloadState::Paused) {
      if (d.worker_active_locked(id)) {
        d.workers[id]->thread.request_stop();
      }
      r.state = DownloadState::Paused;
      d.persist_locked(r);
      d.emit_locked(EngineEvent::Kind::Changed, id);
    }
  }
  d.publish();
  return {};
}

Result<void> Engine::resume(DownloadId id, std::optional<std::string> new_url) {
  Impl &d = *impl_;
  if (new_url) {
    if (auto valid = net::validate_url(*new_url); !valid) {
      return make_error(valid.error());
    }
  }
  {
    std::lock_guard lock(d.mutex);
    const auto it = d.records.find(id);
    if (it == d.records.end()) {
      return make_error(ErrorCode::NotFound, "no download #" + std::to_string(id));
    }
    auto &r = it->second;
    if (r.state == DownloadState::Completed) {
      return make_error(ErrorCode::InvalidArgument,
                        "download #" + std::to_string(id) + " is already complete");
    }
    if (r.state == DownloadState::Running && d.worker_active_locked(id)) {
      return {};
    }
    if (new_url) {
      r.url = *new_url;
    }
    r.state = DownloadState::Queued;
    r.start_now = true;
    r.start_at.reset();
    r.error.clear();
    d.persist_locked(r);
    d.emit_locked(EngineEvent::Kind::Changed, id);
    d.wake_scheduler_locked();
  }
  d.publish();
  return {};
}

Result<void> Engine::set_speed_limit(DownloadId id, std::uint64_t bytes_per_second) {
  Impl &d = *impl_;
  {
    std::lock_guard lock(d.mutex);
    const auto it = d.records.find(id);
    if (it == d.records.end()) {
      return make_error(ErrorCode::NotFound, "no download #" + std::to_string(id));
    }
    auto &r = it->second;
    r.speed_limit = bytes_per_second;
    d.persist_locked(r);
    if (const auto w = d.workers.find(id); w != d.workers.end() && w->second->own_limit) {
      w->second->own_limit->set_rate(bytes_per_second);
    }
    d.emit_locked(EngineEvent::Kind::Changed, id);
  }
  d.publish();
  return {};
}

Result<void> Engine::remove(DownloadId id, bool delete_files) {
  Impl &d = *impl_;
  fs::path delete_now;
  {
    std::lock_guard lock(d.mutex);
    const auto it = d.records.find(id);
    if (it == d.records.end()) {
      return make_error(ErrorCode::NotFound, "no download #" + std::to_string(id));
    }
    const auto &r = it->second;
    // No folder yet means it never started: there are no files to delete.
    const fs::path final_path = r.filename.empty() || r.directory.empty()
                                    ? fs::path()
                                    : r.directory / platform::path_from_utf8(r.filename);
    if (d.worker_active_locked(id)) {
      auto &w = *d.workers[id];
      w.removed = true;
      w.delete_files = delete_files;
      w.final_path = final_path;
      w.thread.request_stop(); // files are deleted once the download has closed them
    } else if (delete_files) {
      delete_now = final_path;
    }
    if (auto ok = d.store.remove(id); !ok) {
      return ok;
    }
    d.records.erase(it);
    d.secrets.erase(id);
    d.live.erase(id);
    d.emit_locked(EngineEvent::Kind::Removed, id);
    d.wake_scheduler_locked();
  }
  if (!delete_now.empty()) {
    delete_download_files(delete_now);
  }
  d.publish();
  return {};
}

void Engine::pause_all() {
  for (const auto &info : list()) {
    if (info.state != DownloadState::Completed && info.state != DownloadState::Paused) {
      (void)pause(info.id);
    }
  }
}

void Engine::resume_all() {
  for (const auto &info : list()) {
    if (info.state == DownloadState::Paused || info.state == DownloadState::Failed ||
        info.state == DownloadState::NeedsAddress) {
      (void)resume(info.id);
    }
  }
}

std::vector<DownloadInfo> Engine::list() const {
  const Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  std::vector<DownloadInfo> out;
  out.reserve(d.records.size());
  for (const auto &[id, r] : d.records) {
    out.push_back(d.info_locked(r));
  }
  return out;
}

std::optional<DownloadInfo> Engine::get(DownloadId id) const {
  const Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  const auto it = d.records.find(id);
  if (it == d.records.end()) {
    return std::nullopt;
  }
  return d.info_locked(it->second);
}

std::optional<DownloadId> Engine::find_by_url(const std::string &url) const {
  const Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  for (const auto &[id, r] : d.records) {
    if (r.url == url) {
      return id;
    }
  }
  return std::nullopt;
}

std::vector<category::Category> Engine::categories() const {
  const Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  return d.categories;
}

Result<void> Engine::set_categories(std::vector<category::Category> categories) {
  for (const auto &c : categories) {
    if (c.name.empty() || c.name == category::kGeneral) {
      return make_error(ErrorCode::InvalidArgument,
                        "a category needs a name other than \"General\" (the fallback)");
    }
  }
  Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  if (auto ok = d.store.set_setting("categories", category::to_json(categories)); !ok) {
    return ok;
  }
  d.categories = std::move(categories);
  return {};
}

std::vector<queue::QueueSettings> Engine::queues() const {
  const Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  std::vector<queue::QueueSettings> out;
  for (const auto &[name, q] : d.queues) {
    out.push_back(q);
  }
  return out;
}

Result<void> Engine::save_queue(const queue::QueueSettings &q) {
  if (q.name.empty()) {
    return make_error(ErrorCode::InvalidArgument, "a queue needs a name");
  }
  if (q.max_active < 1 || q.max_active > kMaxConnections) {
    return make_error(ErrorCode::InvalidArgument, "max downloads at once must be 1-32");
  }
  Impl &d = *impl_;
  {
    std::lock_guard lock(d.mutex);
    if (auto ok = d.store.save_queue(q); !ok) {
      return ok;
    }
    const bool was_enabled = d.queues.contains(q.name) && d.queues[q.name].enabled;
    d.queues[q.name] = q;
    if (was_enabled && !q.enabled) {
      d.stop_queue_locked(d.queues[q.name]);
    }
    d.wake_scheduler_locked();
  }
  d.publish();
  return {};
}

Result<void> Engine::start_queue(const std::string &name) {
  Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  const auto it = d.queues.find(name);
  if (it == d.queues.end()) {
    return make_error(ErrorCode::NotFound, "no queue named '" + name + "'");
  }
  it->second.enabled = true;
  d.wake_scheduler_locked();
  return d.store.save_queue(it->second);
}

Result<void> Engine::stop_queue(const std::string &name) {
  Impl &d = *impl_;
  {
    std::lock_guard lock(d.mutex);
    const auto it = d.queues.find(name);
    if (it == d.queues.end()) {
      return make_error(ErrorCode::NotFound, "no queue named '" + name + "'");
    }
    d.stop_queue_locked(it->second);
  }
  d.publish();
  return {};
}

std::vector<std::pair<std::string, std::string>> Engine::settings() const {
  const Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  std::map<std::string, std::string> values = {
      {"download_dir", platform::path_to_utf8(d.default_directory_locked())},
      {"connections", "8"},
      {"max_active", "4"},
      {"speed_limit", "0"},
      {"category_folders", "1"},
      {"system_proxy", "1"},
  };
  for (const auto &[key, value] : d.settings) {
    values[key] = value;
  }
  return {values.begin(), values.end()};
}

Result<void> Engine::set_setting(const std::string &key, const std::string &value) {
  const auto number = parse_uint(value);
  if (key == "connections") {
    if (!number || *number < 1 || *number > kMaxConnections) {
      return make_error(ErrorCode::InvalidArgument, "connections must be 1-32");
    }
  } else if (key == "max_active") {
    if (!number || *number < 1 || *number > 64) {
      return make_error(ErrorCode::InvalidArgument, "max_active must be 1-64");
    }
  } else if (key == "speed_limit") {
    if (!number) {
      return make_error(ErrorCode::InvalidArgument, "speed_limit must be bytes per second");
    }
  } else if (key == "download_dir") {
    if (value.empty()) {
      return make_error(ErrorCode::InvalidArgument, "download_dir cannot be empty");
    }
  } else if (key == "category_folders" || key == "system_proxy") {
    if (!number || *number > 1) {
      return make_error(ErrorCode::InvalidArgument, key + " must be 0 or 1");
    }
  } else {
    return make_error(ErrorCode::InvalidArgument,
                      "unknown setting '" + key +
                          "' (known: download_dir, connections, max_active, speed_limit, "
                          "category_folders, system_proxy)");
  }
  Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  if (auto ok = d.store.set_setting(key, value); !ok) {
    return ok;
  }
  d.settings[key] = value;
  if (key == "speed_limit") {
    d.global_limit->set_rate(*number);
  }
  d.wake_scheduler_locked();
  return {};
}

bool Engine::idle() const {
  const Impl &d = *impl_;
  std::lock_guard lock(d.mutex);
  for (const auto &[id, w] : d.workers) {
    if (!w->finished) {
      return false;
    }
  }
  for (const auto &[id, r] : d.records) {
    if (r.state == DownloadState::Scheduled || r.state == DownloadState::Running) {
      return false;
    }
    if (r.state == DownloadState::Queued) {
      const auto q = d.queues.find(r.queue);
      const bool will_start =
          r.start_now || (q != d.queues.end() && (q->second.enabled || q->second.start_time));
      if (will_start) {
        return false;
      }
    }
  }
  return true;
}

int Engine::subscribe(std::function<void(const EngineEvent &)> callback) {
  Impl &d = *impl_;
  std::lock_guard lock(d.events_mutex);
  const int id = d.next_subscription++;
  d.subscribers.emplace_back(id, std::move(callback));
  return id;
}

void Engine::unsubscribe(int subscription) {
  Impl &d = *impl_;
  std::lock_guard lock(d.events_mutex);
  std::erase_if(d.subscribers, [&](const auto &s) { return s.first == subscription; });
}

void Engine::shutdown() {
  Impl &d = *impl_;
  std::map<DownloadId, std::unique_ptr<Impl::Worker>> workers;
  {
    std::lock_guard lock(d.mutex);
    if (d.shut_down) {
      return;
    }
    d.shut_down = true;
    for (auto &[id, w] : d.workers) {
      if (!w->finished) {
        if (const auto it = d.records.find(id); it != d.records.end()) {
          it->second.state = DownloadState::Queued; // continues on the next start
          it->second.start_now = true;
          d.persist_locked(it->second);
        }
        w->thread.request_stop();
      }
    }
    d.wake.notify_all();
  }
  d.scheduler.request_stop();
  if (d.scheduler.joinable()) {
    d.scheduler.join();
  }
  // Join in place (without the lock): each worker's on_finished still finds its entry and
  // records its final progress; the scheduler is stopped, so nothing adds or reaps workers.
  std::vector<Impl::Worker *> running;
  {
    std::lock_guard lock(d.mutex);
    for (auto &[id, w] : d.workers) {
      running.push_back(w.get());
    }
  }
  for (Impl::Worker *w : running) {
    if (w->thread.joinable()) {
      w->thread.join();
    }
  }
  {
    std::lock_guard lock(d.mutex);
    workers.swap(d.workers);
  }
  workers.clear();
  d.graveyard.clear();
}

} // namespace lanes::core
