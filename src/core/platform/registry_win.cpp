// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/registry.hpp"

#include <windows.h>

#include "core/platform/win/win32.hpp"

namespace lanes::core::platform {

Result<void> set_user_default_value(const std::string &key, const std::string &value) {
  const std::wstring k = win::widen(key);
  const std::wstring v = win::widen(value);
  const LSTATUS rc = ::RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), nullptr, REG_SZ, v.c_str(),
                                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
  if (rc != ERROR_SUCCESS) {
    return make_error(win::win32_error(static_cast<DWORD>(rc), "write registry key HKCU\\" + key));
  }
  return {};
}

Result<std::optional<std::string>> user_default_value(const std::string &key) {
  const std::wstring k = win::widen(key);
  DWORD bytes = 0;
  LSTATUS rc = ::RegGetValueW(HKEY_CURRENT_USER, k.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                              nullptr, &bytes);
  if (rc == ERROR_FILE_NOT_FOUND) {
    return std::optional<std::string>();
  }
  if (rc != ERROR_SUCCESS) {
    return make_error(win::win32_error(static_cast<DWORD>(rc), "read registry key HKCU\\" + key));
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  rc = ::RegGetValueW(HKEY_CURRENT_USER, k.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value.data(),
                      &bytes);
  if (rc != ERROR_SUCCESS) {
    return make_error(win::win32_error(static_cast<DWORD>(rc), "read registry key HKCU\\" + key));
  }
  value.resize(bytes / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return std::optional<std::string>(win::narrow(value));
}

Result<void> delete_user_key(const std::string &key) {
  const std::wstring k = win::widen(key);
  const LSTATUS rc = ::RegDeleteTreeW(HKEY_CURRENT_USER, k.c_str());
  if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
    return make_error(win::win32_error(static_cast<DWORD>(rc), "delete registry key HKCU\\" + key));
  }
  if (rc == ERROR_SUCCESS) {
    (void)::RegDeleteKeyW(HKEY_CURRENT_USER, k.c_str()); // RegDeleteTree keeps the key itself
  }
  return {};
}

} // namespace lanes::core::platform
