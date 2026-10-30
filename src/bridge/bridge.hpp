// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/platform/registry.hpp"

namespace lanes::bridge {

// Native messaging: Chrome/Edge start lanes-bridge.exe per connection and talk
// over stdin/stdout, each message a 4-byte little-endian length and that many bytes of UTF-8 JSON.

inline constexpr std::size_t kMaxIncoming = 4u << 20; // browser → bridge (Lanes' own cap)
inline constexpr std::size_t kMaxOutgoing = 1u << 20; // bridge → browser (Chrome's limit)
inline constexpr int kProtocolVersion = 1;

// Next message; NotFound at a clean end of input (the browser closed the port), InvalidArgument
// for a truncated or oversized frame. Not thread-safe.
core::Result<std::string> read_message(std::istream &in);
// Writes one framed message and flushes. InvalidArgument if longer than kMaxOutgoing.
core::Result<void> write_message(std::ostream &out, std::string_view message);

// Checks a message from the extension and rebuilds it from known fields only: every message is
// untrusted input. Types: ping, open, capture_download, capture_media, video_page. URLs must be
// http/https/ftp/ftps; strings and lists are capped. Returns the normalized message (JSON) or
// InvalidArgument naming the problem. Thread-safe.
core::Result<std::string> validate(std::string_view message);

// {"v":1,"result":"accepted"|"rejected"|"error","message":...} for the extension.
std::string reply(std::string_view result, std::string_view message = {});

// Sends a validated message to Lanes and returns Lanes' reply (JSON).
using Forward = std::function<core::Result<std::string>(const std::string &message)>;

// One request: validate, forward, and turn failures into "error"/"rejected" replies (the
// browser then keeps the download). Never throws.
std::string handle(std::string_view message, const Forward &forward);

// Forward to the Lanes window over the per-user pipe, starting `gui` (lanes-gui.exe --tray) when
// no Lanes window runs (also when only the windowless `lanes engine` does; the GUI takes over).
core::Result<std::string> forward_to_lanes(const std::string &message,
                                           const std::filesystem::path &gui);

// Native-messaging host registration for Chrome and Edge: writes the host manifest
// `com.lanes.bridge.json` next to `bridge_exe`, allowing `extension_ids`, and points
// {HKCU,HKLM}\Software\{Google\Chrome,Microsoft\Edge}\NativeMessagingHosts\com.lanes.bridge at
// it. `Machine` (an all-users install in Program Files) needs administrator rights, for the
// manifest next to the program as much as for HKLM.
inline constexpr std::string_view kHostName = "com.lanes.bridge";
core::Result<void>
register_host(const std::filesystem::path &bridge_exe, const std::vector<std::string> &extension_ids,
              core::platform::RegistryScope scope = core::platform::RegistryScope::User);
core::Result<void>
unregister_host(const std::filesystem::path &bridge_exe,
                core::platform::RegistryScope scope = core::platform::RegistryScope::User);
// The registry keys register_host writes (Chrome, Edge).
std::vector<std::string> host_registry_keys();

} // namespace lanes::bridge
