// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/engine/download_state.hpp"

#include <array>
#include <utility>

namespace lanes::core {

namespace {
constexpr std::array<std::pair<DownloadState, std::string_view>, 7> kNames{{
    {DownloadState::Queued, "queued"},
    {DownloadState::Scheduled, "scheduled"},
    {DownloadState::Running, "running"},
    {DownloadState::Paused, "paused"},
    {DownloadState::Completed, "completed"},
    {DownloadState::Failed, "failed"},
    {DownloadState::NeedsAddress, "needs_address"},
}};
} // namespace

std::string_view to_string(DownloadState state) noexcept {
  for (const auto &[s, name] : kNames) {
    if (s == state) {
      return name;
    }
  }
  return "queued";
}

std::optional<DownloadState> parse_download_state(std::string_view text) noexcept {
  for (const auto &[s, name] : kNames) {
    if (name == text) {
      return s;
    }
  }
  return std::nullopt;
}

} // namespace lanes::core
