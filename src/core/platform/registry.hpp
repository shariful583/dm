// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>

#include "core/error.hpp"

namespace lanes::core::platform {

// Per-user settings in the Windows registry (HKEY_CURRENT_USER), e.g. browser native-messaging
// host registration. Key paths are relative to HKCU with backslashes
// ("Software\\Google\\Chrome\\NativeMessagingHosts\\com.lanes.bridge"). Values are UTF-8.
// Thread-safe.

// Where a key lives: HKEY_CURRENT_USER, or HKEY_LOCAL_MACHINE for all users (writing there needs
// administrator rights).
enum class RegistryScope { User, Machine };

// Creates the key if needed and sets its default value.
Result<void> set_user_default_value(const std::string &key, const std::string &value,
                                    RegistryScope scope = RegistryScope::User);
// The key's default value; nullopt if the key or value does not exist.
Result<std::optional<std::string>> user_default_value(const std::string &key,
                                                      RegistryScope scope = RegistryScope::User);
// Deletes the key and everything below it; succeeds if it does not exist.
Result<void> delete_user_key(const std::string &key, RegistryScope scope = RegistryScope::User);

// Named string values (e.g. "Lanes" under ...\CurrentVersion\Run).
Result<void> set_user_value(const std::string &key, const std::string &name,
                            const std::string &value);
Result<std::optional<std::string>> user_value(const std::string &key, const std::string &name);
// Succeeds if the value does not exist.
Result<void> delete_user_value(const std::string &key, const std::string &name);

} // namespace lanes::core::platform
