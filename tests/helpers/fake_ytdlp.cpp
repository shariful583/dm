// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for yt-dlp.exe in tests (no test may reach the internet). Behaviour
// comes from environment variables:
//   LANES_FAKE_YTDLP_JSON     file printed for -J, with {{BASE}} replaced by LANES_FAKE_BASE
//   LANES_FAKE_YTDLP_LOG      every run appends its arguments (one per line) and "--cookies-file:
//                             <first line of the cookie file>", then "---"
//   LANES_FAKE_YTDLP_SOURCE   file copied to the -o path when "downloading"
//   LANES_FAKE_YTDLP_VERSION  printed for --version (default 2000.01.01)
//   LANES_FAKE_YTDLP_FAIL     printed as "ERROR: <text>" with exit code 1
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string env(const char *name) {
  const char *v = std::getenv(name);
  return v != nullptr ? v : "";
}

std::string read(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string replace_all(std::string text, const std::string &from, const std::string &to) {
  for (std::size_t at = text.find(from); at != std::string::npos;
       at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
  return text;
}

} // namespace

int main(int argc, char *argv[]) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  const auto value = [&](const std::string &flag) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == flag) {
        return args[i + 1];
      }
    }
    return std::string();
  };
  const auto has = [&](const std::string &flag) {
    for (const auto &a : args) {
      if (a == flag) {
        return true;
      }
    }
    return false;
  };

  if (const std::string log = env("LANES_FAKE_YTDLP_LOG"); !log.empty()) {
    std::ofstream out(log, std::ios::app | std::ios::binary);
    for (const auto &a : args) {
      out << a << '\n';
    }
    if (const std::string cookies = value("--cookies"); !cookies.empty()) {
      std::ifstream c(cookies);
      std::string first;
      std::string line;
      std::getline(c, first);
      while (std::getline(c, line)) {
        out << "cookie-line: " << line << '\n';
      }
      out << "--cookies-file: " << first << '\n';
    }
    out << "---\n";
  }

  if (has("--version")) {
    const std::string v = env("LANES_FAKE_YTDLP_VERSION");
    std::cout << (v.empty() ? "2000.01.01" : v) << '\n';
    return 0;
  }
  if (const std::string fail = env("LANES_FAKE_YTDLP_FAIL"); !fail.empty()) {
    std::cerr << "ERROR: " << fail << '\n';
    return 1;
  }
  if (has("-J")) {
    std::cout << replace_all(read(env("LANES_FAKE_YTDLP_JSON")), "{{BASE}}", env("LANES_FAKE_BASE"))
              << '\n';
    return 0;
  }
  if (has("-f")) {
    const std::string output = value("-o");
    const std::string data = read(env("LANES_FAKE_YTDLP_SOURCE"));
    std::string tmpl = value("--progress-template");
    if (tmpl.rfind("download:", 0) == 0) {
      tmpl = tmpl.substr(9);
    }
    for (int step = 1; step <= 2; ++step) {
      std::string line = tmpl;
      line = replace_all(line, "%(progress.downloaded_bytes)s",
                         std::to_string(data.size() * step / 2));
      line = replace_all(line, "%(progress.total_bytes)s", std::to_string(data.size()));
      line = replace_all(line, "%(progress.total_bytes_estimate)s", "NA");
      line = replace_all(line, "%(progress.speed)s", "1048576.0");
      std::cout << line << '\n' << std::flush;
    }
    std::ofstream(output, std::ios::binary) << data;
    return 0;
  }
  std::cerr << "ERROR: fake yt-dlp: unexpected arguments\n";
  return 2;
}
