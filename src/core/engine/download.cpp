// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/download.hpp"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "core/engine/speed_meter.hpp"
#include "core/net/curl.hpp"
#include "core/platform/file.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "core/resume/sidecar.hpp"

namespace lanes::core::engine {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// Memory for batching received bytes into large positional writes, shared by all connections.
// Measured on a 1 GiB loopback transfer: ~4 MiB per write was fastest on one connection; with many
// connections each gets a share, bounded below so writes stay large.
constexpr std::size_t kTotalWriteBuffer = 16 << 20;
constexpr std::size_t kMaxWriteBuffer = 4 << 20;
constexpr std::size_t kMinWriteBuffer = 512 << 10;
constexpr std::size_t kReceiveChunk = 512 << 10; // CURLOPT_BUFFERSIZE set in net::apply_request

// Upper bound on one idle wait. curl_multi_poll on Windows sometimes misses data that is already
// waiting in the socket and sleeps the whole timeout (measured: 100 ms waits took up to 75 % of a
// 1 GiB transfer), so the timeout is kept short; it also bounds cancel latency.
constexpr auto kPollTimeout = std::chrono::milliseconds(10);

constexpr int kMaxConnections = 32;

// Slow-connection replacement (§4.3).
constexpr auto kSpeedCheckInterval = std::chrono::milliseconds(500);
constexpr auto kMinAgeBeforeReplace = std::chrono::seconds(2); // connect + TCP slow start
constexpr double kSlowFactor = 3.0;                            // slower than median / 3
constexpr double kMinSecondsLeft = 2.0; // not worth replacing if it finishes soon anyway
constexpr int kMaxRestarts = 3;         // per byte range
constexpr std::uint64_t kSplitAlignment = 64 << 10;

class Downloader;

// One transfer: a byte range of the file (or the whole file) over its own connection. Owned by
// the Downloader and touched only on its event-loop thread, so no locking is needed: a split
// shrinks `end` on that thread and on_write honours the new end on the next chunk.
struct Connection {
  Downloader *owner = nullptr;
  std::uint64_t start = 0;          // file offset of the first byte
  std::optional<std::uint64_t> end; // inclusive; nullopt = until the server stops
  bool empty = false;               // replaced before receiving anything: covers no bytes
  bool ranged = false;              // sends Range: start-end and expects 206
  std::uint64_t written = 0;        // bytes handed to the file
  std::string buffer;               // received, not yet written
  bool started = false;             // response headers checked
  bool reached_end = false;         // stopped itself at `end`
  bool paused = false;              // waiting for speed-limit tokens (CURL_WRITEFUNC_PAUSE)
  bool finished = false;
  CURLcode result = CURLE_OK;
  std::optional<Error> error;
  net::EasyHandle easy;
  net::RequestStorage storage;

  // Speed tracking for slow-connection replacement.
  Clock::time_point created{};
  Clock::time_point checkpoint_time{};
  std::uint64_t checkpoint_received = 0;
  double speed = -1; // bytes/s over the last check interval; -1 = not measured yet
  int restarts = 0;  // how many times this byte range has been moved to a new connection
  int retries = 0;   // consecutive failed attempts at this range without progress

  std::uint64_t received() const { return written + buffer.size(); }
  std::optional<std::uint64_t> length() const {
    if (empty) {
      return 0;
    }
    return end ? std::optional(*end - start + 1) : std::nullopt;
  }
  // Bytes still to come (ranged connections only).
  std::uint64_t remaining() const { return *length() - received(); }
};

// "bytes 100-199/1000" → {100, 1000}. nullopt if malformed; total 0 if '*'.
std::optional<std::pair<std::uint64_t, std::uint64_t>> parse_content_range(std::string_view v) {
  const auto space = v.find(' ');
  const auto dash = v.find('-');
  const auto slash = v.find('/');
  if (space == std::string_view::npos || dash == std::string_view::npos ||
      slash == std::string_view::npos || !(space < dash && dash < slash)) {
    return std::nullopt;
  }
  std::uint64_t first = 0;
  if (std::from_chars(v.data() + space + 1, v.data() + dash, first).ec != std::errc{}) {
    return std::nullopt;
  }
  std::uint64_t total = 0;
  std::string_view total_text = v.substr(slash + 1);
  while (!total_text.empty() && (total_text.back() == ' ' || total_text.back() == '\r')) {
    total_text.remove_suffix(1);
  }
  if (total_text != "*" &&
      std::from_chars(total_text.data(), total_text.data() + total_text.size(), total).ec !=
          std::errc{}) {
    return std::nullopt;
  }
  return std::pair{first, total};
}

// Makes checkpoints durable off the event-loop thread. FlushFileBuffers and the sidecar write can
// block for hundreds of milliseconds on a busy or slow disk; on the loop they stalled every
// connection (measured: a 0.13 s parallel download took 1 s under test load).
//
// Ordering (§4.4): the snapshot is taken after the loop wrote the bytes it lists, and the worker
// flushes the file before writing the sidecar, so the sidecar never claims bytes not on disk.
// Only the newest pending snapshot is written. Thread-safe.
class CheckpointWriter {
public:
  CheckpointWriter(platform::File &file, fs::path sidecar)
      : file_(file), sidecar_(std::move(sidecar)),
        thread_([this](std::stop_token stop) { run(std::move(stop)); }) {}
  ~CheckpointWriter() { stop(); }
  CheckpointWriter(const CheckpointWriter &) = delete;
  CheckpointWriter &operator=(const CheckpointWriter &) = delete;

  void submit(resume::ResumeState state) {
    {
      std::lock_guard lock(mutex_);
      pending_ = std::move(state);
    }
    wake_.notify_one();
  }

  // Ends the worker after the write in progress; a pending snapshot is dropped. No sidecar write
  // happens after this returns.
  void stop() {
    if (thread_.joinable()) {
      thread_.request_stop();
      thread_.join();
    }
  }

