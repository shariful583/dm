// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/ipc.hpp"

#include <utility>

#include "core/platform/system.hpp"
#include "core/platform/win/win32.hpp"

#include <sddl.h>

namespace lanes::core::platform {

namespace {

constexpr auto kWaitSlice = std::chrono::milliseconds(100);
constexpr DWORD kBufferSize = 64 * 1024;

std::wstring full_name(const std::string &name) { return L"\\\\.\\pipe\\" + win::widen(name); }

bool is_disconnect(DWORD code) {
  return code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA || code == ERROR_PIPE_NOT_CONNECTED;
}

Error closed_error() {
  return Error{ErrorCode::NotFound, "the other side closed the connection", 0};
}

// Waits for an overlapped operation on `handle`, in slices so `stop` can cancel it.
// Returns the bytes transferred; Cancelled on stop.
Result<DWORD> finish_overlapped(HANDLE handle, OVERLAPPED &ov, std::stop_token stop,
                                std::string_view what) {
  for (;;) {
    const DWORD wait = ::WaitForSingleObject(ov.hEvent, static_cast<DWORD>(kWaitSlice.count()));
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (stop.stop_requested()) {
      ::CancelIoEx(handle, &ov);
      DWORD ignored = 0;
      ::GetOverlappedResult(handle, &ov, &ignored, TRUE); // let the cancel complete
      return make_error(ErrorCode::Cancelled, std::string(what) + " cancelled");
    }
  }
  DWORD transferred = 0;
  if (!::GetOverlappedResult(handle, &ov, &transferred, FALSE)) {
    const DWORD code = ::GetLastError();
    return make_error(is_disconnect(code) ? closed_error() : win::win32_error(code, what));
  }
  return transferred;
}

// Reads or writes exactly `size` bytes.
Result<void> transfer_exact(HANDLE handle, char *data, std::size_t size, bool writing,
                            std::stop_token stop) {
  win::UniqueHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) {
    return make_error(win::last_error("create event"));
  }
  std::size_t done = 0;
  while (done < size) {
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    ::ResetEvent(event.get());
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 20));
    DWORD n = 0;
    const BOOL ok = writing ? ::WriteFile(handle, data + done, chunk, &n, &ov)
                            : ::ReadFile(handle, data + done, chunk, &n, &ov);
    if (!ok) {
      const DWORD code = ::GetLastError();
      if (is_disconnect(code)) {
        return make_error(closed_error());
      }
      if (code != ERROR_IO_PENDING && code != ERROR_MORE_DATA) {
        return make_error(win::win32_error(code, writing ? "pipe write" : "pipe read"));
      }
      auto finished = finish_overlapped(handle, ov, stop, writing ? "pipe write" : "pipe read");
      if (!finished) {
        return make_error(finished.error());
      }
      n = *finished;
    }
    if (n == 0 && !writing) {
      return make_error(closed_error());
    }
    done += n;
  }
  return {};
}

// Security descriptor granting the current user (only) full access to the pipe.
Result<std::unique_ptr<void, decltype(&::LocalFree)>> owner_only_descriptor() {
  auto sid = current_user_sid();
  if (!sid) {
    return make_error(sid.error());
  }
  // D:P = protected DACL (no inheritance); one ACE: Allow Generic All to the user.
  const std::wstring sddl = L"D:P(A;;GA;;;" + win::widen(*sid) + L")";
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                              nullptr)) {
    return make_error(win::last_error("build pipe security"));
  }
  return std::unique_ptr<void, decltype(&::LocalFree)>(sd, &::LocalFree);
}

Result<HANDLE> create_instance(const std::string &name, bool first) {
  auto sd = owner_only_descriptor();
  if (!sd) {
    return make_error(sd.error());
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd->get(), FALSE};
  DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  if (first) {
    open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE; // fails if another process owns the name
  }
  const HANDLE h = ::CreateNamedPipeW(full_name(name).c_str(), open_mode,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                          PIPE_REJECT_REMOTE_CLIENTS,
                                      PIPE_UNLIMITED_INSTANCES, kBufferSize, kBufferSize, 0, &sa);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    if (first && (code == ERROR_ACCESS_DENIED || code == ERROR_PIPE_BUSY)) {
      return make_error(ErrorCode::AlreadyExists, "another Lanes engine is already running");
    }
    return make_error(win::win32_error(code, "create pipe"));
  }
  return h;
}

} // namespace

