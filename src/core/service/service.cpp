// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/service/service.hpp"

#include <atomic>
#include <memory>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/platform/path.hpp"
#include "core/platform/system.hpp"
#include "core/version.hpp"

namespace lanes::core::service {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// --- JSON helpers ---

json opt(const std::optional<std::int64_t> &v) { return v ? json(*v) : json(nullptr); }
json opt(const std::optional<std::uint64_t> &v) { return v ? json(*v) : json(nullptr); }

std::optional<std::int64_t> opt_int(const json &j, const char *key) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_number_integer()) ? std::optional(it->get<std::int64_t>())
                                                    : std::nullopt;
}

std::string str(const json &j, const char *key) {
  const auto it = j.find(key);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

json credentials(const std::optional<net::Credentials> &c) {
  return c ? json::array({c->username, c->password}) : json(nullptr);
}

std::optional<net::Credentials> credentials(const json &j, const char *key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array() || it->size() != 2) {
    return std::nullopt;
  }
  return net::Credentials{(*it)[0].get<std::string>(), (*it)[1].get<std::string>()};
}

const char *duplicate_name(DuplicateAction a) {
  switch (a) {
  case DuplicateAction::Rename:
    return "rename";
  case DuplicateAction::Overwrite:
    return "overwrite";
  case DuplicateAction::Resume:
    return "resume";
  case DuplicateAction::Ask:
    break;
  }
  return "ask";
}

std::optional<DuplicateAction> parse_duplicate(const std::string &text) {
  if (text.empty() || text == "ask") {
    return DuplicateAction::Ask;
  }
  if (text == "rename") {
    return DuplicateAction::Rename;
  }
  if (text == "overwrite") {
    return DuplicateAction::Overwrite;
  }
  if (text == "resume") {
    return DuplicateAction::Resume;
  }
  return std::nullopt;
}

json categories_json(const std::vector<category::Category> &categories) {
  return json::parse(category::to_json(categories));
}

json to_json(const NewDownload &n) {
  json headers = json::array();
  for (const auto &[name, value] : n.request.headers) {
    headers.push_back({name, value});
  }
  return {
      {"url", n.request.url},
      {"method", n.request.method},
      {"body", n.request.body ? json(*n.request.body) : json(nullptr)},
      {"headers", headers},
      {"cookies", n.request.cookies},
      {"referer", n.request.referer},
      {"user_agent", n.request.user_agent},
      {"auth", credentials(n.request.auth)},
      {"proxy", n.request.proxy.url},
      {"proxy_auth", credentials(n.request.proxy.credentials)},
      {"directory", platform::path_to_utf8(n.directory)},
      {"filename", n.filename},
      {"queue", n.queue},
      {"start_at", opt(n.start_at)},
      {"connections", n.connections},
      {"speed_limit", n.speed_limit},
      {"checksum", n.checksum ? json(std::string(n.checksum->algorithm == verify::HashAlgorithm::Md5
                                                     ? "md5:"
                                                     : "sha256:") +
                                     n.checksum->hex)
                              : json(nullptr)},
      {"if_duplicate", duplicate_name(n.if_duplicate)},
      {"overwrite_file", n.overwrite_file},
  };
}

Result<NewDownload> new_download(const json &j) {
  NewDownload n;
  n.request.url = str(j, "url");
  if (const std::string method = str(j, "method"); !method.empty()) {
    n.request.method = method;
  }
  if (const auto it = j.find("body"); it != j.end() && it->is_string()) {
    n.request.body = it->get<std::string>();
  }
  if (const auto it = j.find("headers"); it != j.end() && it->is_array()) {
    for (const auto &h : *it) {
      if (h.is_array() && h.size() == 2 && h[0].is_string() && h[1].is_string()) {
        n.request.headers.emplace_back(h[0].get<std::string>(), h[1].get<std::string>());
      }
    }
  }
  n.request.cookies = str(j, "cookies");
  n.request.referer = str(j, "referer");
  n.request.user_agent = str(j, "user_agent");
  n.request.auth = credentials(j, "auth");
  n.request.proxy.url = str(j, "proxy");
  n.request.proxy.credentials = credentials(j, "proxy_auth");
  n.directory = platform::path_from_utf8(str(j, "directory"));
  n.filename = str(j, "filename");
  n.queue = str(j, "queue");
  n.start_at = opt_int(j, "start_at");
  n.connections = static_cast<int>(opt_int(j, "connections").value_or(0));
  n.speed_limit =
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, opt_int(j, "speed_limit").value_or(0)));
  if (const std::string checksum = str(j, "checksum"); !checksum.empty()) {
    const auto colon = checksum.find(':');
    const auto algorithm = checksum.substr(0, colon) == "md5" ? verify::HashAlgorithm::Md5
                                                              : verify::HashAlgorithm::Sha256;
    auto c = verify::make_checksum(algorithm,
                                   colon == std::string::npos ? "" : checksum.substr(colon + 1));
    if (!c) {
      return make_error(c.error());
    }
    n.checksum = *c;
  }
  const auto duplicate = parse_duplicate(str(j, "if_duplicate"));
  if (!duplicate) {
    return make_error(ErrorCode::InvalidArgument, "if_duplicate must be ask, rename, overwrite "
                                                  "or resume");
  }
  n.if_duplicate = *duplicate;
  n.overwrite_file = j.value("overwrite_file", false);
  return n;
}