  std::optional<Error> error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

private:
  void run(std::stop_token stop) {
    for (;;) {
      resume::ResumeState state;
      {
        std::unique_lock lock(mutex_);
        if (!wake_.wait(lock, stop, [&] { return pending_.has_value(); })) {
          return; // stop requested
        }
        state = std::move(*pending_);
        pending_.reset();
      }
      Result<void> ok = file_.flush();
      if (ok) {
        ok = resume::save(sidecar_, state);
      }
      if (!ok) {
        std::lock_guard lock(mutex_);
        error_ = ok.error();
      }
    }
  }

  platform::File &file_; // flush() may run concurrently with the loop's writes
  fs::path sidecar_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<resume::ResumeState> pending_;
  std::optional<Error> error_;
  std::jthread thread_; // last: starts after the members it uses
};

class Downloader {
public:
  Downloader(const DownloadOptions &options, std::stop_token stop)
      : options_(options), stop_(std::move(stop)) {}
  ~Downloader() { stop_checkpoints(); } // before file_ goes away
  Downloader(const Downloader &) = delete;
  Downloader &operator=(const Downloader &) = delete;

  Result<DownloadResult> run();

private:
  enum class Outcome { Done, Cancelled, Failed, RangeIgnored };

  static std::size_t on_write(char *data, std::size_t size, std::size_t count, void *user);

  Result<void> on_response(Connection &c);
  void set_paths(const std::string &name);
  Result<void> prepare_file();
  Result<bool> open_existing_part();
  std::optional<resume::ResumeState> load_resume_state();
  Result<void> plan_fresh(int segments);
  Result<void> plan_resumed(const resume::ResumeState &saved);
  Result<void> add_ranges(std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges);
  Result<void> checkpoint();
  Result<void> checkpoint_now();
  void stop_checkpoints();
  Result<resume::ResumeState> snapshot();
  Error link_expired_or(Error error) const;
  Result<void> flush(Connection &c);
  Result<std::unique_ptr<Connection>>
  make_connection(std::uint64_t start, std::optional<std::uint64_t> end, bool ranged);
  Result<void> launch(std::uint64_t start, std::optional<std::uint64_t> end, bool ranged,
                      int restarts, int retries);
  Outcome handle_failure(Connection &c);
  Error failure_of(Connection &c) const;
  bool retryable(const Error &error) const;
  bool server_wants_fewer_connections(const Error &error, const Connection &c) const;
  std::chrono::milliseconds retry_delay(const Connection *c, int retries);
  Result<void> launch_due_retries(Clock::time_point now);
  std::size_t active_connections() const;
  bool wait_or_stop(std::chrono::milliseconds delay);
  Result<net::ProbeResult> probe_with_retries();
  Result<void> split_for_idle_connection();
  Result<void> replace_slow_connections(Clock::time_point now);
  Result<void> replace(Connection &c);
  Result<void> release(Connection &c);
  Outcome transfer();
  void keep_partial();
  void report_progress(bool force);
  bool dynamic() const { return ranged_; }

  const DownloadOptions &options_;
  std::stop_token stop_;

  std::optional<net::ProbeResult> info_;
  std::vector<std::unique_ptr<Connection>> connections_;
  // Ranged connections over a known size: may split and restart, and are resumable (checkpointed).
  bool ranged_ = false;
  fs::path sidecar_path_;
  std::unique_ptr<CheckpointWriter> checkpointer_; // uses file_: stopped in ~Downloader
  Clock::time_point last_checkpoint_{};
  std::uint64_t resumed_from_ = 0;
  std::size_t write_buffer_size_ = kMaxWriteBuffer;
  bool range_ignored_ = false;
  std::optional<Error> failure_;

  CURLM *multi_ = nullptr; // valid during transfer()
  std::size_t finished_ = 0;
  int splits_ = 0;
  int restarts_ = 0;

  // A range waiting to be retried (its bytes so far are kept by the failed connection).
  struct PendingRetry {
    std::uint64_t start = 0;
    std::optional<std::uint64_t> end;
    bool ranged = true;
    int restarts = 0;
    int retries = 0;
    Clock::time_point not_before{};
  };
  std::vector<PendingRetry> pending_;
  std::vector<std::shared_ptr<limiter::RateLimiter>> limiters_; // this download's + shared ones
  bool limited() const;
  bool tokens_available() const;
  void resume_paused();
  std::size_t max_active_ = 1; // connection limit; lowered when the server pushes back
  int retries_ = 0;
  std::minstd_rand rng_{std::random_device{}()};
  double reference_speed_ = 0; // median connection speed while several were measured
  Clock::time_point last_speed_check_{};

  fs::path final_path_;
  fs::path part_path_;
  platform::File file_;
  bool sparse_ = false;
  std::uint64_t received_ = 0; // all connections, bytes accepted from the network

