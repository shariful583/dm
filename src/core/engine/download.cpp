// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/download.hpp"

#include <algorithm>
#include <charconv>
#include <memory>

#include "core/engine/speed_meter.hpp"
#include "core/net/curl.hpp"
#include "core/platform/file.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"

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

class Downloader {
public:
  Downloader(const DownloadOptions &options, std::stop_token stop)
      : options_(options), stop_(std::move(stop)) {}

  Result<DownloadResult> run();

private:
  enum class Outcome { Done, Cancelled, Failed, RangeIgnored };

  static std::size_t on_write(char *data, std::size_t size, std::size_t count, void *user);

  Result<void> on_response(Connection &c);
  Result<void> prepare_file();
  Result<void> flush(Connection &c);
  Result<std::unique_ptr<Connection>>
  make_connection(std::uint64_t start, std::optional<std::uint64_t> end, bool ranged);
  Result<void> launch(std::uint64_t start, std::uint64_t end, int restarts);
  Result<void> split_for_idle_connection();
  Result<void> replace_slow_connections(Clock::time_point now);
  Result<void> replace(Connection &c);
  Result<void> release(Connection &c);
  Outcome transfer();
  void record_failure(Connection &c);
  void keep_partial();
  void report_progress(bool force);
  bool dynamic() const { return segmented_; }

  const DownloadOptions &options_;
  std::stop_token stop_;

  std::optional<net::ProbeResult> info_;
  std::vector<std::unique_ptr<Connection>> connections_;
  bool segmented_ = false; // ranged connections: may split and restart
  std::size_t write_buffer_size_ = kMaxWriteBuffer;
  bool range_ignored_ = false;
  std::optional<Error> failure_;

  CURLM *multi_ = nullptr; // valid during transfer()
  std::size_t finished_ = 0;
  int splits_ = 0;
  int restarts_ = 0;
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

