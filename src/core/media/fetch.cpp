// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/fetch.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <system_error>
#include <thread>

#include <openssl/evp.h>

#include "core/media/http.hpp"
#include "core/net/curl.hpp"
#include "core/platform/file.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"

namespace lanes::core::media {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr auto kPollTimeout = std::chrono::milliseconds(10);

struct Job {
  std::size_t track = 0;
  int index = -1; // -1 = initialization section
  const Part *part = nullptr;
  fs::path final_path;
  int attempts = 0;
  Clock::time_point not_before{};
};

struct Active {
  Job job;
  net::EasyHandle easy;
  net::RequestStorage storage;
  platform::File file;
  fs::path tmp_path;
  std::uint64_t written = 0;
  std::optional<Error> error;
  bool paused = false;
  std::vector<std::shared_ptr<limiter::RateLimiter>> *limiters = nullptr;
  std::uint64_t *total_bytes = nullptr;
};

fs::path part_file(const fs::path &folder, std::size_t track, int index) {
  char name[48];
  if (index < 0) {
    std::snprintf(name, sizeof(name), "t%zu-init.bin", track);
  } else {
    std::snprintf(name, sizeof(name), "t%zu-%06d.bin", track, index);
  }
  return folder / name;
}

bool tokens_available(const std::vector<std::shared_ptr<limiter::RateLimiter>> &limiters) {
  return std::all_of(limiters.begin(), limiters.end(),
                     [](const auto &l) { return l->available(); });
}

std::size_t on_write(char *data, std::size_t size, std::size_t count, void *user) {
  auto &a = *static_cast<Active *>(user);
  const std::size_t n = size * count;
  if (net::is_http(a.easy.get())) {
    const long status = net::response_code(a.easy.get());
    if (status >= 400 || (a.job.part->range && status != 206)) {
      a.error = status >= 400 ? net::http_status_error(status)
                              : Error{ErrorCode::Http, "server ignored the byte range", 0};
      return CURL_WRITEFUNC_ERROR;
    }
  }
  if (a.limiters != nullptr && !a.limiters->empty() && !tokens_available(*a.limiters)) {
    a.paused = true;
    return CURL_WRITEFUNC_PAUSE;
  }
  if (auto ok = a.file.write_at(a.written, std::string_view(data, n)); !ok) {
    a.error = ok.error();
    return CURL_WRITEFUNC_ERROR;
  }
  a.written += n;
  *a.total_bytes += n;
  if (a.limiters != nullptr) {
    for (const auto &l : *a.limiters) {
      l->consume(n);
    }
  }
  return n;
}

Result<std::string> aes128_decrypt(const std::string &key, const std::string &iv,
                                   const std::string &cipher) {
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                      &EVP_CIPHER_CTX_free);
  std::string out(cipher.size() + 16, '\0');
  int n1 = 0;
  int n2 = 0;
  auto *o = reinterpret_cast<unsigned char *>(out.data());
  if (!ctx ||
      EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr,
                         reinterpret_cast<const unsigned char *>(key.data()),
                         reinterpret_cast<const unsigned char *>(iv.data())) != 1 ||
      EVP_DecryptUpdate(ctx.get(), o, &n1, reinterpret_cast<const unsigned char *>(cipher.data()),
                        static_cast<int>(cipher.size())) != 1 ||
      EVP_DecryptFinal_ex(ctx.get(), o + n1, &n2) != 1) {
    return make_error(ErrorCode::InvalidArgument,
                      "a stream segment could not be decrypted (wrong key?)");
  }
  out.resize(static_cast<std::size_t>(n1 + n2));
  return out;
}

// IV for a segment without an explicit IV: its media sequence number, big-endian (RFC 8216).
std::string sequence_iv(std::uint64_t sequence) {
  std::string iv(16, '\0');
  for (int i = 0; i < 8; ++i) {
    iv[15 - i] = static_cast<char>((sequence >> (8 * i)) & 0xFF);
  }
  return iv;
}

} // namespace

