// SPDX-License-Identifier: GPL-3.0-or-later
#include "bridge/bridge.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/net/request.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "core/platform/registry.hpp"
#include "core/platform/system.hpp"
#include "core/service/service.hpp"

namespace lanes::bridge {

namespace {

using core::ErrorCode;
using core::make_error;
using nlohmann::json;
using namespace std::chrono_literals;

constexpr std::size_t kMaxUrl = 8192;
constexpr std::size_t kMaxLine = 1024;
constexpr std::size_t kMaxCookies = 64u << 10;
constexpr std::size_t kMaxBody = 1u << 20;
constexpr std::size_t kMaxItems = 1000;

// Thrown inside validate() only; turned into InvalidArgument there.
struct Invalid {
  std::string what;
};

bool has_control_chars(const std::string &s) {
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) {
      return true;
    }
  }
  return false;
}

// A single-line string field (no control characters: these values end up in HTTP headers).
std::string text(const json &j, const char *field, std::size_t max, bool multi_line = false) {
  const auto it = j.find(field);
  if (it == j.end() || it->is_null()) {
    return {};
  }
  if (!it->is_string()) {
    throw Invalid{std::string(field) + " must be a string"};
  }
  std::string s = it->get<std::string>();
  if (s.size() > max) {
    throw Invalid{std::string(field) + " is too long"};
  }
  if (!multi_line && has_control_chars(s)) {
    throw Invalid{std::string(field) + " contains control characters"};
  }
  return s;
}

std::string url(const json &j, const char *field, bool required) {
  std::string u = text(j, field, kMaxUrl);
  if (u.empty()) {
    if (required) {
      throw Invalid{std::string(field) + " is missing"};
    }
    return {};
  }
  if (auto ok = core::net::validate_url(u); !ok) {
    throw Invalid{std::string(field) + ": " + ok.error().message};
  }
  return u;
}

// A byte count: a non-negative integer, or absent (-1).
double size(const json &j, const char *field) {
  const auto it = j.find(field);
  if (it == j.end() || it->is_null()) {
    return -1;
  }
  if (!it->is_number() || it->get<double>() < 0 || it->get<double>() > 9.0e15) {
    throw Invalid{std::string(field) + " must be a byte count"};
  }
  return std::floor(it->get<double>());
}

// Fields every request-carrying message may have.
void request_fields(const json &in, json &out) {
  out["referrer"] = url(in, "referrer", false);
  out["pageUrl"] = url(in, "pageUrl", false);
  out["userAgent"] = text(in, "userAgent", kMaxLine);
  out["cookies"] = text(in, "cookies", kMaxCookies);
  out["title"] = text(in, "title", kMaxLine);
}

// Media the page loaded: [{url, mime, size, title, referrer}] (optional; empty list if absent).
// `referrer` is the player frame's address, which stream servers often check.
json media_items(const json &in) {
  json list = json::array();
  const auto items = in.find("items");
  if (items == in.end() || items->is_null()) {
    return list;
  }
  if (!items->is_array()) {
    throw Invalid{"items must be a list"};
  }
  if (items->size() > kMaxItems) {
    throw Invalid{"too many items"};
  }
  for (const json &item : *items) {
    if (!item.is_object()) {
      throw Invalid{"items must be objects"};
    }
    list.push_back({{"url", url(item, "url", true)},
                    {"mime", text(item, "mime", 256)},
                    {"size", size(item, "size")},
                    {"title", text(item, "title", kMaxLine)},
                    {"referrer", url(item, "referrer", false)}});
  }
  return list;
}

