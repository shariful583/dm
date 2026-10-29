// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/extract/info.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

namespace lanes::core::extract {

namespace {

using nlohmann::json;

std::string str(const json &j, const char *key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double num(const json &j, const char *key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_number() ? it->get<double>() : 0.0;
}

bool starts_with(std::string_view s, std::string_view prefix) {
  return s.substr(0, prefix.size()) == prefix;
}

Format parse_format(const json &f) {
  Format out;
  out.id = str(f, "format_id");
  out.ext = str(f, "ext");
  out.protocol = str(f, "protocol");
  out.url = str(f, "url");
  if (const auto h = f.find("http_headers"); h != f.end() && h->is_object()) {
    for (const auto &[name, value] : h->items()) {
      if (value.is_string()) {
        out.http_headers.emplace_back(name, value.get<std::string>());
      }
    }
  }
  if (num(f, "filesize") > 0) {
    out.filesize = static_cast<std::uint64_t>(num(f, "filesize"));
  } else if (num(f, "filesize_approx") > 0) {
    out.filesize = static_cast<std::uint64_t>(num(f, "filesize_approx"));
    out.size_is_estimate = true;
  }
  out.width = static_cast<int>(num(f, "width"));
  out.height = static_cast<int>(num(f, "height"));
  out.fps = num(f, "fps");
  out.tbr = num(f, "tbr");
  out.abr = num(f, "abr");
  // A missing codec (null) means yt-dlp does not know it; video_ext / audio_ext ("none" when the
  // stream is absent) still tell whether there is one. Without either, a plain file usually has
  // both.
  const auto codec = [&f](const char *codec_key, const char *ext_key) {
    if (const auto c = f.find(codec_key); c != f.end() && c->is_string()) {
      return c->get<std::string>();
    }
    if (const auto e = f.find(ext_key); e != f.end() && e->is_string()) {
      return e->get<std::string>() == "none" ? std::string("none") : std::string("unknown");
    }
    return std::string("unknown");
  };
  out.vcodec = codec("vcodec", "video_ext");
  out.acodec = codec("acodec", "audio_ext");
  out.note = str(f, "format_note");
  out.language = str(f, "language");
  out.has_drm = f.value("has_drm", false) == true;
  out.fragment_base_url = str(f, "fragment_base_url");
  if (const auto fr = f.find("fragments"); fr != f.end() && fr->is_array()) {
    for (const auto &piece : *fr) {
      std::string u = str(piece, "url");
      if (u.empty()) {
        u = str(piece, "path");
      }
      out.fragments.push_back(std::move(u));
    }
  }
  return out;
}

bool usable(const Format &f) {
  if (f.has_drm || f.protocol == "mhtml" || f.note == "storyboard") {
    return false;
  }
  if (f.url.empty() && f.fragments.empty()) {
    return false;
  }
  return f.has_video() || f.has_audio();
}

int protocol_rank(const Format &f) {
  if (f.protocol == "https" || f.protocol == "http" || f.protocol.empty()) {
    return 0; // Lanes' segmented engine
  }
  if (f.protocol == "http_dash_segments") {
    return 1;
  }
  if (starts_with(f.protocol, "m3u8")) {
    return 2;
  }
  return 3;
}

bool mp4_video(const Format &f) {
  return f.ext == "mp4" || f.ext == "m4v" || f.ext == "mov" || starts_with(f.vcodec, "avc") ||
         starts_with(f.vcodec, "h264") || starts_with(f.vcodec, "hev") ||
         starts_with(f.vcodec, "hvc") || starts_with(f.vcodec, "av01");
}

// AAC in MP4: plays everywhere (AC-3 / E-AC-3 tracks also come as .m4a).
bool m4a_audio(const Format &f) {
  return starts_with(f.acodec, "mp4a") ||
         (f.acodec == "unknown" && (f.ext == "m4a" || (f.ext == "mp4" && !f.has_video())));
}

// Dynamic range compressed audio (YouTube "DRC"): quieter parts made louder; not the original.
bool drc(const Format &f) {
  return f.note.find("DRC") != std::string::npos || f.id.ends_with("-drc");
}

// True if `a` is the better video (height, fps, speed of download path, MP4 fit, bitrate).
bool better_video(const Format &a, const Format &b) {
  if (a.height != b.height) {
    return a.height > b.height;
  }
  if (std::lround(a.fps) != std::lround(b.fps)) {
    return a.fps > b.fps;
  }
  if (protocol_rank(a) != protocol_rank(b)) {
    return protocol_rank(a) < protocol_rank(b);
  }
  if (mp4_video(a) != mp4_video(b)) {
    return mp4_video(a);
  }
  return a.tbr > b.tbr;
}

bool better_audio(const Format &a, const Format &b, bool prefer_m4a) {
  if (drc(a) != drc(b)) {
    return !drc(a);
  }
  if (prefer_m4a && m4a_audio(a) != m4a_audio(b)) {
    return m4a_audio(a);
  }
  const double ra = a.abr > 0 ? a.abr : a.tbr;
  const double rb = b.abr > 0 ? b.abr : b.tbr;
  if (ra != rb) {
    return ra > rb;
  }
  return protocol_rank(a) < protocol_rank(b);
}

const Format *best_audio_only(const VideoInfo &info, bool prefer_m4a) {
  const Format *best = nullptr;
  for (const auto &f : info.formats) {
    if (usable(f) && f.has_audio() && !f.has_video() &&
        (best == nullptr || better_audio(f, *best, prefer_m4a))) {
      best = &f;
    }
  }
  return best;
}

std::string container_of(const Format &f) {
  if (starts_with(f.protocol, "m3u8") || f.ext.empty()) {
    return "mp4"; // HLS is remuxed to MP4
  }
  return f.ext;
}

void set_size(Selection &s) {
  std::uint64_t total = 0;
  for (const Format *f : {s.video, s.audio}) {
    if (f == nullptr) {
      continue;
    }
    if (!f->filesize) {
      return;
    }
    total += *f->filesize;
  }
  s.size = total;
}

Result<Selection> finish(Selection s) {
  if (s.video != nullptr && s.audio != nullptr) {
    if (mp4_video(*s.video) && m4a_audio(*s.audio)) {
      s.container = "mp4";
    } else if (s.video->ext == "webm" && s.audio->ext == "webm") {
      s.container = "webm";
    } else {
      s.container = "mkv";
    }
  } else if (s.video != nullptr) {
    s.container = container_of(*s.video);
  } else if (s.audio != nullptr && s.container.empty()) {
    s.container = container_of(*s.audio);
  }
  set_size(s);
  return s;
}

Result<Selection> nothing(const VideoInfo &info, std::string_view what) {
  const bool drm = std::any_of(info.formats.begin(), info.formats.end(),
                               [](const Format &f) { return f.has_drm; });
  if (drm) {
    return make_error(ErrorCode::Unsupported,
                      "this video is DRM-protected; Lanes does not download protected video");
  }
  return make_error(ErrorCode::NotFound, "no " + std::string(what) + " is available");
}

} // namespace

Result<VideoInfo> parse_info(std::string_view text) {
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return make_error(ErrorCode::InvalidArgument, "yt-dlp did not return video information");
  }
  VideoInfo info;
  info.id = str(j, "id");
  info.title = str(j, "title");
  info.extractor = str(j, "extractor");
  info.webpage_url = str(j, "webpage_url");
  if (info.webpage_url.empty()) {
    info.webpage_url = str(j, "original_url");
  }
  // The largest JPEG/PNG thumbnail (WebP shows nowhere without extra codecs), else yt-dlp's pick.
  info.thumbnail = str(j, "thumbnail");
  double best_area = -1;
  for (const auto &t : j.value("thumbnails", json::array())) {
    const std::string url = str(t, "url");
    std::string path = url.substr(0, url.find_first_of("?#"));
    for (char &c : path) {
      c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
    if (!path.ends_with(".jpg") && !path.ends_with(".jpeg") && !path.ends_with(".png")) {
      continue;
    }
    const double area = num(t, "width") * num(t, "height") + num(t, "preference") * 1e-3;
    if (area > best_area) {
      best_area = area;
      info.thumbnail = url;
    }
  }
  info.uploader = str(j, "uploader");
  info.duration = num(j, "duration");
  info.is_live = j.value("is_live", false) == true || str(j, "live_status") == "is_live";

  if (str(j, "_type") == "playlist") {
    info.is_playlist = true;
    if (const auto e = j.find("entries"); e != j.end() && e->is_array()) {
      for (const auto &entry : *e) {
        if (!entry.is_object()) {
          continue;
        }
        PlaylistEntry p{str(entry, "id"), str(entry, "webpage_url"), str(entry, "title"),
                        num(entry, "duration")};
        if (p.url.empty()) {
          p.url = str(entry, "url");
        }
        if (!p.url.empty()) {
          info.entries.push_back(std::move(p));
        }
      }
    }
    return info;
  }

  if (const auto f = j.find("formats"); f != j.end() && f->is_array()) {
    for (const auto &format : *f) {
      if (format.is_object()) {
        info.formats.push_back(parse_format(format));
      }
    }
  } else if (!str(j, "url").empty()) {
    info.formats.push_back(parse_format(j)); // a single-format result
  }
  if (info.formats.empty()) {
    return make_error(ErrorCode::NotFound, "yt-dlp found no video on this page");
  }
  if (const auto s = j.find("subtitles"); s != j.end() && s->is_object()) {
    for (const auto &[language, files] : s->items()) {
      std::vector<Subtitle> list;
      if (files.is_array()) {
        for (const auto &file : files) {
          if (!str(file, "url").empty()) {
            list.push_back({str(file, "ext"), str(file, "url")});
          }
        }
      }
      if (!list.empty()) {
        info.subtitles.emplace_back(language, std::move(list));
      }
    }
  }
  return info;
}

Result<Selection> select_formats(const VideoInfo &info, std::string_view choice) {
  if (info.is_live) {
    return make_error(ErrorCode::Unsupported, "live streams cannot be downloaded");
  }
  Selection s;
  if (choice == "audio" || choice == "mp3") {
    s.audio = best_audio_only(info, choice == "audio");
    if (s.audio == nullptr) {
      return nothing(info, "audio-only format");
    }
    if (choice == "mp3") {
      s.to_mp3 = true;
      s.container = "mp3";
    }
    return finish(s);
  }

  std::optional<int> limit;
  if (choice != "best" && !choice.empty()) {
    if (choice.size() > 1 && choice.back() == 'p') {
      int h = 0;
      const auto r = std::from_chars(choice.data(), choice.data() + choice.size() - 1, h);
      if (r.ec == std::errc{} && r.ptr == choice.data() + choice.size() - 1 && h > 0) {
        limit = h;
      }
    }
    if (!limit) {
      // Format ids: "18" or "137+140".
      std::vector<const Format *> picked;
      std::size_t start = 0;
      while (start <= choice.size()) {
        const auto plus = choice.find('+', start);
        const std::string_view id = choice.substr(start, plus - start);
        const auto it = std::find_if(info.formats.begin(), info.formats.end(),
                                     [&](const Format &f) { return f.id == id; });
        if (it == info.formats.end()) {
          return make_error(ErrorCode::NotFound,
                            "no format '" + std::string(id) + "' (try --list)");
        }
        if (it->has_drm) {
          return nothing(info, "unprotected format");
        }
        picked.push_back(&*it);
        if (plus == std::string_view::npos) {
          break;
        }
        start = plus + 1;
      }
      if (picked.size() > 2) {
        return make_error(ErrorCode::InvalidArgument, "at most one video and one audio format");
      }
      for (const Format *f : picked) {
        (f->has_video() && s.video == nullptr ? s.video : s.audio) = f;
      }
      return finish(s);
    }
  }

  std::vector<const Format *> videos;
  for (const auto &f : info.formats) {
    if (usable(f) && f.has_video()) {
      videos.push_back(&f);
    }
  }
  if (videos.empty()) {
    if (const Format *a = best_audio_only(info, true)) { // audio-only sites
      s.audio = a;
      return finish(s);
    }
    return nothing(info, "downloadable format");
  }
  for (const Format *f : videos) {
    if (limit && f->height > *limit) {
      continue;
    }
    if (s.video == nullptr || better_video(*f, *s.video)) {
      s.video = f;
    }
  }
  if (s.video == nullptr) { // everything is taller than asked: the smallest
    s.video = *std::min_element(videos.begin(), videos.end(), [](const Format *a, const Format *b) {
      return a->height < b->height;
    });
  }
  if (!s.video->has_audio()) {
    s.audio = best_audio_only(info, mp4_video(*s.video));
  }
  return finish(s);
}

std::vector<std::pair<std::string, Selection>> quality_choices(const VideoInfo &info) {
  std::vector<std::pair<std::string, Selection>> out;
  std::set<int, std::greater<>> heights;
  for (const auto &f : info.formats) {
    if (usable(f) && f.has_video() && f.height > 0) {
      heights.insert(f.height);
    }
  }
  for (const int h : heights) {
    if (auto s = select_formats(info, std::to_string(h) + "p")) {
      out.emplace_back(std::to_string(h) + "p", *s);
    }
  }
  if (heights.empty()) {
    if (auto s = select_formats(info, "best")) {
      out.emplace_back("best", *s);
    }
  }
  for (const char *audio : {"audio", "mp3"}) {
    if (auto s = select_formats(info, audio)) {
      out.emplace_back(audio, *s);
    }
  }
  return out;
}

std::string netscape_cookies(std::string_view cookies, std::string_view url) {
  std::string_view host = url;
  if (const auto scheme = host.find("://"); scheme != std::string_view::npos) {
    host.remove_prefix(scheme + 3);
  }
  host = host.substr(0, host.find_first_of("/?#"));
  if (const auto at = host.rfind('@'); at != std::string_view::npos) {
    host.remove_prefix(at + 1);
  }
  host = host.substr(0, host.find(':'));
  std::string domain(host);
  for (char &c : domain) {
    c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  if (starts_with(domain, "www.")) {
    domain.erase(0, 4);
  }
  std::string out = "# Netscape HTTP Cookie File\n";
  std::size_t start = 0;
  while (start < cookies.size()) {
    auto end = cookies.find(';', start);
    if (end == std::string_view::npos) {
      end = cookies.size();
    }
    std::string_view pair = cookies.substr(start, end - start);
    start = end + 1;
    while (!pair.empty() && pair.front() == ' ') {
      pair.remove_prefix(1);
    }
    const auto eq = pair.find('=');
    if (eq == 0 || eq == std::string_view::npos ||
        pair.find_first_of("\t\r\n") != std::string_view::npos) {
      continue;
    }
    out += "." + domain + "\tTRUE\t/\tFALSE\t2147483647\t" + std::string(pair.substr(0, eq)) +
           "\t" + std::string(pair.substr(eq + 1)) + "\n";
  }
  return out;
}

std::optional<ToolProgress> parse_progress_line(std::string_view line) {
  constexpr std::string_view prefix = "LANES|";
  const auto at = line.find(prefix);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  line.remove_prefix(at + prefix.size());
  std::vector<std::optional<double>> fields;
  while (fields.size() < 4) {
    const auto bar = line.find('|');
    const std::string_view field = line.substr(0, bar);
    double v = 0;
    const auto r = std::from_chars(field.data(), field.data() + field.size(), v);
    fields.push_back(r.ec == std::errc{} && v >= 0 ? std::optional<double>(v) : std::nullopt);
    if (bar == std::string_view::npos) {
      break;
    }
    line.remove_prefix(bar + 1);
  }
  if (fields.empty() || !fields[0]) {
    return std::nullopt;
  }
  ToolProgress p;
  p.downloaded = static_cast<std::uint64_t>(*fields[0]);
  if (fields.size() > 1 && fields[1]) {
    p.total = static_cast<std::uint64_t>(*fields[1]);
  } else if (fields.size() > 2 && fields[2]) {
    p.total = static_cast<std::uint64_t>(*fields[2]);
  }
  if (fields.size() > 3 && fields[3]) {
    p.speed = *fields[3];
  }
  return p;
}

} // namespace lanes::core::extract
