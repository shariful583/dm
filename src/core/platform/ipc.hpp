// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace lanes::core::platform {

// Local inter-process messaging between the engine process and its front-ends.
// Windows: a named pipe `\\.\pipe\<name>` that only the current user can open, with remote
// clients rejected. Messages are length-prefixed (4-byte little-endian) byte strings.

inline constexpr std::size_t kMaxMessageSize = 4u << 20;

// "lanes-<user SID>"; the LANES_PIPE environment variable overrides it (tests). Thread-safe.
std::string default_pipe_name();

// One connected end. Move-only; read and write may be used from different threads.
class PipeConnection {
public:
  PipeConnection() = default;
  ~PipeConnection();
  PipeConnection(PipeConnection &&other) noexcept;
  PipeConnection &operator=(PipeConnection &&other) noexcept;
  PipeConnection(const PipeConnection &) = delete;
  PipeConnection &operator=(const PipeConnection &) = delete;

  // Next message. NotFound when the other end closed; Cancelled if `stop` is requested.
  Result<std::string> read(std::stop_token stop = {});
  Result<void> write(std::string_view message);
  void close() noexcept;

private:
  friend class PipeServer;
  friend Result<PipeConnection> connect_pipe(const std::string &, std::chrono::milliseconds);
  explicit PipeConnection(void *handle) : handle_(handle) {}
  void *handle_ = nullptr; // HANDLE, opened for overlapped I/O
};

// Listening end. Not thread-safe: one thread calls accept().
class PipeServer {
public:
  // AlreadyExists if another process already serves `name` (one engine per user).
  static Result<PipeServer> create(const std::string &name);

  PipeServer() = default;
  ~PipeServer();
  PipeServer(PipeServer &&other) noexcept;
  PipeServer &operator=(PipeServer &&other) noexcept;
  PipeServer(const PipeServer &) = delete;
  PipeServer &operator=(const PipeServer &) = delete;

  // Waits for the next client. Cancelled when `stop` is requested.
  Result<PipeConnection> accept(std::stop_token stop);

private:
  std::string name_;
  void *pending_ = nullptr; // HANDLE of the instance waiting for a client
};

// Connects to a server. NotFound if none serves `name`; Timeout if all instances stay busy.
Result<PipeConnection> connect_pipe(const std::string &name, std::chrono::milliseconds timeout);

} // namespace lanes::core::platform
