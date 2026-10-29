// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "core/error.hpp"

namespace lanes::core::platform {

// Keeps the computer from going to sleep while downloads run.
// Windows: a power request ("Lanes is downloading files", visible in `powercfg /requests`); the
// display may still turn off. Thread-safe.
class KeepAwake {
public:
  KeepAwake();
  ~KeepAwake();
  KeepAwake(const KeepAwake &) = delete;
  KeepAwake &operator=(const KeepAwake &) = delete;

  // Turns the request on or off (cheap when nothing changes).
  void set(bool awake);
  bool active() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

enum class PowerAction { Sleep, Hibernate, Shutdown };

// Sleep / hibernate / shut down now ("when done"). Shutdown asks running programs to close (Lanes
// saves its downloads as it exits). Thread-safe.
Result<void> power_action(PowerAction action);

// Mark of the Web and antivirus scan of a finished download, as browsers do: the file gets its
// zone (Zone.Identifier with the source and referrer), so SmartScreen and Office know where it
// came from, and the installed antivirus scans it. AccessDenied if the antivirus blocked (and
// removed) the file. Blocking; call from a worker thread. Thread-safe.
Result<void> mark_and_scan(const std::filesystem::path &file, const std::string &source_url,
                           const std::string &referrer);

} // namespace lanes::core::platform
