// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/system.hpp"

#include <cstdlib>
#include <memory>

#include "core/platform/path.hpp"
#include "core/platform/process.hpp"
#include "core/platform/win/win32.hpp"

#include <sddl.h>
#include <shlobj.h>

namespace lanes::core::platform {

namespace {

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR raw = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) {
    result = raw;
  }
  ::CoTaskMemFree(raw);
  return result;
}

} // namespace

std::optional<std::string> environment_variable(const std::string &name) {
  const std::wstring wname = win::widen(name);
  const DWORD n = ::GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (n == 0) {
    return std::nullopt;
  }
  std::wstring value(n, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(wname.c_str(), value.data(), n);
  value.resize(written);
  return win::narrow(value);
}

std::filesystem::path app_data_dir() {
  if (const auto custom = environment_variable("LANES_DATA_DIR"); custom && !custom->empty()) {
    return path_from_utf8(*custom);
  }
  return known_folder(FOLDERID_LocalAppData) / L"Lanes";
}

std::filesystem::path downloads_dir() { return known_folder(FOLDERID_Downloads); }

std::filesystem::path current_executable() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n < buffer.size()) {
      buffer.resize(n);
      return buffer;
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::uint32_t current_process_id() { return static_cast<std::uint32_t>(::GetCurrentProcessId()); }

Result<std::string> current_user_sid() {
  HANDLE raw = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) {
    return make_error(win::last_error("open process token"));
  }
  win::UniqueHandle token(raw);
  DWORD size = 0;
  ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  std::vector<std::byte> buffer(size);
  if (!::GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
    return make_error(win::last_error("read token user"));
  }
  const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
  LPWSTR text = nullptr;
  if (!::ConvertSidToStringSidW(user->User.Sid, &text)) {
    return make_error(win::last_error("format SID"));
  }
  std::string sid = win::narrow(text);
  ::LocalFree(text);
  return sid;
}

Result<std::uint32_t> spawn_detached(const std::filesystem::path &executable,
                                     const std::vector<std::string> &args) {
  std::wstring command_line = win::widen(windows_command_line(executable, args));
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // CREATE_BREAKAWAY_FROM_JOB: survive the caller's job object (e.g. a test runner's).
  constexpr DWORD kFlags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT;
  BOOL ok = ::CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                             kFlags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
  if (!ok && ::GetLastError() == ERROR_ACCESS_DENIED) {
    // The job does not allow breakaway: start inside it instead.
    ok = ::CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, kFlags,
                          nullptr, nullptr, &si, &pi);
  }
  if (!ok) {
    return make_error(win::last_error("start process", executable));
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return static_cast<std::uint32_t>(pi.dwProcessId);
}

} // namespace lanes::core::platform
