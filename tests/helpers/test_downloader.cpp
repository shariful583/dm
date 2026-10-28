// SPDX-License-Identifier: GPL-3.0-or-later
// Child program for the crash/resume test: downloads one URL with the engine and is killed by the
// test midway. Usage: lanes_test_downloader <url> <directory> <connections>
#include <cstdlib>
#include <string>

#include "core/engine/download.hpp"
#include "core/platform/path.hpp"

int main(int argc, char **argv) {
  if (argc != 4) {
    return 2;
  }
  lanes::core::engine::DownloadOptions options;
  options.request.url = argv[1];
  options.directory = lanes::core::platform::path_from_utf8(argv[2]);
  options.connections = std::atoi(argv[3]);
  options.checkpoint_interval = std::chrono::milliseconds(200);
  return lanes::core::engine::download_file(options) ? 0 : 1;
}
