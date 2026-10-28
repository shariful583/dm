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

class Downloader;

// One transfer: a byte range of the file (or the whole file) over its own connection. Owned by
// the Downloader and touched only on its event-loop thread, so no locking is needed; a later
// re-split (step 6) shrinks `end` on the same thread and on_write honours it on the next chunk.
struct Connection {
  Downloader *owner = nullptr;
  std::uint64_t start = 0;          // file offset of the first byte
  std::optional<std::uint64_t> end; // inclusive; nullopt = until the server stops
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

  std::uint64_t received() const { return written + buffer.size(); }
  std::optional<std::uint64_t> length() const {
    return end ? std::optional(*end - start + 1) : std::nullopt;
  }
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
  Outcome transfer();
  void keep_partial();
  void report_progress(bool force);

  const DownloadOptions &options_;
  std::stop_token stop_;

  std::optional<net::ProbeResult> info_;
  std::vector<std::unique_ptr<Connection>> connections_;
  std::size_t write_buffer_size_ = kMaxWriteBuffer;
  bool range_ignored_ = false;
  std::optional<Error> failure_;

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

  // Never write past the segment's end: bytes beyond it belong to another connection.
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

// Runs every connection to completion (or the first failure / cancel) on one multi handle.
Downloader::Outcome Downloader::transfer() {
  auto multi = net::make_multi();
  curl_multi_setopt(multi.get(), CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_NOTHING));
  curl_multi_setopt(multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS,
                    static_cast<long>(options_.max_connections_per_host));
  for (auto &c : connections_) {
    curl_multi_add_handle(multi.get(), c->easy.get());
  }

  Outcome outcome = Outcome::Done;
  std::size_t finished = 0;
  while (finished < connections_.size()) {
    int running = 0;
    const std::uint64_t received_before = received_;
    curl_multi_perform(multi.get(), &running);

    int queued = 0;
    while (CURLMsg *msg = curl_multi_info_read(multi.get(), &queued)) {
      if (msg->msg != CURLMSG_DONE) {
        continue;
      }
      Connection *c = nullptr;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &c);
      c->finished = true;
      c->result = msg->data.result;
      ++finished;
      const bool ok = c->result == CURLE_OK || (c->result == CURLE_WRITE_ERROR && c->reached_end);
      if (!ok && outcome == Outcome::Done) {
        if (range_ignored_) {
          outcome = Outcome::RangeIgnored;
        } else {
          outcome = Outcome::Failed;
          if (c->error) {
            failure_ = c->error;
          } else if (net::is_http(c->easy.get()) && net::response_code(c->easy.get()) >= 400) {
            failure_ = net::http_status_error(net::response_code(c->easy.get()));
          } else {
            failure_ = net::transfer_error(c->result, c->storage.error_buffer);
          }
        }
      }
    }
    if (outcome != Outcome::Done) {
      break; // retries per segment come in step 8
    }
    if (stop_.stop_requested()) {
      outcome = Outcome::Cancelled;
      break;
    }
    report_progress(false);
    // While data keeps arriving, go straight back to reading; wait only when a round was idle.
    if (received_ == received_before && finished < connections_.size()) {
      curl_multi_poll(multi.get(), nullptr, 0, static_cast<int>(kPollTimeout.count()), nullptr);
    }
  }
  for (auto &c : connections_) {
    curl_multi_remove_handle(multi.get(), c->easy.get());
  }
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
    p.segments.push_back(SegmentProgress{c->start, c->end, c->received()});
  }
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
  return DownloadResult{final_path_, total, static_cast<int>(connections_.size()), *info_};
}

} // namespace

Result<DownloadResult> download_file(const DownloadOptions &options, std::stop_token stop) {
  Downloader download(options, std::move(stop));
  return download.run();
}

} // namespace lanes::core::engine