  SpeedMeter meter_;
  Clock::time_point last_report_{};
};

std::size_t Downloader::on_write(char *data, std::size_t size, std::size_t count, void *user) {
  auto &c = *static_cast<Connection *>(user);
  Downloader &self = *c.owner;
  const std::size_t length = size * count;
  if (!c.started) {
    if (auto ok = self.on_response(c); !ok) {
      c.error = ok.error();
      return CURL_WRITEFUNC_ERROR;
    }
  }
  // Speed limit: no tokens → pause; curl delivers this same chunk again after the event loop
  // unpauses the connection. Never sleep here: it would stall every connection (§4.6).
  if (!self.limiters_.empty() && !self.tokens_available()) {
    c.paused = true;
    return CURL_WRITEFUNC_PAUSE;
  }

  // Never write past the segment's (possibly shrunk) end: those bytes belong to another
  // connection.
  std::size_t take = length;
  if (const auto len = c.length()) {
    take = static_cast<std::size_t>(std::min<std::uint64_t>(length, *len - c.received()));
  }
  c.buffer.append(data, take);
  self.received_ += take;
  for (const auto &limiter : self.limiters_) {
    limiter->consume(take);
  }

  const bool complete = c.length() && c.received() == *c.length();
  if (c.buffer.size() >= self.write_buffer_size_ || complete) {
    if (auto ok = self.flush(c); !ok) {
      c.error = ok.error();
      return CURL_WRITEFUNC_ERROR;
    }
  }
  if (take < length) {
    c.reached_end = true; // stop this transfer; its range is done
    return CURL_WRITEFUNC_ERROR;
  }
  return length;
}

// Runs once per connection when its response headers are in.
Result<void> Downloader::on_response(Connection &c) {
  c.started = true;
  CURL *easy = c.easy.get();
  if (net::is_http(easy)) {
    const long status = net::response_code(easy);
    if (c.ranged) {
      if (status == 200) {
        range_ignored_ = true; // the whole file is coming: fall back to one connection
        return make_error(ErrorCode::Unsupported, "server ignored the range request");
      }
      if (status != 206) {
        return make_error(net::http_status_error(status));
      }
      const auto range =
          parse_content_range(net::last_response_header(easy, "Content-Range").value_or(""));
      if (range && range->second != 0 && info_->size && range->second != *info_->size) {
        return make_error(ErrorCode::Http,
                          "the file changed on the server since it was checked (size " +
                              std::to_string(*info_->size) + " → " + std::to_string(range->second) +
                              ")");
      }
      if (!range || range->first != c.start + c.written) {
        return make_error(ErrorCode::Http, "server sent the wrong byte range");
      }
      return {};
    }
    if (status < 200 || status >= 300) {
      return make_error(net::http_status_error(status));
    }
  }

  // Whole-file transfer: this response may be the first we learn about the file (POST).
  const net::ProbeResult response = net::probe_result_from_response(easy);
  if (!info_) {
    info_ = response;
  } else if (info_->size && response.size && *info_->size != *response.size) {
    return make_error(ErrorCode::Http,
                      "the file changed on the server since it was checked (size " +
                          std::to_string(*info_->size) + " → " + std::to_string(*response.size) +
                          ")");
  }
  if (!c.end && info_->size && *info_->size > 0) {
    c.end = *info_->size - 1;
  }
  if (!file_.is_open()) {
    return prepare_file();
  }
  return {};
}

void Downloader::set_paths(const std::string &name) {
  final_path_ = options_.directory / platform::path_from_utf8(name);
  part_path_ = final_path_;
  part_path_ += ".part";
  sidecar_path_ = resume::sidecar_path(final_path_);
}

// New .part file: folder, space check, sized up front (sparse where supported).
Result<void> Downloader::prepare_file() {
  if (final_path_.empty()) {
    set_paths(options_.filename.empty() ? info_->filename
                                        : platform::sanitize_filename(options_.filename));
  }
  if (auto ok = platform::create_directories(options_.directory); !ok) {
    return ok;
  }
  if (info_->size) {
    if (auto ok = platform::check_can_store(options_.directory, *info_->size); !ok) {
      return ok;
    }
  }
  auto file = platform::File::open(part_path_, platform::OpenMode::CreateAlways);
  if (!file) {
    return make_error(file.error());
  }
  file_ = std::move(*file);
  if (info_->size && *info_->size > 0) {
    auto sparse = file_.preallocate(*info_->size);
    if (!sparse) {
      return make_error(sparse.error());
    }
    sparse_ = *sparse;
  }
  return {};
}

// Reopens the .part file of an earlier run without truncating it. False if it is missing or has
// the wrong size (then the download starts over).
Result<bool> Downloader::open_existing_part() {
  auto file = platform::File::open(part_path_, platform::OpenMode::ReadWrite);
  if (!file) {
    return file.error().code == ErrorCode::NotFound ? Result<bool>(false)
                                                    : make_error(file.error());
  }
  auto size = file->size();
  if (!size) {
    return make_error(size.error());
  }
  if (*size != *info_->size) {
    return false;
  }
  file_ = std::move(*file);
  auto sparse = file_.set_sparse(true); // holes may remain: keep them unallocated
  sparse_ = sparse && *sparse;
  return true;
}

// The saved state of an earlier run, if it exists, matches the .part file and the server's file
// is unchanged. A changed file is reported through failure_ (unless restarting is allowed).
std::optional<resume::ResumeState> Downloader::load_resume_state() {
  auto loaded = resume::load(sidecar_path_);
  if (!loaded || !*loaded) {
    return std::nullopt; // none, or unreadable/corrupt: cannot be trusted, start over
  }
  const resume::ResumeState &saved = **loaded;
  const bool size_changed = saved.size != *info_->size;
  const bool etag_changed =
      !saved.etag.empty() && !info_->etag.empty() && saved.etag != info_->etag;
  const bool date_changed = saved.etag.empty() && info_->etag.empty() &&
                            !saved.last_modified.empty() && !info_->last_modified.empty() &&
                            saved.last_modified != info_->last_modified;
  if (size_changed || etag_changed || date_changed) {
    if (!options_.restart_if_changed) {
      failure_ = Error{ErrorCode::RemoteChanged,
                       "the file on the server changed since this download started (" +
                           std::string(size_changed   ? "size"
                                       : etag_changed ? "ETag"
                                                      : "date") +
                           " differs); the " + std::to_string(saved.done_bytes()) +
                           " bytes already downloaded cannot be reused",
                       0};
    }
    return std::nullopt;
  }
  return **loaded;
}

// Fresh ranged download: `segments` equal ranges.
Result<void> Downloader::plan_fresh(int segments) {
  const std::uint64_t size = *info_->size;
  const std::uint64_t chunk = size / static_cast<std::uint64_t>(segments);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  for (int i = 0; i < segments; ++i) {
    const std::uint64_t start = chunk * static_cast<std::uint64_t>(i);
    ranges.emplace_back(start, i == segments - 1 ? size - 1 : start + chunk - 1);
  }
  return add_ranges(std::move(ranges));
}

// Resumed download: finished bytes become completed segments; the gaps are fetched, halved until
// every connection has work (or the pieces would get too small).
Result<void> Downloader::plan_resumed(const resume::ResumeState &saved) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> missing;
  for (const auto &s : saved.segments) {
    if (s.done > 0) {
      auto done = std::make_unique<Connection>();
      done->owner = this;
      done->start = s.start;
      done->end = s.start + s.done - 1;
      done->ranged = true;
      done->written = s.done;
      done->started = true;
      done->finished = true;
      connections_.push_back(std::move(done));
    }
    if (s.done < s.length()) {
      missing.emplace_back(s.start + s.done, s.end);
    }
  }
  const auto length = [](const auto &r) { return r.second - r.first + 1; };
  while (missing.size() < static_cast<std::size_t>(options_.connections)) {
    auto largest =
        std::max_element(missing.begin(), missing.end(),
                         [&](const auto &a, const auto &b) { return length(a) < length(b); });
    if (largest == missing.end() || length(*largest) / 2 < options_.min_segment_size) {
      break;
    }
    std::uint64_t middle = largest->first + length(*largest) / 2;
    if (const std::uint64_t aligned = middle - middle % kSplitAlignment; aligned > largest->first) {
      middle = aligned;
    }
    const std::pair<std::uint64_t, std::uint64_t> second{middle, largest->second};
    largest->second = middle - 1;
    missing.push_back(second);
  }
  std::sort(missing.begin(), missing.end());
  resumed_from_ = saved.done_bytes();
  received_ = resumed_from_;
  return add_ranges(std::move(missing));
}

