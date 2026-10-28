// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/win/win32.hpp"

#include "core/platform/path.hpp"

namespace lanes::core::platform::win {

namespace {

ErrorCode map_code(DWORD code) {
  switch (code) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_DRIVE:
  case ERROR_BAD_NETPATH:
  case ERROR_BAD_NET_NAME:
  case ERROR_DIRECTORY:
    return ErrorCode::NotFound;
  case ERROR_ACCESS_DENIED:
  case ERROR_SHARING_VIOLATION:
  case ERROR_LOCK_VIOLATION:
  case ERROR_WRITE_PROTECT:
    return ErrorCode::AccessDenied;
  case ERROR_FILE_EXISTS:
  case ERROR_ALREADY_EXISTS:
    return ErrorCode::AlreadyExists;
  case ERROR_DISK_FULL:
  case ERROR_HANDLE_DISK_FULL:
    return ErrorCode::NoSpace;
  case ERROR_FILE_TOO_LARGE:
    return ErrorCode::FileTooLarge;
  case ERROR_NOT_SUPPORTED:
  case ERROR_INVALID_FUNCTION:
    return ErrorCode::Unsupported;
  case ERROR_TIMEOUT:
  case WAIT_TIMEOUT:
    return ErrorCode::Timeout;
  default:
    return ErrorCode::Io;
  }
}

std::string system_message(DWORD code) {
  wchar_t *buffer = nullptr;
  const DWORD n = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
  if (n == 0 || buffer == nullptr) {
    return "unknown error";
  }
  std::wstring text(buffer, n);
  ::LocalFree(buffer);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' ||
                           text.back() == L'.')) {
    text.pop_back();
  }
  return narrow(text);
}

} // namespace

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int n =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string narrow(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n,
                        nullptr, nullptr);
  return out;
}

Error win32_error(DWORD code, std::string_view operation, const std::filesystem::path &subject) {
  std::string message(operation);
  if (!subject.empty()) {
    message += " '" + path_to_utf8(subject) + "'";
  }
  message += ": " + system_message(code) + " (Win32 error " + std::to_string(code) + ")";
  return Error{map_code(code), std::move(message)};
}

Error last_error(std::string_view operation, const std::filesystem::path &subject) {
  return win32_error(::GetLastError(), operation, subject);
}

} // namespace lanes::core::platform::win