json validate_json(const json &in) {
  if (!in.is_object()) {
    throw Invalid{"message is not a JSON object"};
  }
  if (const auto v = in.find("v");
      v != in.end() && (!v->is_number_integer() || v->get<int>() > kProtocolVersion)) {
    throw Invalid{"unsupported protocol version; update Lanes"};
  }
  const std::string type = text(in, "type", 64);
  json out = {{"v", kProtocolVersion}, {"type", type}};
  if (type == "ping" || type == "open") {
    return out;
  }
  if (type == "capture_download") {
    out["url"] = url(in, "url", true);
    out["finalUrl"] = url(in, "finalUrl", false);
    out["filename"] = text(in, "filename", kMaxLine);
    out["mime"] = text(in, "mime", 256);
    out["fileSize"] = size(in, "fileSize");
    request_fields(in, out);
    std::string method = text(in, "method", 8);
    if (method.empty()) {
      method = "GET";
    }
    if (method != "GET" && method != "POST") {
      throw Invalid{"method must be GET or POST"};
    }
    out["method"] = method;
    if (method == "POST") {
      out["body"] = text(in, "body", kMaxBody, true);
    }
    return out;
  }
  if (type == "capture_media") {
    if (!in.contains("items") || !in["items"].is_array() || in["items"].empty()) {
      throw Invalid{"items must be a non-empty list"};
    }
    out["items"] = media_items(in);
    request_fields(in, out);
    return out;
  }
  if (type == "video_page") {
    request_fields(in, out);
    if (out["pageUrl"].get_ref<const std::string &>().empty()) {
      throw Invalid{"pageUrl is missing"};
    }
    out["items"] = media_items(in); // what the page played, in case Lanes cannot read the page
    return out;
  }
  throw Invalid{type.empty() ? std::string("type is missing")
                             : "unknown message type '" + type + "'"};
}

} // namespace

core::Result<std::string> read_message(std::istream &in) {
  std::array<unsigned char, 4> header{};
  in.read(reinterpret_cast<char *>(header.data()), header.size());
  if (in.gcount() == 0 && in.eof()) {
    return make_error(ErrorCode::NotFound, "the browser closed the connection");
  }
  if (in.gcount() != static_cast<std::streamsize>(header.size())) {
    return make_error(ErrorCode::InvalidArgument, "truncated message header");
  }
  const std::size_t length =
      static_cast<std::size_t>(header[0]) | (static_cast<std::size_t>(header[1]) << 8) |
      (static_cast<std::size_t>(header[2]) << 16) | (static_cast<std::size_t>(header[3]) << 24);
  if (length > kMaxIncoming) {
    return make_error(ErrorCode::InvalidArgument, "message too large");
  }
  std::string message(length, '\0');
  in.read(message.data(), static_cast<std::streamsize>(length));
  if (in.gcount() != static_cast<std::streamsize>(length)) {
    return make_error(ErrorCode::InvalidArgument, "truncated message");
  }
  return message;
}

core::Result<void> write_message(std::ostream &out, std::string_view message) {
  if (message.size() > kMaxOutgoing) {
    return make_error(ErrorCode::InvalidArgument, "reply too large");
  }
  const auto n = static_cast<std::uint32_t>(message.size());
  const std::array<char, 4> header{static_cast<char>(n & 0xff), static_cast<char>((n >> 8) & 0xff),
                                   static_cast<char>((n >> 16) & 0xff),
                                   static_cast<char>((n >> 24) & 0xff)};
  out.write(header.data(), header.size());
  out.write(message.data(), static_cast<std::streamsize>(message.size()));
  out.flush();
  if (!out) {
    return make_error(ErrorCode::Io, "cannot write to the browser");
  }
  return {};
}

core::Result<std::string> validate(std::string_view message) {
  const json in = json::parse(message, nullptr, false);
  if (in.is_discarded()) {
    return make_error(ErrorCode::InvalidArgument, "message is not valid JSON");
  }
  try {
    return validate_json(in).dump();
  } catch (const Invalid &e) {
    return make_error(ErrorCode::InvalidArgument, e.what);
  } catch (const json::exception &e) {
    return make_error(ErrorCode::InvalidArgument, std::string("bad message: ") + e.what());
  }
}