Result<void> Downloader::add_ranges(std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges) {
  write_buffer_size_ = std::clamp(kTotalWriteBuffer / std::max<std::size_t>(ranges.size(), 1),
                                  kMinWriteBuffer, kMaxWriteBuffer);
  for (const auto &[start, end] : ranges) {
    auto c = make_connection(start, end, true);
    if (!c) {
      return make_error(c.error());
    }
    connections_.push_back(std::move(*c));
  }
  return {};
}

// Queues a checkpoint: buffered bytes are written here (fast, into the OS cache); the background
// writer makes them durable and then replaces the sidecar.
Result<void> Downloader::checkpoint() {
  if (!ranged_ || !file_.is_open()) {
    return {};
  }
  if (!checkpointer_) {
    checkpointer_ = std::make_unique<CheckpointWriter>(file_, sidecar_path_);
  }
  if (auto failed = checkpointer_->error()) {
    return make_error(*failed);
  }
  auto state = snapshot();
  if (!state) {
    return make_error(state.error());
  }
  last_checkpoint_ = Clock::now();
  checkpointer_->submit(std::move(*state));
  return {};
}

// Final checkpoint on cancel/failure, synchronously: stop the writer, flush, save.
Result<void> Downloader::checkpoint_now() {
  if (!ranged_ || !file_.is_open()) {
    return {};
  }
  stop_checkpoints();
  auto state = snapshot();
  if (!state) {
    return make_error(state.error());
  }
  if (auto ok = file_.flush(); !ok) {
    return ok;
  }
  return resume::save(sidecar_path_, *state);
}

void Downloader::stop_checkpoints() {
  if (checkpointer_) {
    checkpointer_->stop();
  }
}

// Writes every buffered byte, then describes what is in the file.
Result<resume::ResumeState> Downloader::snapshot() {
  for (auto &c : connections_) {
    if (auto ok = flush(*c); !ok) {
      return make_error(ok.error());
    }
  }
  // Every byte range: live/finished connections plus ranges waiting for a retry, so the saved map
  // still tiles the file while a retry is pending.
  std::vector<resume::SavedSegment> parts;
  for (const auto &c : connections_) {
    if (!c->empty) {
      parts.push_back({c->start, *c->end, c->written});
    }
  }
  for (const auto &p : pending_) {
    parts.push_back({p.start, *p.end, 0});
  }
  std::sort(parts.begin(), parts.end(),
            [](const auto &a, const auto &b) { return a.start < b.start; });
  resume::ResumeState state;
  state.url = net::strip_credentials(options_.request.url);
  state.final_url = net::strip_credentials(info_->final_url);
  state.filename = platform::path_to_utf8(final_path_.filename());
  state.size = *info_->size;
  state.etag = info_->etag;
  state.last_modified = info_->last_modified;
  for (const resume::SavedSegment &s : parts) {
    // Merge into a completely written predecessor: keeps the file short after many splits.
    if (!state.segments.empty() && state.segments.back().done == state.segments.back().length()) {
      auto &prev = state.segments.back();
      prev.done += s.done;
      prev.end = s.end;
    } else {
      state.segments.push_back(s);
    }
  }
  return state;
}

// 403/410 after data has been downloaded almost always means the signed/temporary address has
// expired: report it so the front-end can offer "refresh download address".
Error Downloader::link_expired_or(Error error) const {
  const bool has_data = received_ > 0 || resumed_from_ > 0;
  if (error.code == ErrorCode::Http && (error.http_status == 403 || error.http_status == 410) &&
      has_data) {
    error.code = ErrorCode::LinkExpired;
    error.message = "the download address no longer works (HTTP " +
                    std::to_string(error.http_status) +
                    "); refresh the address to continue — downloaded data is kept";
  }
  return error;
}

Result<void> Downloader::flush(Connection &c) {
  if (c.buffer.empty()) {
    return {};
  }
  if (auto ok = file_.write_at(c.start + c.written, c.buffer); !ok) {
    return ok;
  }
  c.written += c.buffer.size();
  c.buffer.clear();
  return {};
}