json to_json(const DownloadInfo &d) {
  json segments = json::array();
  for (const auto &s : d.segments) {
    segments.push_back({s.start, s.end ? json(*s.end) : json(nullptr), s.received});
  }
  return {
      {"id", d.id},
      {"url", d.url},
      {"filename", d.filename},
      {"directory", platform::path_to_utf8(d.directory)},
      {"category", d.category},
      {"queue", d.queue},
      {"state", std::string(to_string(d.state))},
      {"start_now", d.start_now},
      {"start_at", opt(d.start_at)},
      {"size", opt(d.size)},
      {"downloaded", d.downloaded},
      {"speed", d.bytes_per_second},
      {"error", d.error},
      {"connections", d.connections},
      {"speed_limit", d.speed_limit},
      {"added_at", d.added_at},
      {"completed_at", opt(d.completed_at)},
      {"segments", segments},
  };
}

DownloadInfo download_info(const json &j) {
  DownloadInfo d;
  d.id = opt_int(j, "id").value_or(0);
  d.url = str(j, "url");
  d.filename = str(j, "filename");
  d.directory = platform::path_from_utf8(str(j, "directory"));
  d.category = str(j, "category");
  d.queue = str(j, "queue");
  d.state = parse_download_state(str(j, "state")).value_or(DownloadState::Queued);
  d.start_now = j.value("start_now", false);
  d.start_at = opt_int(j, "start_at");
  if (const auto size = opt_int(j, "size")) {
    d.size = static_cast<std::uint64_t>(*size);
  }
  d.downloaded = j.value("downloaded", std::uint64_t{0});
  d.bytes_per_second = j.value("speed", 0.0);
  d.error = str(j, "error");
  d.connections = j.value("connections", 0);
  d.speed_limit = j.value("speed_limit", std::uint64_t{0});
  d.added_at = opt_int(j, "added_at").value_or(0);
  d.completed_at = opt_int(j, "completed_at");
  if (const auto it = j.find("segments"); it != j.end() && it->is_array()) {
    for (const auto &s : *it) {
      if (s.is_array() && s.size() == 3) {
        engine::SegmentProgress p;
        p.start = s[0].get<std::uint64_t>();
        if (!s[1].is_null()) {
          p.end = s[1].get<std::uint64_t>();
        }
        p.received = s[2].get<std::uint64_t>();
        d.segments.push_back(p);
      }
    }
  }
  return d;
}

json to_json(const queue::QueueSettings &q) {
  return {
      {"name", q.name},
      {"max_active", q.max_active},
      {"enabled", q.enabled},
      {"start_time", q.start_time ? json(queue::to_string(*q.start_time)) : json(nullptr)},
      {"stop_time", q.stop_time ? json(queue::to_string(*q.stop_time)) : json(nullptr)},
      {"days", q.days},
  };
}

queue::QueueSettings queue_settings(const json &j) {
  queue::QueueSettings q;
  q.name = str(j, "name");
  q.max_active = j.value("max_active", 3);
  q.enabled = j.value("enabled", false);
  if (auto t = queue::parse_time_of_day(str(j, "start_time"))) {
    q.start_time = *t;
  }
  if (auto t = queue::parse_time_of_day(str(j, "stop_time"))) {
    q.stop_time = *t;
  }
  q.days = static_cast<std::uint8_t>(j.value("days", static_cast<int>(queue::kEveryDay)));
  return q;
}

