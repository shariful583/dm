// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/stream.hpp"

#include <algorithm>
#include <system_error>

#include "core/engine/speed_meter.hpp"
#include "core/media/dash.hpp"
#include "core/media/fetch.hpp"
#include "core/media/ffmpeg.hpp"
#include "core/media/hls.hpp"
#include "core/media/http.hpp"
#include "core/net/probe.hpp"
#include "core/platform/credentials.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"

namespace lanes::core::media {

namespace {

namespace fs = std::filesystem;

std::string lower(std::string_view s) {
  std::string out(s);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

// Path of `url` without query or fragment, lowercase.
std::string url_path(std::string_view url) {
  const auto end = url.find_first_of("?#");
  return lower(url.substr(0, end));
}

struct Loaded {
  StreamKind kind = StreamKind::Hls;
  std::string text;
  std::string base; // final URL of the playlist/manifest
};

Result<Loaded> load(const net::RequestOptions &request) {
  net::RequestOptions get = request;
  get.method = "GET";
  get.body.reset();
  auto fetched = fetch_small(get);
  if (!fetched) {
    return make_error(fetched.error());
  }
  Loaded l{StreamKind::Hls, std::move(fetched->body), fetched->final_url};
  const std::string_view head = std::string_view(l.text).substr(0, 2048);
  if (head.find("#EXTM3U") != std::string_view::npos) {
    l.kind = StreamKind::Hls;
  } else if (head.find("<MPD") != std::string_view::npos) {
    l.kind = StreamKind::Dash;
  } else {
    return make_error(ErrorCode::InvalidArgument, "not an HLS playlist or DASH manifest");
  }
  return l;
}

// "https://cdn/videos/talk-42/master.m3u8" → "talk-42.mp4": generic playlist names say nothing.
std::string stream_filename(const std::string &url) {
  // The path: after "scheme://host", before "?query" / "#fragment".
  std::string path = url.substr(0, url.find_first_of("?#"));
  if (const auto scheme = path.find("://"); scheme != std::string::npos) {
    const auto slash = path.find('/', scheme + 3);
    path = slash == std::string::npos ? std::string() : path.substr(slash);
  }
  std::vector<std::string> segments;
  for (std::size_t start = 0; start <= path.size();) {
    auto end = path.find('/', start);
    if (end == std::string::npos) {
      end = path.size();
    }
    if (end > start) {
      segments.push_back(path.substr(start, end - start));
    }
    start = end + 1;
  }
  std::string stem;
  for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
    std::string s = net::filename_from_url("http://x/" + *it); // percent-decoded
    const auto dot = s.rfind('.');
    if (dot != std::string::npos && dot > 0) {
      s = s.substr(0, dot);
    }
    const std::string l = lower(s);
    const bool generic = l == "master" || l == "index" || l == "playlist" || l == "manifest" ||
                         l == "prog_index" || l == "stream" || l == "video" || l == "media" ||
                         l.starts_with("chunklist") || l == "hls" || l == "dash";
    if (!s.empty() && !generic) {
      stem = s;
      break;
    }
  }
  return platform::sanitize_filename(stem.empty() ? "video" : stem) + ".mp4";
}

// Highest bandwidth within the height limit (or the smallest height if none fits).
template <typename T, typename Height, typename Bandwidth>
const T *choose(const std::vector<const T *> &items, std::optional<int> max_height, Height height,
                Bandwidth bandwidth) {
  const T *best = nullptr;
  for (const T *item : items) {
    const int h = height(*item);
    if (max_height && h > *max_height) {
      continue;
    }
    if (best == nullptr || bandwidth(*item) > bandwidth(*best)) {
      best = item;
    }
  }
  if (best == nullptr && !items.empty()) { // everything is taller than asked: take the smallest
    best = *std::min_element(items.begin(), items.end(),
                             [&](const T *a, const T *b) { return height(*a) < height(*b); });
  }
  return best;
}

struct Plan {
  StreamInfo info;
  std::vector<Track> tracks;
};

Result<Plan> plan(const net::RequestOptions &request, std::optional<int> max_height,
                  bool fetch_tracks_too) {
  auto loaded = load(request);
  if (!loaded) {
    return make_error(loaded.error());
  }
  Plan p;
  p.info.kind = loaded->kind;
  p.info.filename = stream_filename(loaded->base);
  const auto media_track = [&](const std::string &url) -> Result<Track> {
    net::RequestOptions r = request;
    r.url = url;
    r.method = "GET";
    r.body.reset();
    auto text = fetch_small(r);
    if (!text) {
      return make_error(text.error());
    }
    return hls::parse_media(text->body, text->final_url);
  };

  if (loaded->kind == StreamKind::Hls) {
    if (!hls::is_master(loaded->text)) {
      auto track = hls::parse_media(loaded->text, loaded->base);
      if (!track) {
        return make_error(track.error());
      }
      p.info.choices.push_back({"video", 0, 0, 0, "", ""});
      p.tracks.push_back(std::move(*track));
      return p;
    }
    auto master = hls::parse_master(loaded->text, loaded->base);
    if (!master) {
      return make_error(master.error());
    }
    std::vector<const hls::Variant *> variants;
    for (const auto &v : master->variants) {
      variants.push_back(&v);
      p.info.choices.push_back({"video", v.width, v.height, v.bandwidth, v.codecs, ""});
    }
    for (const auto &r : master->renditions) {
      if (r.type == "AUDIO" && !r.uri.empty()) {
        p.info.choices.push_back({"audio", 0, 0, 0, "", r.language});
      }
    }
    if (!fetch_tracks_too) {
      return p;
    }
    const hls::Variant *v = choose(
        variants, max_height, [](const hls::Variant &x) { return x.height; },
        [](const hls::Variant &x) { return x.bandwidth; });
    auto video = media_track(v->uri);
    if (!video) {
      return make_error(video.error());
    }
    p.tracks.push_back(std::move(*video));
    if (!v->audio_group.empty()) { // separate audio: the group's default rendition
      const hls::Rendition *audio = nullptr;
      for (const auto &r : master->renditions) {
        if (r.type == "AUDIO" && r.group_id == v->audio_group && !r.uri.empty() &&
            (audio == nullptr || (r.is_default && !audio->is_default))) {
          audio = &r;
        }
      }
      if (audio != nullptr) {
        auto track = media_track(audio->uri);
        if (!track) {
          return make_error(track.error());
        }
        p.tracks.push_back(std::move(*track));
      }
    }
    return p;
  }

  auto manifest = dash::parse_mpd(loaded->text, loaded->base);
  if (!manifest) {
    return make_error(manifest.error());
  }
  std::vector<const dash::Representation *> videos;
  std::vector<const dash::Representation *> audios;
  for (const auto &r : manifest->representations) {
    p.info.choices.push_back(
        {r.kind.empty() ? "video" : r.kind, r.width, r.height, r.bandwidth, r.codecs, r.language});
    if (r.kind == "audio") {
      audios.push_back(&r);
    } else if (r.kind != "text") {
      videos.push_back(&r);
    }
  }
  if (!fetch_tracks_too) {
    return p;
  }
  const auto height = [](const dash::Representation &r) { return r.height; };
  const auto bandwidth = [](const dash::Representation &r) { return r.bandwidth; };
  if (const auto *v = choose(videos, max_height, height, bandwidth)) {
    p.tracks.push_back(v->track);
  }
  if (const auto *a = choose(audios, std::nullopt, height, bandwidth)) {
    p.tracks.push_back(a->track);
  }
  if (p.tracks.empty()) {
    return make_error(ErrorCode::InvalidArgument, "the DASH manifest has no audio or video");
  }
  return p;
}

// Players often serve playlists under another name or type (".txt", text/plain,
// application/octet-stream): small responses of such types are read to see what they are.
bool disguised_stream(const net::RequestOptions &request, const net::ProbeResult &probe) {
  constexpr std::uint64_t kMaxPlaylist = 4u << 20;
  if (probe.size && *probe.size > kMaxPlaylist) {
    return false;
  }
  const std::string type = lower(probe.content_type.substr(0, probe.content_type.find(';')));
  if (!type.empty() && !type.starts_with("text/") && type != "application/octet-stream" &&
      type != "binary/octet-stream" && type != "application/json" && type != "application/xml") {
    return false;
  }
  net::RequestOptions get = request;
  get.method = "GET";
  get.body.reset();
  get.url = probe.final_url.empty() ? request.url : probe.final_url;
  // Only the beginning is needed. Binary types may be real files of a few MiB: ask for 4 KiB and
  // give up after 64 KiB if the server sends everything. Text is a playlist or small anyway.
  const bool text = type.starts_with("text/");
  get.headers.emplace_back("Range", "bytes=0-4095");
  auto fetched = fetch_small(get, text ? kMaxPlaylist : 64u << 10);
  if (!fetched) {
    return false; // includes "too large": not a playlist
  }
  const std::string_view head = std::string_view(fetched->body).substr(0, 2048);
  return head.find("#EXTM3U") != std::string_view::npos ||
         head.find("<MPD") != std::string_view::npos;
}

void apply_saved_login(engine::DownloadOptions &options) {
  if (options.use_saved_logins && !options.request.auth) {
    if (auto login = platform::find_login(net::host_of(options.request.url)); login && *login) {
      options.request.auth = net::Credentials{(*login)->username, (*login)->password};
    }
  }
}

} // namespace

bool looks_like_stream(std::string_view url, std::string_view content_type) {
  const std::string type = lower(content_type.substr(0, content_type.find(';')));
  if (type == "application/vnd.apple.mpegurl" || type == "application/x-mpegurl" ||
      type == "audio/mpegurl" || type == "audio/x-mpegurl" || type == "application/dash+xml") {
    return true;
  }
  const std::string path = url_path(url);
  return path.ends_with(".m3u8") || path.ends_with(".mpd");
}

Result<StreamInfo> inspect_stream(const net::RequestOptions &request) {
  auto p = plan(request, std::nullopt, false);
  if (!p) {
    return make_error(p.error());
  }
  return std::move(p->info);
}

Result<engine::DownloadResult> download_stream(const engine::DownloadOptions &given,
                                               std::stop_token stop) {
  engine::DownloadOptions options = given;
  apply_saved_login(options);
  auto p = plan(options.request, options.max_video_height, true);
  if (!p) {
    return make_error(p.error());
  }

  // Where the joined file goes, and under which name.
  std::string name =
      options.filename.empty() ? p->info.filename : platform::sanitize_filename(options.filename);
  if (fs::path(platform::path_from_utf8(name)).extension().empty()) {
    name += ".mp4";
  }
  std::error_code ec;
  fs::path directory = options.directory;
  if (directory.empty()) {
    directory = options.choose_directory ? options.choose_directory(name) : fs::current_path(ec);
  }
  if (options.if_file_exists != engine::ExistingFile::Overwrite &&
      fs::exists(platform::to_long_path(directory / platform::path_from_utf8(name)), ec)) {
    if (options.if_file_exists == engine::ExistingFile::Fail) {
      return make_error(ErrorCode::AlreadyExists, "'" + name + "' already exists");
    }
    name = platform::unique_filename(directory, name);
  }
  const fs::path final_path = directory / platform::path_from_utf8(name);
  if (options.on_target) {
    options.on_target(final_path);
  }
  fs::path parts = final_path;
  parts += ".parts";

  FetchOptions fetch;
  fetch.request = options.request;
  fetch.connections = options.connections;
  fetch.limiters = options.limiters;
  if (options.speed_limit > 0) {
    fetch.limiters.push_back(std::make_shared<limiter::RateLimiter>(options.speed_limit));
  }
  fetch.progress_interval = options.progress_interval;
  engine::SpeedMeter meter;
  engine::DownloadProgress last;
  fetch.on_progress = [&](const FetchProgress &f) {
    meter.add(f.bytes);
    last = engine::DownloadProgress{f.bytes, std::nullopt, meter.rate(),
                                    {},      f.parts_done, f.parts_total};
    if (options.on_progress) {
      options.on_progress(last);
    }
  };
  auto files = fetch_tracks(p->tracks, parts, fetch, stop);
  if (!files) {
    return make_error(files.error()); // finished segments stay for the next attempt
  }
  if (auto ok = mux(*files, final_path, stop); !ok) {
    return make_error(ok.error());
  }
  fs::remove_all(platform::to_long_path(parts), ec);

  engine::DownloadResult result;
  result.path = final_path;
  result.size = fs::file_size(platform::to_long_path(final_path), ec);
  result.connections = options.connections;
  result.connection_limit = options.connections;
  result.info.final_url = options.request.url;
  result.info.size = result.size;
  result.info.content_type = "video/mp4";
  result.info.filename = name;
  if (options.on_progress) {
    last.downloaded = result.size;
    last.total = result.size;
    options.on_progress(last);
  }
  return result;
}

Result<engine::DownloadResult> download_any(engine::DownloadOptions options, std::stop_token stop) {
  if (options.request.method == "POST") {
    return engine::download_file(options, stop);
  }
  if (options.probe) {
    if (looks_like_stream(options.probe->final_url, options.probe->content_type)) {
      return download_stream(options, stop);
    }
    return engine::download_file(options, stop);
  }
  if (looks_like_stream(options.request.url, "")) {
    return download_stream(options, stop);
  }
  apply_saved_login(options);
  if (auto probed = net::probe(options.request)) {
    if (looks_like_stream(probed->final_url, probed->content_type) ||
        disguised_stream(options.request, *probed)) {
      return download_stream(options, stop);
    }
    options.probe = std::move(*probed); // reused: no second probe
  }
  // A failed probe is retried (and reported) by download_file itself.
  return engine::download_file(options, stop);
}

} // namespace lanes::core::media
