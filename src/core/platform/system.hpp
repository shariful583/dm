// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::platform {

// Per-user data folder: %LOCALAPPDATA%\Lanes (not created here). LANES_DATA_DIR overrides it
// (tests). Thread-safe.
std::filesystem::path app_data_dir();

// The user's Downloads folder (Known Folder). Thread-safe.
std::filesystem::path downloads_dir();

// Full path of the running executable. Thread-safe.
std::filesystem::path current_executable();

// Environment variable (UTF-8), nullopt if unset. Thread-safe.
std::optional<std::string> environment_variable(const std::string &name);

// Thread-safe.
std::uint32_t current_process_id();

// The current user's SID, e.g. "S-1-5-21-...". Thread-safe.
Result<std::string> current_user_sid();

// Starts a program that keeps running after this process exits: no console window, no inherited
// handles, and out of the caller's job object when the job allows breakaway (otherwise it stays
// in it). Arguments are quoted like ProcessOptions::args. Thread-safe.
Result<std::uint32_t> spawn_detached(const std::filesystem::path &executable,
                                     const std::vector<std::string> &args);

} // namespace lanes::core::platform