std::string ok(json body = json::object()) {
  body["ok"] = true;
  return body.dump();
}

std::string failure(const Error &e) {
  return json{{"ok", false},
              {"error",
               {{"code", std::string(to_string(e.code))},
                {"message", e.message},
                {"http_status", e.http_status}}}}
      .dump();
}

std::string respond(const Result<void> &r) { return r ? ok() : failure(r.error()); }

ErrorCode parse_error_code(const std::string &text) {
  for (int i = 0; i <= static_cast<int>(ErrorCode::ChecksumMismatch); ++i) {
    const auto code = static_cast<ErrorCode>(i);
    if (to_string(code) == text) {
      return code;
    }
  }
  return ErrorCode::Unknown;
}

} // namespace

// --- server side ---

EngineService::EngineService(Engine &engine, std::function<void()> request_shutdown)
    : engine_(engine), request_shutdown_(std::move(request_shutdown)) {}

void EngineService::set_on_activate(std::function<void(std::vector<std::string>)> handler) {
  on_activate_ = std::move(handler);
}

std::string EngineService::handle(std::string_view request) {
  const json j = json::parse(request, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return failure({ErrorCode::InvalidArgument, "request is not a JSON object", 0});
  }
  if (j.value("v", 0) > kProtocolVersion) {
    return failure({ErrorCode::Unsupported, "newer protocol version; update Lanes", 0});
  }
  const std::string cmd = str(j, "cmd");
  try {
    if (cmd == "ping") {
      return ok(
          {{"pid", platform::current_process_id()}, {"version", std::string(kVersionString)}});
    }
    if (cmd == "add") {
      auto n = new_download(j.value("download", json::object()));
      if (!n) {
        return failure(n.error());
      }
      auto id = engine_.add(std::move(*n));
      return id ? ok({{"id", *id}}) : failure(id.error());
    }
    if (cmd == "list") {
      json downloads = json::array();
      for (const auto &d : engine_.list()) {
        downloads.push_back(to_json(d));
      }
      return ok({{"downloads", downloads}});
    }
    if (cmd == "pause" || cmd == "resume") {
      if (j.value("all", false)) {
        cmd == "pause" ? engine_.pause_all() : engine_.resume_all();
        return ok();
      }
      const auto id = opt_int(j, "id");
      if (!id) {
        return failure({ErrorCode::InvalidArgument, "missing download id", 0});
      }
      if (cmd == "pause") {
        return respond(engine_.pause(*id));
      }
      const std::string url = str(j, "url");
      return respond(engine_.resume(*id, url.empty() ? std::nullopt : std::optional(url)));
    }
    if (cmd == "remove") {
      const auto id = opt_int(j, "id");
      if (!id) {
        return failure({ErrorCode::InvalidArgument, "missing download id", 0});
      }
      return respond(engine_.remove(*id, j.value("delete_files", false)));
    }
    if (cmd == "queues") {
      json queues = json::array();
      for (const auto &q : engine_.queues()) {
        queues.push_back(to_json(q));
      }
      return ok({{"queues", queues}});
    }
    if (cmd == "queue_start") {
      const std::string name = str(j, "name");
      if (const auto max = opt_int(j, "max_active")) {
        for (auto q : engine_.queues()) {
          if (q.name == name) {
            q.max_active = static_cast<int>(*max);
            if (auto saved = engine_.save_queue(q); !saved) {
              return failure(saved.error());
            }
          }
        }
      }
      return respond(engine_.start_queue(name));
    }
    if (cmd == "queue_stop") {
      return respond(engine_.stop_queue(str(j, "name")));
    }
    if (cmd == "queue_save") {
      return respond(engine_.save_queue(queue_settings(j.value("queue", json::object()))));
    }
    if (cmd == "categories") {
      return ok({{"categories", categories_json(engine_.categories())}});
    }
    if (cmd == "set_categories") {
      auto parsed = category::from_json(j.value("categories", json::array()).dump());
      if (!parsed) {
        return failure(parsed.error());
      }
      return respond(engine_.set_categories(std::move(*parsed)));
    }
    if (cmd == "settings") {
      json settings = json::object();
      for (const auto &[key, value] : engine_.settings()) {
        settings[key] = value;
      }
      return ok({{"settings", settings}});
    }
    if (cmd == "set_setting") {
      return respond(engine_.set_setting(str(j, "key"), str(j, "value")));
    }
    if (cmd == "activate") {
      if (!on_activate_) {
        return failure({ErrorCode::Unsupported, "no Lanes window to show", 0});
      }
      std::vector<std::string> args;
      for (const auto &a : j.value("args", json::array())) {
        if (args.size() < 64 && a.is_string() && a.get_ref<const std::string &>().size() < 8192) {
          args.push_back(a.get<std::string>());
        }
      }
      on_activate_(std::move(args));
      return ok();
    }
    if (cmd == "shutdown") {
      if (request_shutdown_) {
        request_shutdown_();
      }
      return ok();
    }
  } catch (const json::exception &e) {
    return failure({ErrorCode::InvalidArgument, std::string("bad request: ") + e.what(), 0});
  }
  return failure({ErrorCode::InvalidArgument, "unknown command '" + cmd + "'", 0});
}

