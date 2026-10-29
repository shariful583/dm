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
  // File already exists: "overwrite" | "rename" | "fail". Empty: overwrite when -o names the
  // file, rename when the name comes from the server.
  std::string if_exists;
  int max_height = 0; // HLS/DASH: best quality up to this video height; 0 = best
};

core::Result<core::net::RequestOptions> build_request(const RequestArgs &args);

int run_get(const GetArgs &args);
int run_info(const RequestArgs &args);

// `lanes video` (video.cpp): video sites through yt-dlp.
struct VideoArgs {
  RequestArgs request;         // the page; cookies for logged-in or age-restricted videos
  std::string format = "best"; // best | <height>p | audio | mp3 | format ids
  bool list = false;           // show formats (or playlist entries) only
  std::string output;          // folder
  std::string subs;            // subtitle languages "en,de"
  bool thumbnail = false;
  bool playlist = false; // the whole playlist, not just the video
  std::string items;     // playlist entries "1-3,7"
  int connections = 8;
  std::string limit;
  bool add = false; // hand to the background engine instead of downloading here
  std::string queue;
  bool quiet = false;
  bool update = false;   // update yt-dlp now
  bool rollback = false; // go back to the yt-dlp before the last update
};

int run_video(const VideoArgs &args);

// --- engine commands (queue.cpp): talk to the background engine over the pipe ---

struct AddArgs {
  RequestArgs request;
  std::string output; // folder, or file inside a folder
  std::string queue;  // empty = start now
  std::string at;     // "YYYY-MM-DD HH:MM"
  int connections = 0;
  std::string limit;
  std::string sha256;
  std::string md5;
  std::string batch;             // file with one URL per line (instead of request.url)
  bool pattern = false;          // expand [001-120] / [a-z] ranges in the URL
  std::string if_exists = "ask"; // URL already listed: ask | rename | overwrite | resume
};

struct RunArgs {
  std::string queue = "main";
  int max = 0;       // 0 = keep the queue's setting
  std::string until; // "HH:MM": stop the queue then
};

int run_add(const AddArgs &args);
int run_list(const std::string &queue);
int run_pause(const std::string &target);                          // id or "all"
int run_resume(const std::string &target, const std::string &url); // id or "all"
int run_remove(const std::string &id, bool delete_file);
int run_queue(const RunArgs &args);
int run_config(const std::vector<std::string> &args); // [] | [key] | [key, value]
int run_engine(bool stop);

// --- organize.cpp: file categories and site logins ---

int run_categories();
int run_category_set(const std::string &name, const std::string &extensions,
                     const std::string &folder);
int run_category_remove(const std::string &name);
int run_category_reset();
int run_login_add(const std::string &host, const std::string &user, const std::string &password,
                  bool password_given);
int run_login_list();
int run_login_remove(const std::string &host);

} // namespace lanes::cli