Result<std::unique_ptr<Connection>>
Downloader::make_connection(std::uint64_t start, std::optional<std::uint64_t> end, bool ranged) {
  auto c = std::make_unique<Connection>();
  c->owner = this;
  c->start = start;
  c->end = end;
  c->ranged = ranged;
  c->created = Clock::now();
  c->checkpoint_time = c->created;
  c->easy = net::make_easy();
  if (auto ok = net::apply_request(c->easy.get(), options_.request, c->storage); !ok) {
    return make_error(ok.error());
  }
  CURL *easy = c->easy.get();
  // HTTP/1.1 per connection: with HTTP/2 every range would share one TCP connection and the
  // parallel speed-up would disappear.
  curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &Downloader::on_write);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, c.get());
  curl_easy_setopt(easy, CURLOPT_PRIVATE, c.get());
  if (ranged) {
    const std::string range = std::to_string(start) + "-" + std::to_string(*end);
    curl_easy_setopt(easy, CURLOPT_RANGE, range.c_str());
  }
  if (limited()) {
    std::uint64_t slowest = 0;
    for (const auto &limiter : limiters_) {
      if (const auto rate = limiter->rate(); rate > 0 && (slowest == 0 || rate < slowest)) {
        slowest = rate;
      }
    }
    // Reads of ~100 ms worth keep a throttled flow smooth instead of 512 KiB lurches.
    const auto chunk = std::clamp<std::uint64_t>(slowest / 10, 16 << 10, kReceiveChunk);
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, static_cast<long>(chunk));
    // A connection waiting for tokens receives nothing for a while: only a truly dead one
    // (nothing at all for low_speed_time) should count as stalled.
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
  }
  c->buffer.reserve(write_buffer_size_ + kReceiveChunk);
  return c;
}

// Starts a connection on the running multi handle. The multi handle's connection pool reuses an
// idle keep-alive connection to the host when one is available.
Result<void> Downloader::launch(std::uint64_t start, std::optional<std::uint64_t> end, bool ranged,
                                int restarts, int retries) {
  auto c = make_connection(start, end, ranged);
  if (!c) {
    return make_error(c.error());
  }
  (*c)->restarts = restarts;
  (*c)->retries = retries;
  curl_multi_add_handle(multi_, (*c)->easy.get());
  connections_.push_back(std::move(*c));
  return {};
}

bool Downloader::limited() const {
  return std::any_of(limiters_.begin(), limiters_.end(),
                     [](const auto &limiter) { return limiter->rate() > 0; });
}

bool Downloader::tokens_available() const {
  const auto now = limiter::RateLimiter::Clock::now();
  return std::all_of(limiters_.begin(), limiters_.end(),
                     [&](const auto &limiter) { return limiter->available(now); });
}

// Unpauses connections once every bucket has tokens again. curl may deliver the held chunk from
// inside curl_easy_pause, which can pause the connection again: that is fine.
void Downloader::resume_paused() {
  if (!tokens_available()) {
    return;
  }
  for (auto &c : connections_) {
    if (c->paused && !c->finished && c->easy) {
      c->paused = false;
      curl_easy_pause(c->easy.get(), CURLPAUSE_CONT);
    }
  }
}

std::size_t Downloader::active_connections() const {
  return static_cast<std::size_t>(std::count_if(connections_.begin(), connections_.end(),
                                                [](const auto &c) { return !c->finished; }));
}

// What went wrong with a failed connection.
Error Downloader::failure_of(Connection &c) const {
  if (c.error) {
    return *c.error;
  }
  if (c.easy && net::is_http(c.easy.get()) && net::response_code(c.easy.get()) >= 400) {
    return net::http_status_error(net::response_code(c.easy.get()));
  }
  return net::transfer_error(c.result, c.storage.error_buffer);
}

// Transient failures worth another attempt; everything else (404, disk errors, a changed file,
// a wrong range) fails the download.
bool Downloader::retryable(const Error &error) const {
  if (error.code == ErrorCode::Network || error.code == ErrorCode::Timeout) {
    return true;
  }
  if (error.code == ErrorCode::Http) {
    switch (error.http_status) {
    case 408: // request timeout
    case 425: // too early
    case 429: // too many requests
    case 500:
    case 502:
    case 503:
    case 504:
      return true;
    default:
      return false;
    }
  }
  return false;
}

// The server is limiting us: "too many requests", or it refuses/drops new connections while
// others still work (many servers cap connections per IP).
bool Downloader::server_wants_fewer_connections(const Error &error, const Connection &c) const {
  if (error.http_status == 429 || error.http_status == 503) {
    return true;
  }
  const bool others_working = active_connections() > 0;
  const bool refused = c.result == CURLE_COULDNT_CONNECT || c.result == CURLE_GOT_NOTHING;
  const bool dropped_at_once =
      (c.result == CURLE_RECV_ERROR || c.result == CURLE_SEND_ERROR) && c.received() == 0;
  return others_working && (refused || dropped_at_once);
}

