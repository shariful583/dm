// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include "cli/commands.hpp"
#include "core/version.hpp"

namespace {

void add_request_options(CLI::App &cmd, lanes::cli::RequestArgs &r) {
  cmd.add_option("url", r.url, "http, https or ftp address")->required();
  cmd.add_option("-H,--header", r.headers, "Extra request header \"Name: value\" (repeatable)");
  cmd.add_option("--cookie", r.cookie, "Cookies \"name=value; name2=value2\"");
  cmd.add_option("--referer", r.referer, "Referer page");
  cmd.add_option("-A,--user-agent", r.user_agent, "User-Agent (default Lanes/<version>)");
  cmd.add_option("--user", r.user, "HTTP login \"name:password\"");
  cmd.add_option("--proxy", r.proxy, "Proxy, e.g. http://host:8080 or socks5h://host:1080");
  cmd.add_option("--proxy-user", r.proxy_user, "Proxy login \"name:password\"");
}

} // namespace

int main(int argc, char **argv) {
  using namespace lanes;
  CLI::App app{std::string(core::kProductName), "lanes"};
  argv = app.ensure_utf8(argv); // Windows: real Unicode arguments, not the ANSI code page
  app.set_version_flag("-V,--version",
                       std::string(core::kProductName) + " " + std::string(core::kVersionString));
  app.require_subcommand(0, 1);

  cli::GetArgs get;
  auto *get_cmd = app.add_subcommand("get", "Download a file (resumes automatically)");
  add_request_options(*get_cmd, get.request);
  get_cmd->add_option("-o,--output", get.output,
                      "Output file, or folder (existing or ending in \\) for the server's name");
  get_cmd->add_option("-c,--connections", get.connections, "Parallel connections, 1-32")
      ->capture_default_str()
      ->check(CLI::Range(1, 32));
  get_cmd->add_option("--limit", get.limit, "Speed limit, e.g. 500K, 2M");
  get_cmd->add_option("-d,--data", get.request.data, "POST this body instead of GET");
  get_cmd->add_option("--sha256", get.sha256, "Verify the SHA-256 of the finished file");
  get_cmd->add_option("--md5", get.md5, "Verify the MD5 of the finished file");
  get_cmd->add_flag("--no-resume", get.no_resume, "Start over even if a partial download exists");
  get_cmd->add_flag("--restart", get.restart,
                    "If the file changed on the server, download it again from the start");
  get_cmd->add_flag("-q,--quiet", get.quiet, "No progress or summary output");

  cli::RequestArgs info;
  auto *info_cmd = app.add_subcommand("info", "Show what the server says about a download");
  add_request_options(*info_cmd, info);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    const int code = app.exit(e);
    return code == 0 ? cli::kExitOk : cli::kExitUsage;
  }

  if (get_cmd->parsed()) {
    return cli::run_get(get);
  }
  if (info_cmd->parsed()) {
    return cli::run_info(info);
  }
  std::cout << app.help();
  return cli::kExitOk;
}
