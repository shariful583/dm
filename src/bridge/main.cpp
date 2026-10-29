// SPDX-License-Identifier: GPL-3.0-or-later
// lanes-bridge.exe: native-messaging host for the Lanes browser extension.
//
//   (started by Chrome/Edge)   serve messages on stdin/stdout until the browser closes the port
//   lanes-bridge --register    register the host for Chrome and Edge (current user)
//   lanes-bridge --unregister  remove the registration
//
// stdout carries the protocol only: nothing else may ever be printed there while serving.

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <io.h>

#include "bridge/bridge.hpp"
#include "core/platform/system.hpp"

namespace {

// "id1;id2" from CMake (LANES_EXTENSION_IDS): the dev ID plus the store IDs once published.
std::vector<std::string> extension_ids() {
  std::vector<std::string> ids;
  std::string_view all = LANES_EXTENSION_IDS;
  while (!all.empty()) {
    const auto semi = all.find(';');
    const std::string_view id = all.substr(0, semi);
    if (!id.empty()) {
      ids.emplace_back(id);
    }
    all = semi == std::string_view::npos ? std::string_view() : all.substr(semi + 1);
  }
  return ids;
}

} // namespace

int main(int argc, char *argv[]) {
  using namespace lanes;
  const auto exe = core::platform::current_executable();
  if (argc >= 2 && std::string_view(argv[1]) == "--register") {
    if (auto ok = bridge::register_host(exe, extension_ids()); !ok) {
      std::fprintf(stderr, "lanes-bridge: %s\n", ok.error().message.c_str());
      return 1;
    }
    std::printf("Registered %s for Chrome and Edge.\n", std::string(bridge::kHostName).c_str());
    return 0;
  }
  if (argc >= 2 && std::string_view(argv[1]) == "--unregister") {
    if (auto ok = bridge::unregister_host(exe); !ok) {
      std::fprintf(stderr, "lanes-bridge: %s\n", ok.error().message.c_str());
      return 1;
    }
    return 0;
  }

  // The browser passes the extension's origin (and on Windows a window handle) as arguments.
  (void)_setmode(_fileno(stdin), _O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
  const auto gui = exe.parent_path() / "lanes-gui.exe";
  const bridge::Forward forward = [&gui](const std::string &message) {
    return bridge::forward_to_lanes(message, gui);
  };
  while (true) {
    auto message = bridge::read_message(std::cin);
    if (!message) {
      // End of input (normal), or a broken frame: nothing more can be read reliably.
      return message.error().code == core::ErrorCode::NotFound ? 0 : 1;
    }
    if (!bridge::write_message(std::cout, bridge::handle(*message, forward))) {
      return 1;
    }
  }
}
