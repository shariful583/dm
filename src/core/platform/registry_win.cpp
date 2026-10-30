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

HKEY root_of(RegistryScope scope) {
  return scope == RegistryScope::Machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

Error registry_error(LSTATUS rc, const char *what, const std::string &key,
                     RegistryScope scope = RegistryScope::User) {
  const char *root = scope == RegistryScope::Machine ? "HKLM\\" : "HKCU\\";
  return win::win32_error(static_cast<DWORD>(rc),
                          std::string(what) + " registry key " + root + key);
}

Result<void> set_value(HKEY root, RegistryScope scope, const std::string &key,
                       const std::string &name, const std::string &value) {
  const std::wstring k = win::widen(key);
  const std::wstring n = win::widen(name);
  const std::wstring v = win::widen(value);
  const LSTATUS rc = ::RegSetKeyValueW(root, k.c_str(), value_name(n), REG_SZ, v.c_str(),
                                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
  if (rc != ERROR_SUCCESS) {
    return make_error(registry_error(rc, "write", key, scope));
  }
  return {};
}

Result<std::optional<std::string>> get_value(HKEY root, RegistryScope scope,
                                             const std::string &key, const std::string &name) {
  const std::wstring k = win::widen(key);
  const std::wstring n = win::widen(name);
  DWORD bytes = 0;
  LSTATUS rc =
      ::RegGetValueW(root, k.c_str(), value_name(n), RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
  if (rc == ERROR_FILE_NOT_FOUND) {
    return std::optional<std::string>();
  }
  if (rc != ERROR_SUCCESS) {
    return make_error(registry_error(rc, "read", key, scope));
  }
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  rc = ::RegGetValueW(root, k.c_str(), value_name(n), RRF_RT_REG_SZ, nullptr, value.data(),
                      &bytes);
  if (rc != ERROR_SUCCESS) {
    return make_error(registry_error(rc, "read", key, scope));
  }
  value.resize(bytes / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return std::optional<std::string>(win::narrow(value));
}

} // namespace

Result<void> set_user_value(const std::string &key, const std::string &name,
                            const std::string &value) {
  return set_value(HKEY_CURRENT_USER, RegistryScope::User, key, name, value);
}

Result<std::optional<std::string>> user_value(const std::string &key, const std::string &name) {
  return get_value(HKEY_CURRENT_USER, RegistryScope::User, key, name);
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

Result<void> set_user_default_value(const std::string &key, const std::string &value,
                                    RegistryScope scope) {
  return set_value(root_of(scope), scope, key, {}, value);
}

Result<std::optional<std::string>> user_default_value(const std::string &key,
                                                      RegistryScope scope) {
  return get_value(root_of(scope), scope, key, {});
}

Result<void> delete_user_key(const std::string &key, RegistryScope scope) {
  const HKEY root = root_of(scope);
  const std::wstring k = win::widen(key);
  const LSTATUS rc = ::RegDeleteTreeW(root, k.c_str());
  if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
    return make_error(registry_error(rc, "delete", key, scope));
  }
  if (rc == ERROR_SUCCESS) {
    (void)::RegDeleteKeyW(root, k.c_str()); // RegDeleteTree keeps the key itself
  }
  return {};
}

} // namespace lanes::core::platform
