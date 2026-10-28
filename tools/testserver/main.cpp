// SPDX-License-Identifier: GPL-3.0-or-later
// lanes-testserver: the test server as a standalone program, for manual testing.
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>

#include "hls_fixture.hpp"
#include "test_server.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

} // namespace

int main(int argc, char **argv) {
  CLI::App app{"Lanes local test HTTP server (testing only)", "lanes-testserver"};
  int port = 8080;
  app.add_option("-p,--port", port, "Port on 127.0.0.1 (0 = any free port)")->capture_default_str();
  CLI11_PARSE(app, argc, argv);

  using lanes::testserver::FileSpec;
  constexpr std::uint64_t kMiB = 1024 * 1024;
  lanes::testserver::TestServer server;
  server.add_file("/files/1m.bin", {.size = kMiB, .seed = 1});
  server.add_file("/files/16m.bin", {.size = 16 * kMiB, .seed = 2});
  server.add_file("/files/256m.bin", {.size = 256 * kMiB, .seed = 3});
  server.add_file("/files/1g.bin", {.size = 1024 * kMiB, .seed = 4});
  server.add_file("/files/named", {.size = 4 * kMiB,
                                   .seed = 5,
                                   .content_disposition = "attachment; filename=\"report.pdf\"; "
                                                          "filename*=UTF-8''r%C3%A9sum%C3%A9.pdf"});
  server.add_file("/files/post.bin", {.size = 2 * kMiB, .seed = 6, .post_body = "token=lanes"});
  server.add_hls_fixture("/hls", lanes::testserver::make_hls_fixture("/hls"));

  try {
    server.start(port);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }

  std::cout << "Listening on " << server.url("/") << "\n\n"
            << "Files:  /files/1m.bin  /files/16m.bin  /files/256m.bin  /files/1g.bin\n"
            << "        /files/named (Content-Disposition)\n"
            << "        /files/post.bin (POST body: token=lanes)\n"
            << "HLS:    /hls/master.m3u8  /hls/plain/index.m3u8  /hls/fmp4/index.m3u8\n"
            << "Signed: " << server.signed_url("/files/16m.bin", std::chrono::hours(1)) << "\n"
            << "Faults: ?rate= ?no_range=1 ?no_head=1 ?chunked=1 ?drop_after= ?drop_prob=\n"
            << "        ?status=429&status_times= ?max_conns= ?slow_requests=&slow_rate=\n"
            << "        /redirect?to=files/1m.bin&hops=3\n"
            << "(see tools/testserver/test_server.hpp)  Ctrl+C to stop.\n";

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  server.stop();
  return 0;
}
