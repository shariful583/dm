// SPDX-License-Identifier: GPL-3.0-or-later
// Internal Win32 helpers for core/platform. Not part of the public core API.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>

#include "core/error.hpp"

namespace lanes::core::platform::win {

// UTF-8 ↔ UTF-16. Invalid input sequences become U+FFFD. Thread-safe.
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// Error for Win32 error `code`, mapped to an ErrorCode, with the system message:
// "<operation> '<subject>': <message> (Win32 error <code>)". Thread-safe.
Error win32_error(DWORD code, std::string_view operation,
                  const std::filesystem::path &subject = {});
// Same, for GetLastError().
Error last_error(std::string_view operation, const std::filesystem::path &subject = {});

// Owning HANDLE; treats both nullptr and INVALID_HANDLE_VALUE as empty. Move-only, not
// synchronized.
class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
  ~UniqueHandle() { reset(); }
  UniqueHandle(UniqueHandle &&other) noexcept : h_(other.release()) {}
  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;

  HANDLE get() const noexcept { return h_; }
  explicit operator bool() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
  HANDLE release() noexcept { return std::exchange(h_, nullptr); }
  void reset(HANDLE h = nullptr) noexcept {
    if (*this) {
      ::CloseHandle(h_);
    }
    h_ = h;
  }

private:
  HANDLE h_ = nullptr;
};

} // namespace lanes::core::platform::win
