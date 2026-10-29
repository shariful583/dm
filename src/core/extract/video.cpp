// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/extract/video.hpp"

#include <algorithm>
#include <mutex>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/engine/speed_meter.hpp"
#include "core/extract/ytdlp.hpp"
#include "core/media/fetch.hpp"
#include "core/media/ffmpeg.hpp"
#include "core/media/stream.hpp"
#include "core/net/request.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"

namespace lanes::core::extract {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

std::string lower(std::string s) {
  for (char &c : s) {
    c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  return s;
}

bool native(const Format &f) {
  return ((f.protocol == "https" || f.protocol == "http" || f.protocol.empty()) &&
          !f.url.empty()) ||
         (starts_with(f.protocol, "m3u8") && !f.url.empty()) ||
         (f.protocol == "http_dash_segments" && !f.fragments.empty());
}

// Lanes may not fetch it itself (no data yet): let yt-dlp try.
bool refused(const Error &e) {
  return e.code == ErrorCode::Http && (e.http_status == 401 || e.http_status == 403 ||
                                       e.http_status == 410 || e.http_status == 429);
}

std::string ids_of(const Selection &s) {
  if (s.video != nullptr && s.audio != nullptr) {
    return s.video->id + "+" + s.audio->id;
  }
  return s.video != nullptr ? s.video->id : s.audio->id;
}

// Totals across the formats of one video, for one progress line.
class Progress {
public:
  Progress(const engine::DownloadOptions &options, std::optional<std::uint64_t> total)
      : options_(options), total_(total) {}

  // After re-reading the page: start over with the new formats' total.
  void reset(std::optional<std::uint64_t> total) {
    std::lock_guard lock(mutex_);
    total_ = total;
    done_ = 0;
  }

  // A format finished with `bytes`.
  void finished(std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    done_ += bytes;
  }

  void part(const engine::DownloadProgress &p) {
    if (!options_.on_progress) {
      return;
    }
    engine::DownloadProgress out;
    {
      std::lock_guard lock(mutex_);
      out.downloaded = done_ + p.downloaded;
      out.total = total_;
      out.bytes_per_second = p.bytes_per_second;
      out.parts_done = p.parts_done;
      out.parts_total = p.parts_total;
      // The connection map shows the current format at its place in the whole download.
      for (auto s : p.segments) {
        s.start += done_;
        if (s.end) {
          *s.end += done_;
        }
        out.segments.push_back(s);
      }
    }
    options_.on_progress(out);
  }

  void tool(const ToolProgress &t) {
    meter_.add(t.downloaded);
    engine::DownloadProgress p;
    p.downloaded = t.downloaded;
    p.total = t.total ? t.total : total_;
    p.bytes_per_second = t.speed > 0 ? t.speed : meter_.rate();
    if (options_.on_progress) {
      options_.on_progress(p);
    }
  }

private:
  const engine::DownloadOptions &options_;
  std::optional<std::uint64_t> total_;
  std::mutex mutex_;
  std::uint64_t done_ = 0;
  engine::SpeedMeter meter_;
};

struct Fetched {
  std::vector<fs::path> files; // played in this order
  std::vector<fs::path> cleanup;
  std::uint64_t bytes = 0;
};

engine::DownloadOptions part_options(const engine::DownloadOptions &options, const Format &f,
                                     const fs::path &dir, const std::string &name,
                                     Progress &progress) {
  engine::DownloadOptions o = options;
  o.request.url = f.url;
  o.request.method = "GET";
  o.request.body.reset();
  o.request.headers.clear();
  for (const auto &[header, value] : f.http_headers) {
    if (lower(header) == "user-agent") {
      o.request.user_agent = value;
    } else if (lower(header) != "cookie" || options.request.cookies.empty()) {
      o.request.headers.emplace_back(header, value);
    }
  }
  o.directory = dir;
  o.choose_directory = {};
  o.filename = name;
  o.if_file_exists = engine::ExistingFile::Overwrite;
  o.probe.reset();
  o.checksum.reset();
  o.on_target = {};
  o.on_progress = [&progress](const engine::DownloadProgress &p) { progress.part(p); };
  return o;
}

// One format into files next to the final file.
Result<Fetched> fetch_format(const engine::DownloadOptions &options, const Format &f,
                             const fs::path &final_path, Progress &progress, std::stop_token stop) {
  const fs::path dir = final_path.parent_path();
  const std::string stem = platform::path_to_utf8(final_path.stem());
  Fetched out;
  if (starts_with(f.protocol, "m3u8")) {
    const std::string name = stem + ".f" + f.id + ".mp4";
    auto r = media::download_stream(part_options(options, f, dir, name, progress), stop);
    if (!r) {
      return make_error(r.error());
    }
    out.files = {r->path};
    out.bytes = r->size;
  } else if (f.protocol == "http_dash_segments") {
    media::Track track;
    for (const auto &piece : f.fragments) {
      const bool absolute = piece.find("://") != std::string::npos;
      track.parts.push_back({absolute ? piece : f.fragment_base_url + piece, {}, -1, 0});
    }
    fs::path folder = final_path;
    folder += ".f" + f.id + ".parts";
    media::FetchOptions fo;
    fo.request = part_options(options, f, dir, "", progress).request;
    fo.connections = options.connections;
    fo.limiters = options.limiters;
    if (options.speed_limit > 0) {
      fo.limiters.push_back(std::make_shared<limiter::RateLimiter>(options.speed_limit));
    }
    fo.on_progress = [&progress](const media::FetchProgress &p) {
      progress.part({p.bytes, std::nullopt, 0, {}, p.parts_done, p.parts_total});
    };
    auto files = media::fetch_tracks({track}, folder, fo, stop);
    if (!files) {
      return make_error(files.error());
    }
    out.files = (*files)[0];
    out.cleanup = {folder};
    std::error_code ec;
    for (const auto &file : out.files) {
      out.bytes += fs::file_size(platform::to_long_path(file), ec);
    }
  } else {
    const std::string name = stem + ".f" + f.id + "." + (f.ext.empty() ? "bin" : f.ext);
    auto r = engine::download_file(part_options(options, f, dir, name, progress), stop);
    if (!r) {
      return make_error(r.error());
    }
    out.files = {r->path};
    out.bytes = r->size;
  }
  out.cleanup.insert(out.cleanup.end(), out.files.begin(), out.files.end());
  progress.finished(out.bytes);
  return out;
}

void remove_paths(const std::vector<fs::path> &paths) {
  std::error_code ec;
  for (const auto &p : paths) {
    fs::remove_all(platform::to_long_path(p), ec);
  }
}

// Subtitles and the thumbnail, next to the video; best effort (a missing one is no failure).
void download_extras(const engine::DownloadOptions &options, const VideoRequest &request,
                     const VideoInfo &info, const fs::path &final_path, std::stop_token stop) {
  const fs::path dir = final_path.parent_path();
  const std::string stem = platform::path_to_utf8(final_path.stem());
  const auto fetch = [&](const std::string &url, const std::string &name) {
    engine::DownloadOptions o = options;
    o.request.url = url;
    o.request.method = "GET";
    o.request.body.reset();
    o.directory = dir;
    o.choose_directory = {};
    o.filename = name;
    o.if_file_exists = engine::ExistingFile::Overwrite;
    o.connections = 1;
    o.probe.reset();
    o.checksum.reset();
    o.on_progress = {};
    o.on_target = {};
    (void)engine::download_file(o, stop);
  };
  for (const auto &language : request.subtitles) {
    for (const auto &[lang, files] : info.subtitles) {
      if (lang != language && !starts_with(lang, language + "-")) {
        continue;
      }
      const auto pick = std::find_if(files.begin(), files.end(), [](const Subtitle &s) {
        return s.ext == "vtt" || s.ext == "srt";
      });
      const Subtitle &s = pick != files.end() ? *pick : files.front();
      fetch(s.url, stem + "." + lang + "." + (s.ext.empty() ? "vtt" : s.ext));
      break;
    }
  }
  if (request.thumbnail && !info.thumbnail.empty()) {
    std::string ext = lower(platform::path_to_utf8(
        fs::path(platform::path_from_utf8(net::filename_from_url(info.thumbnail))).extension()));
    if (ext != ".jpg" && ext != ".jpeg" && ext != ".png" && ext != ".webp") {
      ext = ".jpg";
    }
    fetch(info.thumbnail, stem + ext);
  }
}

} // namespace

std::string to_json(const VideoRequest &request) {
  return json{{"format", request.format},
              {"subtitles", request.subtitles},
              {"thumbnail", request.thumbnail}}
      .dump();
}

Result<VideoRequest> parse_video_request(std::string_view text) {
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return make_error(ErrorCode::InvalidArgument, "bad video request");
  }
  VideoRequest r;
  r.format = j.value("format", std::string("best"));
  if (const auto s = j.find("subtitles"); s != j.end() && s->is_array()) {
    for (const auto &lang : *s) {
      if (lang.is_string()) {
        r.subtitles.push_back(lang.get<std::string>());
      }
    }
  }
  r.thumbnail = j.value("thumbnail", false);
  return r;
}

