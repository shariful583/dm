// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace lanes::core {

using DownloadId = std::int64_t;

enum class DownloadState {
  Queued,    // waiting for a free slot (or for its queue to start)
  Scheduled, // waiting for its start time
  Running,
  Paused, // stopped by the user
  Completed,
  Failed,       // gave up; `resume` tries again
  NeedsAddress, // the link expired (403/410): resume with a new URL
};

// Stable lowercase names, also used in the database and the pipe protocol.
std::string_view to_string(DownloadState state) noexcept;
std::optional<DownloadState> parse_download_state(std::string_view text) noexcept;

} // namespace lanes::core
