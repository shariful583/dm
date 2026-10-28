// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/net/request.hpp"

namespace lanes::cli {

// Exit codes.
inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitInterrupted = 130;

// Request options shared by `get` and `info`, as typed on the command line.
struct RequestArgs {
  std::string url;
  std::vector<std::string> headers; // "Name: value"
  std::string cookie;
  std::string referer;
  std::string user_agent;
  std::string user; // "name:password"
  std::string proxy;
  std::string proxy_user;
  std::string data; // POST body; non-empty means POST
};

struct GetArgs {
  RequestArgs request;
  std::string output; // file, or folder (existing, or ending in a separator); empty = current
  int connections = 8;
  std::string limit; // "2M"; empty = unlimited
  std::string sha256;
  std::string md5;
  bool no_resume = false;
  bool restart = false; // start over if the file changed on the server
  bool quiet = false;
};

core::Result<core::net::RequestOptions> build_request(const RequestArgs &args);

int run_get(const GetArgs &args);
int run_info(const RequestArgs &args);

} // namespace lanes::cli