// Retry-After (seconds) if the server sent one, else exponential backoff with ±25 % jitter.
std::chrono::milliseconds Downloader::retry_delay(const Connection *c, int retries) {
  constexpr auto kMaxBackoff = std::chrono::milliseconds(30'000);
  constexpr auto kMaxRetryAfter = std::chrono::milliseconds(120'000);
  if (c != nullptr && c->easy) {
    if (const auto header = net::last_response_header(c->easy.get(), "Retry-After")) {
      long long seconds = 0;
      const auto [ptr, ec] =
          std::from_chars(header->data(), header->data() + header->size(), seconds);
      if (ec == std::errc{} && seconds >= 0) {
        return std::min<std::chrono::milliseconds>(std::chrono::seconds(seconds), kMaxRetryAfter);
      }
    }
  }
  const int doublings = std::min(retries - 1, 16);
  const auto base =
      std::min<std::chrono::milliseconds>(options_.retry_delay * (1LL << doublings), kMaxBackoff);
  std::uniform_real_distribution<double> jitter(0.75, 1.25);
  return std::chrono::milliseconds(
      static_cast<long long>(static_cast<double>(base.count()) * jitter(rng_)));
}

// A connection failed. Retryable: keep its bytes, queue the rest of its range (returns Done so
// the loop carries on). Otherwise: Failed, or RangeIgnored for the single-connection fallback.
Downloader::Outcome Downloader::handle_failure(Connection &c) {
  if (range_ignored_) {
    return Outcome::RangeIgnored;
  }
  Error error = failure_of(c);
  if (!retryable(error)) {
    failure_ = error;
    return Outcome::Failed;
  }
  // Progress since the last failure resets the count: only stuck ranges give up.
  const int retries = c.received() > 0 ? 1 : c.retries + 1;
  if (retries > options_.max_retries) {
    error.message += " (gave up after " + std::to_string(options_.max_retries) + " retries)";
    failure_ = error;
    return Outcome::Failed;
  }
  if (server_wants_fewer_connections(error, c)) {
    max_active_ = std::max<std::size_t>(1, std::min(max_active_, active_connections()));
  }
  const auto delay = retry_delay(&c, retries);
  ++retries_;

  if (auto ok = flush(c); !ok) {
    failure_ = ok.error();
    return Outcome::Failed;
  }
  PendingRetry retry;
  retry.restarts = c.restarts;
  retry.retries = retries;
  retry.not_before = Clock::now() + delay;
  if (c.ranged) {
    const std::uint64_t position = c.start + c.written;
    const std::uint64_t old_end = *c.end;
    if (position > old_end) {
      (void)release(c);
      return Outcome::Done; // every byte arrived before the error
    }
    if (c.written == 0) {
      c.empty = true;
    } else {
      c.end = position - 1;
    }
    retry.start = position;
    retry.end = old_end;
  } else {
    // A whole-file transfer cannot continue mid-file: start it again from the beginning.
    received_ -= c.received();
    c.written = 0;
    c.buffer.clear();
    c.empty = true;
    retry.ranged = false;
  }
  (void)release(c);
  pending_.push_back(retry);
  return Outcome::Done;
}

// Starts queued retries whose delay has passed, while the connection limit allows.
Result<void> Downloader::launch_due_retries(Clock::time_point now) {
  std::sort(pending_.begin(), pending_.end(), [](const PendingRetry &a, const PendingRetry &b) {
    return a.not_before < b.not_before;
  });
  while (!pending_.empty() && pending_.front().not_before <= now &&
         active_connections() < max_active_) {
    const PendingRetry retry = pending_.front();
    pending_.erase(pending_.begin());
    if (auto ok = launch(retry.start, retry.end, retry.ranged, retry.restarts, retry.retries);
        !ok) {
      return ok;
    }
  }
  return {};
}

// Sleeps up to `delay`; false if a stop was requested meanwhile.
bool Downloader::wait_or_stop(std::chrono::milliseconds delay) {
  const auto until = Clock::now() + delay;
  while (Clock::now() < until) {
    if (stop_.stop_requested()) {
      return false;
    }
    std::this_thread::sleep_for(std::min<Clock::duration>(until - Clock::now(), kPollTimeout * 5));
  }
  return !stop_.stop_requested();
}

// The probe, retried like a range when the failure is transient (e.g. 503 on the first request).
Result<net::ProbeResult> Downloader::probe_with_retries() {
  for (int attempt = 1;; ++attempt) {
    auto probed = net::probe(options_.request);
    if (probed || !retryable(probed.error()) || attempt > options_.max_retries) {
      return probed;
    }
    ++retries_;
    if (!wait_or_stop(retry_delay(nullptr, attempt))) {
      return make_error(ErrorCode::Cancelled, "download cancelled");
    }
  }
}

// A connection just finished: give it the second half of the largest remaining segment.
Result<void> Downloader::split_for_idle_connection() {
  Connection *largest = nullptr;
  for (auto &c : connections_) {
    // Only connections already receiving: one still queued for a host slot gets its turn anyway.
    if (!c->finished && c->ranged && c->started &&
        (largest == nullptr || c->remaining() > largest->remaining())) {
      largest = c.get();
    }
  }
  if (largest == nullptr || largest->remaining() <= options_.min_split_size) {
    return {};
  }
  const std::uint64_t position = largest->start + largest->received();
  std::uint64_t middle = position + largest->remaining() / 2;
  if (const std::uint64_t aligned = middle - middle % kSplitAlignment; aligned > position) {
    middle = aligned; // block-aligned writes for both halves
  }
  const std::uint64_t old_end = *largest->end;
  largest->end = middle - 1; // on_write stops it here
  ++splits_;
  return launch(middle, old_end, true, largest->restarts, 0);
}

// Every check interval: measure each connection's speed and restart those far below the median.
Result<void> Downloader::replace_slow_connections(Clock::time_point now) {
  if (now - last_speed_check_ < kSpeedCheckInterval) {
    return {};
  }
  last_speed_check_ = now;

  std::vector<Connection *> active;
  std::vector<double> speeds;
  for (auto &c : connections_) {
    if (c->finished || !c->ranged || !c->started) {
      continue; // not yet receiving (connecting, or queued for a host slot): nothing to measure
    }
    const double seconds = std::chrono::duration<double>(now - c->checkpoint_time).count();
    if (seconds > 0) {
      c->speed = static_cast<double>(c->received() - c->checkpoint_received) / seconds;
      c->checkpoint_time = now;
      c->checkpoint_received = c->received();
    }
    active.push_back(c.get());
    if (now - c->created >= kMinAgeBeforeReplace / 2) {
      speeds.push_back(c->speed);
    }
  }
  // The reference only updates while several connections can be compared; it stays valid after
  // the others finish, so the last slow connection can still be replaced.
  if (speeds.size() >= 2) {
    std::sort(speeds.begin(), speeds.end());
    reference_speed_ = speeds[speeds.size() / 2];
  }
  if (reference_speed_ <= 0) {
    return {};
  }
  for (Connection *c : active) {
    if (now - c->created < kMinAgeBeforeReplace || c->restarts >= kMaxRestarts || c->speed < 0) {
      continue;
    }
    const double seconds_left =
        c->speed > 0 ? static_cast<double>(c->remaining()) / c->speed : kMinSecondsLeft * 1e6;
    if (c->speed * kSlowFactor < reference_speed_ && seconds_left > kMinSecondsLeft) {
      if (auto ok = replace(*c); !ok) {
        return ok;
      }
    }
  }
  return {};
}

// Moves the rest of `c`'s range to a new connection; `c` keeps what it already received.
Result<void> Downloader::replace(Connection &c) {
  curl_multi_remove_handle(multi_, c.easy.get());
  if (auto ok = flush(c); !ok) {
    return ok;
  }
  const std::uint64_t position = c.start + c.written;
  const std::uint64_t old_end = *c.end;
  if (c.written == 0) {
    c.empty = true;
  } else {
    c.end = position - 1;
  }
  c.finished = true;
  ++finished_;
  ++restarts_;
  if (auto ok = release(c); !ok) {
    return ok;
  }
  return launch(position, old_end, true, c.restarts + 1, 0);
}

// A connection that is done for good: write out its bytes, free its buffer and curl handle.
// Finished connections stay in the list (they describe which bytes are on disk).
Result<void> Downloader::release(Connection &c) {
  if (auto ok = flush(c); !ok) {
    return ok;
  }
  std::string().swap(c.buffer);
  if (c.ranged) {
    c.easy.reset(); // a whole-file transfer keeps its handle: run() may read its headers
  }
  return {};
}

// Runs every connection to completion (or the first failure / cancel) on one multi handle.
Downloader::Outcome Downloader::transfer() {
  auto multi = net::make_multi();
  multi_ = multi.get();
  curl_multi_setopt(multi_, CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_NOTHING));
  curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS,
                    static_cast<long>(options_.max_connections_per_host));
  finished_ = 0;
  for (auto &c : connections_) {
    if (c->finished) {
      ++finished_; // completed in an earlier run
    } else {
      curl_multi_add_handle(multi_, c->easy.get());
    }
  }
  last_speed_check_ = Clock::now();
  last_checkpoint_ = Clock::now();

  Outcome outcome = Outcome::Done;
  while (finished_ < connections_.size() || !pending_.empty()) {
    int running = 0;
    const std::uint64_t received_before = received_;
    curl_multi_perform(multi_, &running);

    int queued = 0;
    while (CURLMsg *msg = curl_multi_info_read(multi_, &queued)) {
      if (msg->msg != CURLMSG_DONE) {
        continue;
      }
      Connection *c = nullptr;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &c);
      curl_multi_remove_handle(multi_, c->easy.get());
      c->finished = true;
      c->result = msg->data.result;
      ++finished_;
      bool ok = c->result == CURLE_OK || (c->result == CURLE_WRITE_ERROR && c->reached_end);
      if (ok && c->ranged && !c->reached_end) {
        // curl reports success for any complete response: an error status without a body never
        // reached on_write, and a server may close cleanly before the range is done.
        if (!c->started) {
          if (auto checked = on_response(*c); !checked) {
            c->error = checked.error();
            ok = false;
          }
        }
        if (ok && c->received() < *c->length()) {
          c->error =
              Error{ErrorCode::Network, "connection closed before its range was complete", 0};
          ok = false;
        }
      }
      if (!ok) {
        if (outcome == Outcome::Done) {
          outcome = handle_failure(*c); // Done again when the range was queued for a retry
        }
        continue;
      }
      if (auto released = release(*c); !released && outcome == Outcome::Done) {
        failure_ = released.error();
        outcome = Outcome::Failed;
      }
      // A free slot serves a waiting retry before it splits someone else's range.
      if (dynamic() && options_.split_when_idle && outcome == Outcome::Done && pending_.empty() &&
          active_connections() < max_active_) {
        if (auto split = split_for_idle_connection(); !split) {
          failure_ = split.error();
          outcome = Outcome::Failed;
        }
      }
    }
    if (outcome != Outcome::Done) {
      break;
    }
    if (stop_.stop_requested()) {
      outcome = Outcome::Cancelled;
      break;
    }
    const auto now = Clock::now();
    if (!limiters_.empty()) {
      resume_paused();
    }
    if (!pending_.empty()) {
      if (auto launched = launch_due_retries(now); !launched) {
        failure_ = launched.error();
        outcome = Outcome::Failed;
        break;
      }
    }
    // Under a speed limit every connection is slow on purpose: nothing to replace.
    if (dynamic() && options_.replace_slow_connections && !limited()) {
      if (auto replaced = replace_slow_connections(now); !replaced) {
        failure_ = replaced.error();
        outcome = Outcome::Failed;
        break;
      }
    }
    if (ranged_ && now - last_checkpoint_ >= options_.checkpoint_interval) {
      if (auto saved = checkpoint(); !saved) {
        failure_ = saved.error();
        outcome = Outcome::Failed;
        break;
      }
    }
    report_progress(false);
    // While data keeps arriving, go straight back to reading; wait only when a round was idle.
    if (received_ == received_before) {
      curl_multi_poll(multi_, nullptr, 0, static_cast<int>(kPollTimeout.count()), nullptr);
    }
  }
  for (auto &c : connections_) {
    if (!c->finished) {
      curl_multi_remove_handle(multi_, c->easy.get());
    }
  }
  multi_ = nullptr;
  return outcome;
}

