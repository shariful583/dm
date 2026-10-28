// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "hls_fixture.hpp"

namespace lanes::testserver {

// A generated file served by TestServer. Content is fill_content(seed, ...), never stored.
struct FileSpec {
  std::uint64_t size = 0;
  std::uint64_t seed = 1;
  std::string content_type = "application/octet-stream";
  std::string content_disposition;      // full header value; empty = header not sent
  std::optional<std::string> post_body; // set: only a POST with exactly this body gets the file
};

// Counters across all requests since start or reset_stats().
struct Stats {
  std::uint64_t requests = 0;       // every request, including HEAD and errors
  std::uint64_t body_responses = 0; // responses that started sending a file body
  std::uint64_t bytes_sent = 0;     // body bytes passed (or being passed) to the socket
  std::uint64_t connections = 0;    // distinct client TCP connections (remote address + port)
  int max_concurrent_bodies = 0;    // peak number of bodies being sent at the same time
};

// Local HTTP/1.1 test server for the download engine. Serves generated files and fixtures on
// 127.0.0.1 from a background thread pool.
//
// Every file supports HEAD, Range (single and multi), ETag, Last-Modified and
// Content-Disposition. Behaviour is changed per request with query parameters, so one server
// covers every scenario (all optional; counters are per path + query string):
//
//   no_range=1             ignore Range, always 200 with the full body, no Accept-Ranges
//   no_head=1              HEAD answers 405 (forces the GET Range: bytes=0-0 probe fallback)
//   no_accept_ranges=1     omit Accept-Ranges but still honour Range (common on real servers)
//   chunked=1              no Content-Length, chunked transfer encoding (unknown size)
//   rate=N                 throttle each body to N bytes/s
//   slow_requests=K        the first K bodies use slow_rate (bytes/s, default 64 KiB/s) instead
//   slow_rate=N
//   drop_after=N           close the connection after sending N body bytes ...
//   drop_times=K           ... for the first K bodies (default 1)
//   drop_prob=P            each 16 KiB chunk drops the connection with probability P (0..1)
//   status=C               answer status C (e.g. 429, 503) instead of the file ...
//   status_times=K         ... for the first K requests (default 1; 0 = always)
//   retry_after=S          add Retry-After: S to injected 429/503 answers
//   max_conns=N            more than N bodies in flight at once → 503
//   basic_auth=U:P         401 + WWW-Authenticate: Basic unless the request has these credentials
//
// Special paths:
//   /redirect?to=<path>&hops=N   302 chain of N hops (default 1) ending at /<path>
//   /signed/<path>?expires=T&sig=S   valid only before unix time T with sig from signed_url()
//   /echo                        200 with the request method, headers and body as text
//
// Thread safety: all public members may be called from any thread while the server runs.
class TestServer {
public:
  TestServer();
  ~TestServer(); // stops the server
  TestServer(const TestServer &) = delete;
  TestServer &operator=(const TestServer &) = delete;

  // Binds 127.0.0.1 on `port` (0 = any free port) and starts serving. Returns once listening.
  // Throws std::runtime_error if the port cannot be bound.
  void start(int port = 0);
  // Stops accepting, aborts in-flight bodies and joins the server thread. Idempotent.
  void stop();

  int port() const;
  // "http://127.0.0.1:<port>" + path (path must start with '/').
  std::string url(std::string_view path) const;

  // Registers a generated file at `path` (e.g. "/files/a.bin").
  void add_file(std::string path, FileSpec spec);
  // Registers static content at `path`.
  void add_static(std::string path, std::string body, std::string content_type);
  // Registers every file of `fixture` under `prefix` (e.g. "/hls").
  void add_hls_fixture(const std::string &prefix, const HlsFixture &fixture);

  // Changes the content of a generated file: new seed, new ETag and Last-Modified, same size
  // unless `new_size` is given. Simulates the file being replaced on the server.
  void change_content(const std::string &path, std::uint64_t new_seed,
                      std::optional<std::uint64_t> new_size = std::nullopt);

  // Full URL for /signed/<path> that stays valid for `ttl` (negative = already expired).
  std::string signed_url(std::string_view path, std::chrono::seconds ttl) const;

  Stats stats() const;
  void reset_stats();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lanes::testserver