Result<std::vector<std::vector<fs::path>>> fetch_tracks(const std::vector<Track> &tracks,
                                                        const fs::path &folder,
                                                        const FetchOptions &options,
                                                        std::stop_token stop) {
  if (auto ok = platform::create_directories(folder); !ok) {
    return make_error(ok.error());
  }

  // Keys first (16 bytes each), once per URI.
  std::vector<std::vector<std::string>> keys(tracks.size());
  std::map<std::string, std::string> key_cache;
  for (std::size_t t = 0; t < tracks.size(); ++t) {
    for (const auto &k : tracks[t].keys) {
      if (!key_cache.contains(k.uri)) {
        net::RequestOptions request = options.request;
        request.url = k.uri;
        auto fetched = fetch_small(request, 1024);
        if (!fetched) {
          return make_error(fetched.error());
        }
        if (fetched->body.size() != 16) {
          return make_error(ErrorCode::InvalidArgument,
                            "the stream's AES-128 key is not 16 bytes (DRM or wrong key URL)");
        }
        key_cache[k.uri] = fetched->body;
      }
      keys[t].push_back(key_cache[k.uri]);
    }
  }

  // Every part, in play order per track; parts already on disk are done.
  std::vector<std::vector<fs::path>> files(tracks.size());
  std::deque<Job> pending;
  FetchProgress progress;
  std::error_code ec;
  for (std::size_t t = 0; t < tracks.size(); ++t) {
    const auto add = [&](const Part &part, int index) {
      Job job{t, index, &part, part_file(folder, t, index)};
      files[t].push_back(job.final_path);
      ++progress.parts_total;
      if (fs::exists(platform::to_long_path(job.final_path), ec)) {
        ++progress.parts_done;
        progress.bytes += fs::file_size(platform::to_long_path(job.final_path), ec);
      } else {
        pending.push_back(job);
      }
    };
    if (tracks[t].init) {
      add(*tracks[t].init, -1);
    }
    for (std::size_t i = 0; i < tracks[t].parts.size(); ++i) {
      add(tracks[t].parts[i], static_cast<int>(i));
    }
  }

  auto limiters = options.limiters;
  std::erase(limiters, nullptr);
  auto multi = net::make_multi();
  std::map<CURL *, std::unique_ptr<Active>> active;
  auto last_report = Clock::now();
  const auto report = [&](bool force) {
    if (options.on_progress && (force || Clock::now() - last_report >= options.progress_interval)) {
      last_report = Clock::now();
      options.on_progress(progress);
    }
  };

  const auto start_job = [&](Job job) -> Result<void> {
    auto a = std::make_unique<Active>();
    a->job = std::move(job);
    a->limiters = &limiters;
    a->total_bytes = &progress.bytes;
    a->tmp_path = a->job.final_path;
    a->tmp_path += ".tmp";
    auto file = platform::File::open(a->tmp_path, platform::OpenMode::CreateAlways);
    if (!file) {
      return make_error(file.error());
    }
    a->file = std::move(*file);
    a->easy = net::make_easy();
    net::RequestOptions request = options.request;
    request.url = a->job.part->url;
    request.method = "GET";
    request.body.reset();
    if (auto ok = net::apply_request(a->easy.get(), request, a->storage); !ok) {
      return ok;
    }
    CURL *easy = a->easy.get();
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &on_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, a.get());
    if (const auto &r = a->job.part->range) {
      const std::string range =
          std::to_string(r->first) + "-" + std::to_string(r->first + r->second - 1);
      curl_easy_setopt(easy, CURLOPT_RANGE, range.c_str());
    }
    curl_multi_add_handle(multi.get(), easy);
    active.emplace(easy, std::move(a));
    return {};
  };

  // A finished part: decrypt if needed, then give it its final name.
  const auto complete = [&](Active &a) -> Result<void> {
    a.file.close();
    const Track &track = tracks[a.job.track];
    if (a.job.part->key >= 0) {
      auto cipher = platform::read_small_file(a.tmp_path, 1u << 30);
      if (!cipher) {
        return make_error(cipher.error());
      }
      const Key &key = track.keys[static_cast<std::size_t>(a.job.part->key)];
      const std::string iv = key.iv ? *key.iv : sequence_iv(a.job.part->sequence);
      auto plain =
          aes128_decrypt(keys[a.job.track][static_cast<std::size_t>(a.job.part->key)], iv, *cipher);
      if (!plain) {
        return make_error(plain.error());
      }
      progress.bytes -= cipher->size(); // count the decrypted size instead
      progress.bytes += plain->size();
      if (auto ok = platform::write_file_atomic(a.job.final_path, *plain); !ok) {
        return ok;
      }
      (void)platform::remove_file(a.tmp_path);
    } else if (auto ok = platform::atomic_replace(a.tmp_path, a.job.final_path); !ok) {
      return ok;
    }
    ++progress.parts_done;
    return {};
  };

  std::optional<Error> failure;
  const auto max_active = static_cast<std::size_t>(std::clamp(options.connections, 1, 32));
  while ((!pending.empty() || !active.empty()) && !failure) {
    if (stop.stop_requested()) {
      failure = Error{ErrorCode::Cancelled, "download cancelled", 0};
      break;
    }
    // Start parts while slots are free (retries wait for their delay).
    const auto now = Clock::now();
    for (auto it = pending.begin(); it != pending.end() && active.size() < max_active;) {
      if (it->not_before > now) {
        ++it;
        continue;
      }
      Job job = *it;
      it = pending.erase(it);
      if (auto ok = start_job(std::move(job)); !ok) {
        failure = ok.error();
        break;
      }
    }
    if (failure) {
      break;
    }
    if (!limiters.empty() && tokens_available(limiters)) {
      for (auto &[easy, a] : active) {
        if (a->paused) {
          a->paused = false;
          curl_easy_pause(easy, CURLPAUSE_CONT);
        }
      }
    }

    const std::uint64_t before = progress.bytes;
    int running = 0;
    curl_multi_perform(multi.get(), &running);
    int queued = 0;
    while (CURLMsg *msg = curl_multi_info_read(multi.get(), &queued)) {
      if (msg->msg != CURLMSG_DONE) {
        continue;
      }
      CURL *easy = msg->easy_handle;
      curl_multi_remove_handle(multi.get(), easy);
      auto node = active.extract(easy);
      Active &a = *node.mapped();
      std::optional<Error> error = a.error;
      if (!error && msg->data.result != CURLE_OK) {
        error = net::transfer_error(msg->data.result, a.storage.error_buffer);
      }
      if (!error && net::is_http(easy) && net::response_code(easy) >= 400) {
        error = net::http_status_error(net::response_code(easy));
      }
      if (!error) {
        if (auto ok = complete(a); !ok) {
          failure = ok.error();
        }
        continue;
      }
      // Failed: discard the partial part and retry it later, unless retries ran out or the
      // error is permanent (404, disk errors...).
      progress.bytes -= a.written;
      a.file.close();
      (void)platform::remove_file(a.tmp_path);
      const bool permanent =
          error->code == ErrorCode::Io || error->code == ErrorCode::NoSpace ||
          error->code == ErrorCode::AccessDenied ||
          (error->code == ErrorCode::Http && error->http_status != 0 && error->http_status != 408 &&
           error->http_status != 429 && error->http_status < 500);
      Job retry = a.job;
      if (permanent || ++retry.attempts > options.max_retries) {
        failure = *error;
        continue;
      }
      retry.not_before = Clock::now() + options.retry_delay * (1 << std::min(retry.attempts, 6));
      pending.push_back(std::move(retry));
    }
    report(false);
    if (progress.bytes == before) {
      curl_multi_poll(multi.get(), nullptr, 0, static_cast<int>(kPollTimeout.count()), nullptr);
    }
  }

  for (auto &[easy, a] : active) { // cancelled or failed: drop unfinished parts
    curl_multi_remove_handle(multi.get(), easy);
    a->file.close();
    (void)platform::remove_file(a->tmp_path);
  }
  report(true);
  if (failure) {
    return make_error(*failure);
  }
  return files;
}

} // namespace lanes::core::media
