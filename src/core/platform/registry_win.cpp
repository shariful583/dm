// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/registry.hpp"

#include <windows.h>

#include "core/platform/win/win32.hpp"

namespace lanes::core::platform {

namespace {

// nullptr = the key's default value.
const wchar_t *value_name(const std::wstring &name) {
  return name.empty() ? nullptr : name.c_str();
}

Error registry_error(LSTATUS rc, const char *what, const std::string &key) {
  return win::win32_error(static_cast<DWORD>(rc), std::string(what) + " registry key HKCU\\" + key);
}

} // namespace

Result<void> set_user_value(const std::string &key, const std::string &name,
                            const std::string &value) {
  const std::wstring k = win::widen(key);
  const std::wstring n = win::widen(name);
  const std::wstring v = win::widen(value);
  const LSTATUS rc =
      ::RegSetKeyValueW(HKEY_CURRENT_USER, k.c_str(), value_name(n), REG_SZ, v.c_str(),
                        static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
  if (rc != ERROR_SUCCESS) {
    return make_error(registry_error(rc, "write", key));
  }
  return {};
}

Result<std::optional<std::string>> user_value(const std::string &key, const std::string &name) {
  const std::wstring k = win::widen(key);
  const std::wstring n = win::widen(name);
  DWORD bytes = 0;
  LSTATUS rc = ::RegGetValueW(HKEY_CURRENT_USER, k.c_str(), value_name(n), RRF_RT_REG_SZ, nullptr,
                              nullptr, &bytes);
  if (rc == ERROR_FILE_NOT_FOUND) {
    return std::optional<std::string>();
  }
  if (rc != ERROR_SUCCESS) {
    return make_error(registry_error(rc, "read", key));
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  rc = ::RegGetValueW(HKEY_CURRENT_USER, k.c_str(), value_name(n), RRF_RT_REG_SZ, nullptr,
                      value.data(), &bytes);
  if (rc != ERROR_SUCCESS) {
    return make_error(registry_error(rc, "read", key));
  }
  value.resize(bytes / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return std::optional<std::string>(win::narrow(value));
}

Result<void> delete_user_value(const std::string &key, const std::string &name) {
  const std::wstring k = win::widen(key);
  const std::wstring n = win::widen(name);
  const LSTATUS rc = ::RegDeleteKeyValueW(HKEY_CURRENT_USER, k.c_str(), value_name(n));
  if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
    return make_error(registry_error(rc, "delete a value of", key));
  }
  return {};
}

Result<void> set_user_default_value(const std::string &key, const std::string &value) {
  return set_user_value(key, {}, value);
}

Result<std::optional<std::string>> user_default_value(const std::string &key) {
  return user_value(key, {});
}

Result<void> delete_user_key(const std::string &key) {
  const std::wstring k = win::widen(key);
  const LSTATUS rc = ::RegDeleteTreeW(HKEY_CURRENT_USER, k.c_str());
  if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
    return make_error(registry_error(rc, "delete", key));
  }
  if (rc == ERROR_SUCCESS) {
    (void)::RegDeleteKeyW(HKEY_CURRENT_USER, k.c_str()); // RegDeleteTree keeps the key itself
  }
  return {};
}

} // namespace lanes::core::platform