std::string default_pipe_name() {
  if (const auto custom = environment_variable("LANES_PIPE"); custom && !custom->empty()) {
    return *custom;
  }
  const auto sid = current_user_sid();
  return "lanes-" + (sid ? *sid : std::string("user"));
}

PipeConnection::~PipeConnection() { close(); }

PipeConnection::PipeConnection(PipeConnection &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

PipeConnection &PipeConnection::operator=(PipeConnection &&other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

void PipeConnection::close() noexcept {
  if (handle_ != nullptr) {
    ::CloseHandle(handle_);
    handle_ = nullptr;
  }
}

Result<std::string> PipeConnection::read(std::stop_token stop) {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::NotFound, "not connected");
  }
  unsigned char header[4] = {};
  if (auto ok = transfer_exact(handle_, reinterpret_cast<char *>(header), 4, false, stop); !ok) {
    return make_error(ok.error());
  }
  const std::uint32_t size =
      static_cast<std::uint32_t>(header[0]) | (static_cast<std::uint32_t>(header[1]) << 8) |
      (static_cast<std::uint32_t>(header[2]) << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
  if (size > kMaxMessageSize) {
    return make_error(ErrorCode::InvalidArgument, "message too large");
  }
  std::string message(size, '\0');
  if (auto ok = transfer_exact(handle_, message.data(), size, false, stop); !ok) {
    return make_error(ok.error());
  }
  return message;
}

Result<void> PipeConnection::write(std::string_view message) {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::NotFound, "not connected");
  }
  if (message.size() > kMaxMessageSize) {
    return make_error(ErrorCode::InvalidArgument, "message too large");
  }
  const auto size = static_cast<std::uint32_t>(message.size());
  std::string frame(4 + message.size(), '\0');
  for (int i = 0; i < 4; ++i) {
    frame[i] = static_cast<char>((size >> (8 * i)) & 0xFF);
  }
  frame.replace(4, message.size(), message);
  return transfer_exact(handle_, frame.data(), frame.size(), true, {});
}

Result<PipeServer> PipeServer::create(const std::string &name) {
  auto first = create_instance(name, true);
  if (!first) {
    return make_error(first.error());
  }
  PipeServer server;
  server.name_ = name;
  server.pending_ = *first;
  return server;
}

PipeServer::~PipeServer() {
  if (pending_ != nullptr) {
    ::CloseHandle(pending_);
  }
}

PipeServer::PipeServer(PipeServer &&other) noexcept
    : name_(std::move(other.name_)), pending_(std::exchange(other.pending_, nullptr)) {}

PipeServer &PipeServer::operator=(PipeServer &&other) noexcept {
  if (this != &other) {
    if (pending_ != nullptr) {
      ::CloseHandle(pending_);
    }
    name_ = std::move(other.name_);
    pending_ = std::exchange(other.pending_, nullptr);
  }
  return *this;
}

Result<PipeConnection> PipeServer::accept(std::stop_token stop) {
  if (pending_ == nullptr) {
    auto next = create_instance(name_, false);
    if (!next) {
      return make_error(next.error());
    }
    pending_ = *next;
  }
  win::UniqueHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
  OVERLAPPED ov{};
  ov.hEvent = event.get();
  if (!::ConnectNamedPipe(pending_, &ov)) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_IO_PENDING) {
      if (auto done = finish_overlapped(pending_, ov, stop, "wait for client"); !done) {
        return make_error(done.error());
      }
    } else if (code != ERROR_PIPE_CONNECTED) { // ERROR_PIPE_CONNECTED: a client was quicker
      return make_error(win::win32_error(code, "wait for client"));
    }
  }
  PipeConnection connection(std::exchange(pending_, nullptr));
  return connection;
}

Result<PipeConnection> connect_pipe(const std::string &name, std::chrono::milliseconds timeout) {
  const std::wstring path = full_name(name);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    // SECURITY_IDENTIFICATION: the server may identify, but not impersonate, the client.
    const HANDLE h = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      return PipeConnection(h);
    }
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND) {
      return make_error(ErrorCode::NotFound, "no Lanes engine is running");
    }
    if (code != ERROR_PIPE_BUSY) {
      return make_error(win::win32_error(code, "connect to the Lanes engine"));
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return make_error(ErrorCode::Timeout, "the Lanes engine is busy");
    }
    ::WaitNamedPipeW(path.c_str(), static_cast<DWORD>(left.count()));
  }
}

} // namespace lanes::core::platform
