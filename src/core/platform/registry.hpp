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

// Creates the key if needed and sets its default value.
Result<void> set_user_default_value(const std::string &key, const std::string &value);
// The key's default value; nullopt if the key or value does not exist.
Result<std::optional<std::string>> user_default_value(const std::string &key);
// Deletes the key and everything below it; succeeds if it does not exist.
Result<void> delete_user_key(const std::string &key);

} // namespace lanes::core::platform