Result<engine::DownloadResult> download_video(const engine::DownloadOptions &options,
                                              const VideoRequest &request, std::stop_token stop,
                                              const VideoInfo *given) {
  const PageRequest page{options.request.url, options.request.cookies, options.request.user_agent,
                         options.request.referer};
  VideoInfo info;
  if (given != nullptr) {
    info = *given;
  } else {
    auto extracted = extract({.page = page}, stop);
    if (!extracted) {
      return make_error(extracted.error());
    }
    info = std::move(*extracted);
  }
  if (info.is_playlist) {
    return make_error(ErrorCode::InvalidArgument, "this address is a playlist (" +
                                                      std::to_string(info.entries.size()) +
                                                      " videos); choose the videos to download");
  }
  auto selection = select_formats(info, request.format);
  if (!selection) {
    return make_error(selection.error());
  }

  // The final file: the title (or the given name) with the container's extension.
  std::string name = options.filename.empty() ? platform::sanitize_filename(info.title, "video") +
                                                    "." + selection->container
                                              : platform::sanitize_filename(options.filename);
  if (fs::path(platform::path_from_utf8(name)).extension().empty()) {
    name += "." + selection->container;
  }
  std::error_code ec;
  fs::path directory = options.directory;
  if (directory.empty()) {
    directory = options.choose_directory ? options.choose_directory(name) : fs::current_path(ec);
  }
  if (auto ok = platform::create_directories(directory); !ok) {
    return make_error(ok.error());
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
  const std::string stem = platform::path_to_utf8(final_path.stem());

  Progress progress(options, selection->size);
  std::vector<std::vector<fs::path>> tracks;
  std::vector<fs::path> cleanup;
  bool use_tool = (selection->video != nullptr && !native(*selection->video)) ||
                  (selection->audio != nullptr && !native(*selection->audio));
  if (!use_tool) {
    for (int attempt = 0; attempt < 2 && !use_tool; ++attempt) {
      tracks.clear();
      Error failure{};
      bool failed = false;
      for (const Format *f : {selection->video, selection->audio}) {
        if (f == nullptr) {
          continue;
        }
        auto fetched = fetch_format(options, *f, final_path, progress, stop);
        if (!fetched) {
          failure = fetched.error();
          failed = true;
          break;
        }
        tracks.push_back(fetched->files);
        cleanup.insert(cleanup.end(), fetched->cleanup.begin(), fetched->cleanup.end());
      }
      if (!failed) {
        break;
      }
      if (failure.code == ErrorCode::LinkExpired && attempt == 0) {
        // Format addresses expire (YouTube: hours): read the page again, same formats, and
        // continue; finished bytes are kept by the parts' resume data.
        const std::string ids = ids_of(*selection);
        auto fresh = extract({.page = page}, stop);
        if (!fresh) {
          return make_error(fresh.error());
        }
        info = std::move(*fresh);
        selection = select_formats(info, ids);
        if (!selection) {
          return make_error(selection.error());
        }
        progress.reset(selection->size);
        continue;
      }
      if (refused(failure) || failure.code == ErrorCode::LinkExpired) {
        use_tool = true; // the site refuses Lanes' own requests: yt-dlp knows its tricks
        break;
      }
      return make_error(failure);
    }
  }

  if (use_tool) {
    tracks.clear();
    const std::string tool_ext = selection->to_mp3 ? selection->audio->ext : selection->container;
    const fs::path out =
        final_path.parent_path() / platform::path_from_utf8(stem + ".lanes-ytdlp." + tool_ext);
    auto ok = download_with_ytdlp(
        page, ids_of(*selection), out, [&progress](const ToolProgress &t) { progress.tool(t); },
        stop);
    if (!ok) {
      return make_error(ok.error());
    }
    tracks.push_back({out});
    cleanup.push_back(out);
  }

  // Assemble the final file.
  Result<void> joined;
  if (selection->to_mp3) {
    fs::path audio = tracks.front().front();
    if (tracks.front().size() > 1) { // DASH fragments: join first
      audio = final_path.parent_path() /
              platform::path_from_utf8(stem + ".lanes-audio." + selection->audio->ext);
      cleanup.push_back(audio);
      if (auto ok = media::mux({tracks.front()}, audio, stop); !ok) {
        return make_error(ok.error());
      }
    }
    joined = media::to_mp3(audio, final_path, stop);
  } else if (tracks.size() == 1 && tracks.front().size() == 1 &&
             lower(platform::path_to_utf8(tracks.front().front().extension())) ==
                 "." + selection->container) {
    joined = platform::atomic_replace(tracks.front().front(), final_path); // nothing to join
  } else {
    joined = media::mux(tracks, final_path, stop);
  }
  if (!joined) {
    return make_error(joined.error());
  }
  std::erase(cleanup, final_path);
  remove_paths(cleanup);

  download_extras(options, request, info, final_path, stop);

  engine::DownloadResult result;
  result.path = final_path;
  result.size = fs::file_size(platform::to_long_path(final_path), ec);
  result.connections = options.connections;
  result.connection_limit = options.connections;
  result.info.final_url = info.webpage_url.empty() ? options.request.url : info.webpage_url;
  result.info.size = result.size;
  result.info.filename = name;
  if (options.on_progress) {
    engine::DownloadProgress done;
    done.downloaded = result.size;
    done.total = result.size;
    options.on_progress(done);
  }
  return result;
}

} // namespace lanes::core::extract
