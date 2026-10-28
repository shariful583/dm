// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/download.hpp"

#include "core/engine/speed_meter.hpp"
#include "core/net/curl.hpp"
#include "core/platform/file.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"

namespace lanes::core::engine {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// Bytes collected in memory before one positional write. Measured on a 1 GiB loopback transfer:
// 4 MiB gave ~450-500 MB/s on one connection; 1 MiB and smaller were slower and less stable.
constexpr std::size_t kWriteBufferSize = 4 << 20;

// Upper bound on one idle wait. curl_multi_poll on Windows sometimes misses data that is already
// waiting in the socket and sleeps the whole timeout (measured: 100 ms waits took up to 75 % of a
// 1 GiB transfer), so the timeout is kept short; it also bounds cancel latency.
constexpr auto kPollTimeout = std::chrono::milliseconds(10);

class SingleDownload {
public:
  SingleDownload(const DownloadOptions &options, std::stop_token stop)
      : options_(options), stop_(std::move(stop)) {}

  Result<DownloadResult> run();

private:
  static std::size_t on_write(char *data, std::size_t size, std::size_t count, void *user);

  Result<void> begin();
  Result<void> flush_buffer();
  void report_progress(bool force);

  const DownloadOptions &options_;
  std::stop_token stop_;

  std::optional<net::ProbeResult> info_;
  net::EasyHandle easy_;
  net::RequestStorage storage_;

  bool started_ = false;
  fs::path final_path_;
  fs::path part_path_;
  platform::File file_;
  bool sparse_ = false;
  std::string buffer_;
  std::uint64_t written_ = 0;  // bytes handed to the file
  std::uint64_t received_ = 0; // bytes received from the network
  std::optional<Error> write_error_;

  SpeedMeter meter_;
  Clock::time_point last_report_{};
};

std::size_t SingleDownload::on_write(char *data, std::size_t size, std::size_t count, void *user) {
  auto *self = static_cast<SingleDownload *>(user);
  const std::size_t length = size * count;
  if (!self->started_) {
    if (auto ok = self->begin(); !ok) {
      self->write_error_ = ok.error();
      return CURL_WRITEFUNC_ERROR;
    }
  }
  self->buffer_.append(data, length);
  self->received_ += length;
  if (self->buffer_.size() >= kWriteBufferSize) {
    if (auto ok = self->flush_buffer(); !ok) {
      self->write_error_ = ok.error();
      return CURL_WRITEFUNC_ERROR;
    }
  }
  return length;
}

// Runs when the response headers are in: validates the response and opens the .part file.
Result<void> SingleDownload::begin() {
  started_ = true;
  CURL *easy = easy_.get();
  if (net::is_http(easy)) {
    const long status = net::response_code(easy);
    if (status < 200 || status >= 300) {
      return make_error(net::http_status_error(status));
    }
  }

  const net::ProbeResult response = net::probe_result_from_response(easy);
  if (!info_) {
    info_ = response; // POST / unprobed: the response is the probe
  } else if (info_->size && response.size && *info_->size != *response.size) {
    return make_error(ErrorCode::Http,
                      "the file changed on the server since it was checked (size " +
                          std::to_string(*info_->size) + " → " + std::to_string(*response.size) +
                          ")");
  }

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
  buffer_.reserve(kWriteBufferSize + CURL_MAX_READ_SIZE);
  return {};
}

Result<void> SingleDownload::flush_buffer() {
  if (buffer_.empty()) {
    return {};
  }
  if (auto ok = file_.write_at(written_, buffer_); !ok) {
    return ok;
  }
  written_ += buffer_.size();
  buffer_.clear();
  return {};
}

void SingleDownload::report_progress(bool force) {
  const auto now = Clock::now();
  if (!force && now - last_report_ < options_.progress_interval) {
    return;
  }
  last_report_ = now;
  meter_.add(received_, now);
  if (options_.on_progress) {
    options_.on_progress(
        DownloadProgress{received_, info_ ? info_->size : std::nullopt, meter_.rate()});
  }
}

Result<DownloadResult> SingleDownload::run() {
  const bool is_post = options_.request.method == "POST";
  if (options_.probe) {
    info_ = options_.probe;
  } else if (!is_post) {
    auto probed = net::probe(options_.request);
    if (!probed) {
      return make_error(probed.error());
    }
    info_ = std::move(*probed);
  }

  easy_ = net::make_easy();
  if (auto ok = net::apply_request(easy_.get(), options_.request, storage_); !ok) {
    return make_error(ok.error());
  }
  curl_easy_setopt(easy_.get(), CURLOPT_WRITEFUNCTION, &SingleDownload::on_write);
  curl_easy_setopt(easy_.get(), CURLOPT_WRITEDATA, this);

  auto multi = net::make_multi();
  curl_multi_add_handle(multi.get(), easy_.get());
  meter_.add(0);
  last_report_ = Clock::now();

  CURLcode result = CURLE_OK;
  bool done = false;
  bool cancelled = false;
  while (!done) {
    int running = 0;
    const std::uint64_t received_before = received_;
    curl_multi_perform(multi.get(), &running);
    int queued = 0;
    while (CURLMsg *msg = curl_multi_info_read(multi.get(), &queued)) {
      if (msg->msg == CURLMSG_DONE) {
        result = msg->data.result;
        done = true;
      }
    }
    if (done) {
      break;
    }
    if (stop_.stop_requested()) {
      cancelled = true;
      break;
    }
    report_progress(false);
    // While data keeps arriving, go straight back to reading; wait only when a round was idle.
    if (received_ == received_before) {
      curl_multi_poll(multi.get(), nullptr, 0, static_cast<int>(kPollTimeout.count()), nullptr);
    }
  }
  curl_multi_remove_handle(multi.get(), easy_.get());

  // Keep what arrived, for resuming (step 7).
  const auto keep_partial = [&]() {
    if (file_.is_open()) {
      (void)flush_buffer();
      file_.close();
    }
  };

  if (cancelled) {
    keep_partial();
    return make_error(ErrorCode::Cancelled, "download cancelled");
  }
  if (result != CURLE_OK) {
    keep_partial();
    if (write_error_) {
      return make_error(*write_error_);
    }
    if (net::is_http(easy_.get()) && net::response_code(easy_.get()) >= 400) {
      return make_error(net::http_status_error(net::response_code(easy_.get())));
    }
    return make_error(net::transfer_error(result, storage_.error_buffer));
  }

  if (!started_) { // empty body: no write callback ran
    if (auto ok = begin(); !ok) {
      return make_error(ok.error());
    }
  }
  if (auto ok = flush_buffer(); !ok) {
    return make_error(ok.error());
  }
  if (info_->size && written_ != *info_->size) {
    file_.close();
    return make_error(ErrorCode::Network, "connection ended early: received " +
                                              std::to_string(written_) + " of " +
                                              std::to_string(*info_->size) + " bytes");
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
    info_->size = written_;
  }
  report_progress(true);
  return DownloadResult{final_path_, written_, *info_};
}

} // namespace

Result<DownloadResult> download_file(const DownloadOptions &options, std::stop_token stop) {
  SingleDownload download(options, std::move(stop));
  return download.run();
}

} // namespace lanes::core::engine
