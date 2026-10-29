// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <string>
#include <vector>

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

  // --- through the background engine ---
  cli::AddArgs add;
  auto *add_cmd = app.add_subcommand("add", "Add a download to the background engine");
  add_request_options(*add_cmd, add.request);
  add_cmd->add_option("-o,--output", add.output, "Folder (or folder\\file name) to save into");
  add_cmd->add_option("--queue", add.queue, "Wait in this queue instead of starting now");
  add_cmd->add_option("--at", add.at, "Start at local time \"YYYY-MM-DD HH:MM\"");
  add_cmd->add_option("-c,--connections", add.connections, "Parallel connections, 1-32")
      ->check(CLI::Range(1, 32));
  add_cmd->add_option("--limit", add.limit, "Speed limit for this download, e.g. 2M");
  add_cmd->add_option("-d,--data", add.request.data, "POST this body instead of GET");
  add_cmd->add_option("--sha256", add.sha256, "Verify the SHA-256 of the finished file");
  add_cmd->add_option("--md5", add.md5, "Verify the MD5 of the finished file");

  std::string list_queue;
  auto *list_cmd = app.add_subcommand("list", "Show downloads");
  list_cmd->add_option("--queue", list_queue, "Only this queue");

  std::string pause_target;
  auto *pause_cmd = app.add_subcommand("pause", "Pause a download (or all)");
  pause_cmd->add_option("id", pause_target, "Download id, or \"all\"")->required();

  std::string resume_target;
  std::string resume_url;
  auto *resume_cmd = app.add_subcommand("resume", "Resume a download (or all)");
  resume_cmd->add_option("id", resume_target, "Download id, or \"all\"")->required();
  resume_cmd->add_option("--url", resume_url, "New address for an expired link");

  std::string remove_target;
  bool remove_delete = false;
  auto *remove_cmd = app.add_subcommand("remove", "Remove a download from the list");
  remove_cmd->add_option("id", remove_target, "Download id")->required();
  remove_cmd->add_flag("--delete-file", remove_delete, "Also delete the file (or partial file)");

  cli::RunArgs run;
  auto *run_cmd = app.add_subcommand("run", "Process a queue and show its progress");
  run_cmd->add_option("--queue", run.queue, "Queue name")->capture_default_str();
  run_cmd->add_option("--max", run.max, "Downloads at once")->check(CLI::Range(1, 32));
  run_cmd->add_option("--until", run.until, "Stop the queue at this time, HH:MM");

  std::vector<std::string> config_args;
  auto *config_cmd = app.add_subcommand(
      "config", "Show or change settings: download_dir, connections, max_active, speed_limit");
  config_cmd->add_option("key_value", config_args, "[key [value]]")->expected(0, 2);

  bool engine_stop = false;
  auto *engine_cmd = app.add_subcommand("engine", "Run the background engine (started on demand)");
  engine_cmd->add_flag("--stop", engine_stop, "Stop the running engine");
  engine_cmd->group(""); // hidden from help

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
  if (add_cmd->parsed()) {
    return cli::run_add(add);
  }
  if (list_cmd->parsed()) {
    return cli::run_list(list_queue);
  }
  if (pause_cmd->parsed()) {
    return cli::run_pause(pause_target);
  }
  if (resume_cmd->parsed()) {
    return cli::run_resume(resume_target, resume_url);
  }
  if (remove_cmd->parsed()) {
    return cli::run_remove(remove_target, remove_delete);
  }
  if (run_cmd->parsed()) {
    return cli::run_queue(run);
  }
  if (config_cmd->parsed()) {
    return cli::run_config(config_args);
  }
  if (engine_cmd->parsed()) {
    return cli::run_engine(engine_stop);
  }
  std::cout << app.help();
  return cli::kExitOk;
}