std::string reply(std::string_view result, std::string_view message) {
  json r = {{"v", kProtocolVersion}, {"result", std::string(result)}};
  if (!message.empty()) {
    r["message"] = std::string(message);
  }
  return r.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string handle(std::string_view message, const Forward &forward) {
  auto valid = validate(message);
  if (!valid) {
    return reply("error", valid.error().message);
  }
  auto answer = forward(*valid);
  if (!answer) {
    // Lanes is not reachable: the browser keeps the download.
    return reply("rejected", "Lanes is not available: " + answer.error().message);
  }
  json parsed = json::parse(*answer, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("result")) {
    return reply("error", "Lanes gave a malformed reply");
  }
  parsed["v"] = kProtocolVersion;
  return parsed.dump(-1, ' ', false, json::error_handler_t::replace);
}

core::Result<std::string> forward_to_lanes(const std::string &message,
                                           const std::filesystem::path &gui) {
  {
    core::service::ClientOptions options;
    options.engine_executable = gui;
    options.engine_args = {"--tray"};
    options.start_timeout = 15s;
    auto client = core::service::EngineClient::connect(options);
    if (!client) {
      return make_error(client.error());
    }
    auto answer = client->browser(message);
    if (answer || answer.error().code != ErrorCode::Unsupported) {
      return answer;
    }
  }
  // Only the windowless `lanes engine` runs: start the GUI, which takes the engine over.
  if (auto started = core::platform::spawn_detached(gui, {"--tray"}); !started) {
    return make_error(started.error());
  }
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  core::Error last{ErrorCode::Timeout, "Lanes did not start in time", 0};
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(200ms);
    core::service::ClientOptions options;
    options.start_engine = false;
    auto client = core::service::EngineClient::connect(options);
    if (!client) {
      last = client.error();
      continue; // between the old owner leaving and the GUI arriving
    }
    auto answer = client->browser(message);
    if (answer || answer.error().code != ErrorCode::Unsupported) {
      return answer;
    }
    last = answer.error();
  }
  return make_error(last);
}

std::vector<std::string> host_registry_keys() {
  // LANES_REGISTRY_PREFIX replaces "Software" (tests).
  std::string prefix = "Software";
  if (auto custom = core::platform::environment_variable("LANES_REGISTRY_PREFIX");
      custom && !custom->empty()) {
    prefix = *custom;
  }
  const std::string host(kHostName);
  return {prefix + "\\Google\\Chrome\\NativeMessagingHosts\\" + host,
          prefix + "\\Microsoft\\Edge\\NativeMessagingHosts\\" + host};
}

core::Result<void> register_host(const std::filesystem::path &bridge_exe,
                                 const std::vector<std::string> &extension_ids) {
  json origins = json::array();
  for (const auto &id : extension_ids) {
    origins.push_back("chrome-extension://" + id + "/");
  }
  const json manifest = {
      {"name", std::string(kHostName)},
      {"description", "Lanes Download Manager"},
      {"path", core::platform::path_to_utf8(bridge_exe)},
      {"type", "stdio"},
      {"allowed_origins", origins},
  };
  const auto manifest_path =
      bridge_exe.parent_path() / core::platform::path_from_utf8(std::string(kHostName) + ".json");
  if (auto ok = core::platform::write_file_atomic(manifest_path, manifest.dump(2) + "\n"); !ok) {
    return ok;
  }
  for (const auto &key : host_registry_keys()) {
    if (auto ok = core::platform::set_user_default_value(
            key, core::platform::path_to_utf8(manifest_path));
        !ok) {
      return ok;
    }
  }
  return {};
}

core::Result<void> unregister_host(const std::filesystem::path &bridge_exe) {
  for (const auto &key : host_registry_keys()) {
    if (auto ok = core::platform::delete_user_key(key); !ok) {
      return ok;
    }
  }
  (void)core::platform::remove_file(
      bridge_exe.parent_path() / core::platform::path_from_utf8(std::string(kHostName) + ".json"));
  return {};
}

} // namespace lanes::bridge
