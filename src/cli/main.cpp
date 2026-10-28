// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include "core/version.hpp"

int main(int argc, char **argv) {
  CLI::App app{std::string(lanes::core::kProductName), "lanes"};
  app.set_version_flag("-V,--version", std::string(lanes::core::kProductName) + " " +
                                           std::string(lanes::core::kVersionString));
  app.require_subcommand(0, 1);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  if (app.get_subcommands().empty()) {
    std::cout << app.help();
  }
  return 0;
}