// Writes out whatever arrived and checkpoints it, so a later run can resume from here.
void Downloader::keep_partial() {
  if (!file_.is_open()) {
    return;
  }
  for (auto &c : connections_) {
    (void)flush(*c);
  }
  (void)checkpoint_now();
  file_.close();
}

void Downloader::report_progress(bool force) {
  const auto now = Clock::now();
  if (!force && now - last_report_ < options_.progress_interval) {
    return;
  }
  last_report_ = now;
  meter_.add(received_, now);
  if (!options_.on_progress) {
    return;
  }
  DownloadProgress p{received_, info_ ? info_->size : std::nullopt, meter_.rate(), {}};
  p.segments.reserve(connections_.size());
  for (const auto &c : connections_) {
    if (!c->empty) {
      p.segments.push_back(SegmentProgress{c->start, c->end, c->received()});
    }
  }
  std::sort(p.segments.begin(), p.segments.end(),
            [](const SegmentProgress &a, const SegmentProgress &b) { return a.start < b.start; });
  options_.on_progress(p);
}

Result<DownloadResult> Downloader::run() {
  if (options_.connections < 1 || options_.connections > kMaxConnections) {
    return make_error(ErrorCode::InvalidArgument, "connections must be between 1 and 32");
  }
  limiters_ = options_.limiters;
  std::erase(limiters_, nullptr);
  if (options_.speed_limit > 0) {
    limiters_.push_back(std::make_shared<limiter::RateLimiter>(options_.speed_limit));
  }
  // A caller-given name is known before probing, so a refused probe can be recognised as an
  // expired address of a download that already has data.
  if (!options_.filename.empty()) {
    set_paths(platform::sanitize_filename(options_.filename));
  }
  if (options_.probe) {
    info_ = options_.probe;
  } else if (options_.request.method != "POST") {
    auto probed = probe_with_retries();
    if (!probed) {
      Error error = probed.error();
      if (!sidecar_path_.empty() && options_.resume) {
        if (auto saved = resume::load(sidecar_path_); saved && *saved) {
          resumed_from_ = (*saved)->done_bytes();
          error = link_expired_or(std::move(error));
        }
      }
      return make_error(std::move(error));
    }
    info_ = std::move(*probed);
  }

  // Ranged plan when the server supports ranges and the size is known (also with one connection,
  // so the download stays resumable); otherwise one whole-file transfer, not resumable.
  ranged_ = info_ && info_->accepts_ranges && info_->size && *info_->size > 0 &&
            options_.min_segment_size > 0;
  int segments = 1;
  if (ranged_) {
    if (final_path_.empty()) {
      set_paths(info_->filename);
    }
    std::optional<resume::ResumeState> saved;
    if (options_.resume) {
      saved = load_resume_state();
      if (failure_) {
        return make_error(*failure_); // RemoteChanged
      }
    }
    bool resumed = false;
    if (saved) {
      auto opened = open_existing_part();
      if (!opened) {
        return make_error(opened.error());
      }
      if (*opened) {
        if (auto ok = plan_resumed(*saved); !ok) {
          return make_error(ok.error());
        }
        resumed = true;
      }
    }
    if (!resumed) {
      const std::uint64_t by_size = *info_->size / options_.min_segment_size;
      segments =
          std::max(1, static_cast<int>(std::min<std::uint64_t>(options_.connections, by_size)));
      if (auto ok = prepare_file(); !ok) {
        return make_error(ok.error());
      }
      if (auto ok = plan_fresh(segments); !ok) {
        return make_error(ok.error());
      }
      if (auto ok = checkpoint(); !ok) { // resumable from the very start
        return make_error(ok.error());
      }
    } else {
      segments = static_cast<int>(std::count_if(connections_.begin(), connections_.end(),
                                                [](const auto &c) { return !c->finished; }));
    }
  } else {
    write_buffer_size_ = kMaxWriteBuffer;
    auto c = make_connection(0, std::nullopt, false);
    if (!c) {
      return make_error(c.error());
    }
    connections_.push_back(std::move(*c));
  }

  max_active_ = static_cast<std::size_t>(std::max(options_.connections, 1));
  meter_.add(received_);
  last_report_ = Clock::now();
  Outcome outcome = transfer();

  if (outcome == Outcome::RangeIgnored) {
    // Start over with one whole-file transfer into the same (already sized) .part file; without
    // ranges it cannot be resumed, so the sidecar goes.
    stop_checkpoints();
    (void)platform::remove_file(sidecar_path_);
    connections_.clear();
    pending_.clear();
    received_ = 0;
    resumed_from_ = 0;
    range_ignored_ = false;
    ranged_ = false;
    segments = 1;
    write_buffer_size_ = kMaxWriteBuffer;
    auto c = make_connection(0, std::nullopt, false);
    if (!c) {
      return make_error(c.error());
    }
    connections_.push_back(std::move(*c));
    outcome = transfer();
  }

  if (outcome == Outcome::Cancelled) {
    keep_partial();
    return make_error(ErrorCode::Cancelled, "download cancelled");
  }
  if (outcome != Outcome::Done) {
    keep_partial();
    return make_error(
        link_expired_or(failure_.value_or(Error{ErrorCode::Network, "download failed", 0})));
  }

  if (!file_.is_open()) {                    // empty body: no write callback ran
    Connection &last = *connections_.back(); // earlier ones may be failed attempts
    last.started = false;
    if (auto ok = on_response(last); !ok) {
      return make_error(ok.error());
    }
  }
  std::uint64_t total = 0;
  for (auto &c : connections_) {
    if (auto ok = flush(*c); !ok) {
      return make_error(ok.error());
    }
    if (const auto len = c->length(); len && c->written != *len) {
      keep_partial();
      return make_error(ErrorCode::Network, "connection ended early: received " +
                                                std::to_string(c->written) + " of " +
                                                std::to_string(*len) + " bytes");
    }
    total += c->written;
  }
  if (info_->size && total != *info_->size) {
    keep_partial();
    return make_error(ErrorCode::Network, "download incomplete: received " + std::to_string(total) +
                                              " of " + std::to_string(*info_->size) + " bytes");
  }
  stop_checkpoints(); // complete: no sidecar write may follow the rename
  if (auto ok = file_.flush(); !ok) {
    return make_error(ok.error());
  }
  if (sparse_) {
    (void)file_.set_sparse(false); // fully written: make it an ordinary file
  }
  file_.close();
  if (auto ok = platform::atomic_replace(part_path_, final_path_); !ok) {
    return make_error(ok.error());
  }
  if (!sidecar_path_.empty()) {
    (void)platform::remove_file(sidecar_path_); // done: nothing left to resume
  }

  if (!info_->size) {
    info_->size = total;
  }
  report_progress(true);
  return DownloadResult{final_path_, total,         segments, splits_,
                        restarts_,   resumed_from_, retries_, static_cast<int>(max_active_),
                        *info_};
}

} // namespace

Result<DownloadResult> download_file(const DownloadOptions &options, std::stop_token stop) {
  Downloader download(options, std::move(stop));
  return download.run();
}

} // namespace lanes::core::engine