  // Never write past the segment's (possibly shrunk) end: those bytes belong to another
  // connection.
  std::size_t take = length;
  if (const auto len = c.length()) {
    take = static_cast<std::size_t>(std::min<std::uint64_t>(length, *len - c.received()));
  }
  c.buffer.append(data, take);
  self.received_ += take;

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

// Final/part paths, folder, space check, .part file sized up front (sparse where supported).
Result<void> Downloader::prepare_file() {
  const std::string name =
      options_.filename.empty() ? info_->filename : platform::sanitize_filename(options_.filename);
  final_path_ = options_.directory / platform::path_from_utf8(name);
  part_path_ = final_path_;
  part_path_ += ".part";

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
  c->buffer.reserve(write_buffer_size_ + kReceiveChunk);
  return c;
}

// Starts a ranged connection for [start, end] on the running multi handle. The multi handle's
// connection pool reuses an idle keep-alive connection to the host when one is available.
Result<void> Downloader::launch(std::uint64_t start, std::uint64_t end, int restarts) {
  auto c = make_connection(start, end, true);
  if (!c) {
    return make_error(c.error());
  }
  (*c)->restarts = restarts;
  curl_multi_add_handle(multi_, (*c)->easy.get());
  connections_.push_back(std::move(*c));
  return {};
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
  return launch(middle, old_end, largest->restarts);
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
  return launch(position, old_end, c.restarts + 1);
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

void Downloader::record_failure(Connection &c) {
  if (c.error) {
    failure_ = c.error;
  } else if (net::is_http(c.easy.get()) && net::response_code(c.easy.get()) >= 400) {
    failure_ = net::http_status_error(net::response_code(c.easy.get()));
  } else {
    failure_ = net::transfer_error(c.result, c.storage.error_buffer);
  }
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
    curl_multi_add_handle(multi_, c->easy.get());
  }
  last_speed_check_ = Clock::now();

  Outcome outcome = Outcome::Done;
  while (finished_ < connections_.size()) {
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
      const bool ok = c->result == CURLE_OK || (c->result == CURLE_WRITE_ERROR && c->reached_end);
      if (!ok) {
        if (outcome == Outcome::Done) {
          outcome = range_ignored_ ? Outcome::RangeIgnored : Outcome::Failed;
          if (outcome == Outcome::Failed) {
            record_failure(*c);
          }
        }
      } else {
        if (auto released = release(*c); !released && outcome == Outcome::Done) {
          failure_ = released.error();
          outcome = Outcome::Failed;
        }
        if (dynamic() && options_.split_when_idle && outcome == Outcome::Done) {
          if (auto split = split_for_idle_connection(); !split) {
            failure_ = split.error();
            outcome = Outcome::Failed;
          }
        }
      }
    }
    if (outcome != Outcome::Done) {
      break; // per-segment retries come in step 8
    }
    if (stop_.stop_requested()) {
      outcome = Outcome::Cancelled;
      break;
    }
    const auto now = Clock::now();
    if (dynamic() && options_.replace_slow_connections) {
      if (auto replaced = replace_slow_connections(now); !replaced) {
        failure_ = replaced.error();
        outcome = Outcome::Failed;
        break;
      }
    }
    report_progress(false);
    // While data keeps arriving, go straight back to reading; wait only when a round was idle.
    if (received_ == received_before && finished_ < connections_.size()) {
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

// Writes out whatever arrived, so a later resume (step 7) can keep it.
void Downloader::keep_partial() {
  if (!file_.is_open()) {
    return;
  }
  for (auto &c : connections_) {
    (void)flush(*c);
  }
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
  if (options_.probe) {
    info_ = options_.probe;
  } else if (options_.request.method != "POST") {
    auto probed = net::probe(options_.request);
    if (!probed) {
      return make_error(probed.error());
    }
    info_ = std::move(*probed);
  }

  // Plan: equal ranges when the server supports them, else one whole-file transfer.
  int segments = 1;
  if (info_ && info_->accepts_ranges && info_->size && options_.min_segment_size > 0) {
    const std::uint64_t by_size = *info_->size / options_.min_segment_size;
    segments = static_cast<int>(std::min<std::uint64_t>(options_.connections, by_size));
    segments = std::max(segments, 1);
  }
  write_buffer_size_ = std::clamp(kTotalWriteBuffer / static_cast<std::size_t>(segments),
                                  kMinWriteBuffer, kMaxWriteBuffer);

  if (segments > 1) {
    segmented_ = true;
    if (auto ok = prepare_file(); !ok) {
      return make_error(ok.error());
    }
    const std::uint64_t size = *info_->size;
    const std::uint64_t chunk = size / static_cast<std::uint64_t>(segments);
    for (int i = 0; i < segments; ++i) {
      const std::uint64_t start = chunk * static_cast<std::uint64_t>(i);
      const std::uint64_t end = (i == segments - 1) ? size - 1 : start + chunk - 1;
      auto c = make_connection(start, end, true);
      if (!c) {
        return make_error(c.error());
      }
      connections_.push_back(std::move(*c));
    }
  } else {
    auto c = make_connection(0, std::nullopt, false);
    if (!c) {
      return make_error(c.error());
    }
    connections_.push_back(std::move(*c));
  }

  meter_.add(0);
  last_report_ = Clock::now();
  Outcome outcome = transfer();

  if (outcome == Outcome::RangeIgnored) {
    // Start over with one whole-file transfer into the same (already sized) .part file.
    connections_.clear();
    received_ = 0;
    range_ignored_ = false;
    segmented_ = false;
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
    return make_error(failure_.value_or(Error{ErrorCode::Network, "download failed", 0}));
  }

  if (!file_.is_open()) { // empty body: no write callback ran
    connections_.front()->started = false;
    if (auto ok = on_response(*connections_.front()); !ok) {
      return make_error(ok.error());
    }
  }
  std::uint64_t total = 0;
  for (auto &c : connections_) {
    if (auto ok = flush(*c); !ok) {
      return make_error(ok.error());
    }
    if (const auto len = c->length(); len && c->written != *len) {
      file_.close();
      return make_error(ErrorCode::Network, "connection ended early: received " +
                                                std::to_string(c->written) + " of " +
                                                std::to_string(*len) + " bytes");
    }
    total += c->written;
  }
  if (info_->size && total != *info_->size) {
    file_.close();
    return make_error(ErrorCode::Network, "download incomplete: received " + std::to_string(total) +
                                              " of " + std::to_string(*info_->size) + " bytes");
  }
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

  if (!info_->size) {
    info_->size = total;
  }
  report_progress(true);
  return DownloadResult{final_path_, total, segments, splits_, restarts_, *info_};
}

} // namespace

Result<DownloadResult> download_file(const DownloadOptions &options, std::stop_token stop) {
  Downloader download(options, std::move(stop));
  return download.run();
}

} // namespace lanes::core::engine
