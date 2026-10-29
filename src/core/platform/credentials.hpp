// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::platform {

// Site logins: host → user name and password, kept in the operating system's
// credential store (Windows Credential Manager, per user), never in Lanes' database.
struct SiteLogin {
  std::string host; // lowercase host name, e.g. "files.example.com"
  std::string username;
  std::string password;
};

// Creates or replaces the login for `login.host`. Thread-safe.
Result<void> save_login(const SiteLogin &login);
// nullopt if none is saved for `host`. Thread-safe.
Result<std::optional<SiteLogin>> find_login(const std::string &host);
// Succeeds if there was none. Thread-safe.
Result<void> delete_login(const std::string &host);
// All saved logins, without passwords. Thread-safe.
Result<std::vector<SiteLogin>> list_logins();

} // namespace lanes::core::platform