void serve(platform::PipeServer server, EngineService &service, std::stop_token stop,
           std::function<void()> on_request) {
  std::vector<std::jthread> clients;
  while (!stop.stop_requested()) {
    auto connection = server.accept(stop);
    if (!connection) {
      if (connection.error().code == ErrorCode::Cancelled) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    clients.emplace_back([&service, &on_request,
                          conn = std::move(*connection)](std::stop_token client_stop) mutable {
      for (;;) {
        auto request = conn.read(client_stop);
        if (!request) {
          return; // closed, cancelled or broken
        }
        if (on_request) {
          on_request();
        }
        if (!conn.write(service.handle(*request))) {
          return;
        }
      }
    });
  }
  clients.clear(); // stops and joins every client thread
}

int run_engine_host(std::chrono::seconds idle_timeout) {
  auto server = platform::PipeServer::create(platform::default_pipe_name());
  if (!server) {
    return server.error().code == ErrorCode::AlreadyExists ? 0 : 1;
  }
  auto engine = Engine::open({.database = platform::app_data_dir() / "lanes.db"});
  if (!engine) {
    return 1;
  }
  std::stop_source stop;
  std::atomic<std::chrono::steady_clock::rep> last_request{
      std::chrono::steady_clock::now().time_since_epoch().count()};
  EngineService service(**engine, [&stop] { stop.request_stop(); });
  std::jthread serving([&](std::stop_token st) {
    serve(std::move(*server), service, st, [&last_request] {
      last_request = std::chrono::steady_clock::now().time_since_epoch().count();
    });
  });

  while (!stop.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto since = std::chrono::steady_clock::now() -
                       std::chrono::steady_clock::time_point(
                           std::chrono::steady_clock::duration(last_request.load()));
    if ((*engine)->idle() && since > idle_timeout) {
      break;
    }
  }
  serving.request_stop();
  serving.join();
  (*engine)->shutdown();
  return 0;
}

// --- client side ---

Result<EngineClient> EngineClient::connect(const ClientOptions &options) {
  const std::string name = platform::default_pipe_name();
  auto connection = platform::connect_pipe(name, std::chrono::seconds(5));
  if (!connection && connection.error().code == ErrorCode::NotFound && options.start_engine) {
    if (auto started = platform::spawn_detached(options.engine_executable, options.engine_args);
        !started) {
      return make_error(started.error());
    }
    const auto deadline = std::chrono::steady_clock::now() + options.start_timeout;
    do {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      connection = platform::connect_pipe(name, std::chrono::seconds(5));
    } while (!connection && connection.error().code == ErrorCode::NotFound &&
             std::chrono::steady_clock::now() < deadline);
  }
  if (!connection) {
    return make_error(connection.error());
  }
  EngineClient client;
  client.connection_ = std::move(*connection);
  return client;
}

Result<std::string> EngineClient::call(const std::string &request) {
  if (auto sent = connection_.write(request); !sent) {
    return make_error(sent.error());
  }
  return connection_.read();
}

namespace {

// Sends `request`; returns the response object when "ok", else its error.
Result<json> exchange(EngineClient &client, json request) {
  request["v"] = kProtocolVersion;
  auto text = client.call(request.dump());
  if (!text) {
    return make_error(text.error());
  }
  json response = json::parse(*text, nullptr, false);
  if (response.is_discarded() || !response.is_object()) {
    return make_error(ErrorCode::Unknown, "malformed reply from the Lanes engine");
  }
  if (!response.value("ok", false)) {
    const json &e = response.value("error", json::object());
    return make_error(Error{parse_error_code(e.value("code", std::string())),
                            e.value("message", std::string("unknown error")),
                            e.value("http_status", 0)});
  }
  return response;
}

Result<void> exchange_void(EngineClient &client, json request) {
  auto r = exchange(client, std::move(request));
  return r ? Result<void>() : make_error(r.error());
}

} // namespace

Result<std::uint32_t> EngineClient::ping() {
  auto r = exchange(*this, {{"cmd", "ping"}});
  if (!r) {
    return make_error(r.error());
  }
  return r->value("pid", std::uint32_t{0});
}

Result<DownloadId> EngineClient::add(const NewDownload &download) {
  auto r = exchange(*this, {{"cmd", "add"}, {"download", to_json(download)}});
  if (!r) {
    return make_error(r.error());
  }
  return r->value("id", DownloadId{0});
}

Result<std::vector<DownloadInfo>> EngineClient::list() {
  auto r = exchange(*this, {{"cmd", "list"}});
  if (!r) {
    return make_error(r.error());
  }
  std::vector<DownloadInfo> out;
  for (const auto &d : r->value("downloads", json::array())) {
    out.push_back(download_info(d));
  }
  return out;
}

Result<void> EngineClient::pause(std::optional<DownloadId> id) {
  return exchange_void(*this, id ? json{{"cmd", "pause"}, {"id", *id}}
                                 : json{{"cmd", "pause"}, {"all", true}});
}

Result<void> EngineClient::resume(std::optional<DownloadId> id,
                                  std::optional<std::string> new_url) {
  json request = id ? json{{"cmd", "resume"}, {"id", *id}} : json{{"cmd", "resume"}, {"all", true}};
  if (new_url) {
    request["url"] = *new_url;
  }
  return exchange_void(*this, std::move(request));
}

Result<void> EngineClient::remove(DownloadId id, bool delete_files) {
  return exchange_void(*this, {{"cmd", "remove"}, {"id", id}, {"delete_files", delete_files}});
}

Result<std::vector<queue::QueueSettings>> EngineClient::queues() {
  auto r = exchange(*this, {{"cmd", "queues"}});
  if (!r) {
    return make_error(r.error());
  }
  std::vector<queue::QueueSettings> out;
  for (const auto &q : r->value("queues", json::array())) {
    out.push_back(queue_settings(q));
  }
  return out;
}

Result<void> EngineClient::start_queue(const std::string &name, std::optional<int> max_active) {
  json request{{"cmd", "queue_start"}, {"name", name}};
  if (max_active) {
    request["max_active"] = *max_active;
  }
  return exchange_void(*this, std::move(request));
}

Result<void> EngineClient::stop_queue(const std::string &name) {
  return exchange_void(*this, {{"cmd", "queue_stop"}, {"name", name}});
}

Result<std::vector<std::pair<std::string, std::string>>> EngineClient::settings() {
  auto r = exchange(*this, {{"cmd", "settings"}});
  if (!r) {
    return make_error(r.error());
  }
  std::vector<std::pair<std::string, std::string>> out;
  // Keep the object alive: items() only references it.
  const json settings = r->value("settings", json::object());
  for (const auto &[key, value] : settings.items()) {
    out.emplace_back(key, value.is_string() ? value.get<std::string>() : value.dump());
  }
  return out;
}

Result<void> EngineClient::set_setting(const std::string &key, const std::string &value) {
  return exchange_void(*this, {{"cmd", "set_setting"}, {"key", key}, {"value", value}});
}

Result<void> EngineClient::shutdown_engine() { return exchange_void(*this, {{"cmd", "shutdown"}}); }

Result<void> EngineClient::activate(const std::vector<std::string> &args) {
  return exchange_void(*this, {{"cmd", "activate"}, {"args", args}});
}

Result<std::vector<category::Category>> EngineClient::categories() {
  auto r = exchange(*this, {{"cmd", "categories"}});
  if (!r) {
    return make_error(r.error());
  }
  return category::from_json(r->value("categories", json::array()).dump());
}

Result<void> EngineClient::set_categories(const std::vector<category::Category> &categories) {
  return exchange_void(*this,
                       {{"cmd", "set_categories"}, {"categories", categories_json(categories)}});
}

} // namespace lanes::core::service
