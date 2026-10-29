// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <random>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/platform/ipc.hpp"

using namespace lanes::core;
using namespace lanes::core::platform;
using namespace std::chrono_literals;

namespace {

std::string unique_name() {
  std::random_device rd;
  return "lanes-test-" + std::to_string(rd()) + std::to_string(rd());
}

// Echo server: answers each message with "echo:" + message.
struct EchoServer {
  std::string name = unique_name();
  std::jthread thread;
  EchoServer() {
    auto server = PipeServer::create(name);
    REQUIRE(server);
    thread = std::jthread([s = std::move(*server)](std::stop_token stop) mutable {
      std::vector<std::jthread> clients;
      for (;;) {
        auto conn = s.accept(stop);
        if (!conn) {
          return;
        }
        clients.emplace_back([c = std::move(*conn)](std::stop_token st) mutable {
          for (;;) {
            auto msg = c.read(st);
            if (!msg || !c.write("echo:" + *msg)) {
              return;
            }
          }
        });
      }
    });
  }
};

} // namespace

TEST_CASE("pipe messages round-trip", "[platform][ipc]") {
  EchoServer server;
  auto client = connect_pipe(server.name, 2s);
  REQUIRE(client);
  REQUIRE(client->write("hello"));
  CHECK(*client->read() == "echo:hello");
  REQUIRE(client->write(""));
  CHECK(*client->read() == "echo:");

  const std::string big(3u << 20, 'x'); // several pipe buffers
  REQUIRE(client->write(big));
  const auto back = client->read();
  REQUIRE(back);
  CHECK(back->size() == big.size() + 5);
}

TEST_CASE("several clients at once", "[platform][ipc]") {
  EchoServer server;
  auto a = connect_pipe(server.name, 2s);
  auto b = connect_pipe(server.name, 2s);
  REQUIRE((a && b));
  REQUIRE(b->write("b"));
  REQUIRE(a->write("a"));
  CHECK(*a->read() == "echo:a");
  CHECK(*b->read() == "echo:b");
}

TEST_CASE("pipe errors", "[platform][ipc]") {
  const auto none = connect_pipe(unique_name(), 500ms);
  REQUIRE_FALSE(none);
  CHECK(none.error().code == ErrorCode::NotFound);

  EchoServer server;
  const auto second = PipeServer::create(server.name); // one engine per user
  REQUIRE_FALSE(second);
  CHECK(second.error().code == ErrorCode::AlreadyExists);

  auto client = connect_pipe(server.name, 2s);
  REQUIRE(client);
  CHECK_FALSE(client->write(std::string(kMaxMessageSize + 1, 'x')));
}

TEST_CASE("a read can be cancelled", "[platform][ipc]") {
  EchoServer server;
  auto client = connect_pipe(server.name, 2s);
  REQUIRE(client);
  std::stop_source stop;
  std::jthread canceller([&] {
    std::this_thread::sleep_for(200ms);
    stop.request_stop();
  });
  const auto start = std::chrono::steady_clock::now();
  const auto r = client->read(stop.get_token()); // nothing was sent: waits until cancelled
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::Cancelled);
  CHECK(std::chrono::steady_clock::now() - start < 2s);
}
