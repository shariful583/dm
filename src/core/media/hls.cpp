// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/hls.hpp"

#include <charconv>
#include <map>
#include <memory>

#include <curl/curl.h>

#include "core/net/curl.hpp"

namespace lanes::core::media::hls {

namespace {

struct UrlDeleter {
  void operator()(CURLU *u) const noexcept { curl_url_cleanup(u); }
};

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

std::vector<std::string_view> lines_of(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) {
    text.remove_prefix(3);
  }
  std::vector<std::string_view> lines;
  for (std::size_t start = 0; start < text.size();) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    if (const auto line = trim(text.substr(start, end - start)); !line.empty()) {
      lines.push_back(line);
    }
    start = end + 1;
  }
  return lines;
}

// KEY=VALUE,KEY="quoted, value",... → map (quotes removed).
std::map<std::string, std::string> attributes(std::string_view list) {
  std::map<std::string, std::string> out;
  std::size_t pos = 0;
  while (pos < list.size()) {
    const auto eq = list.find('=', pos);
    if (eq == std::string_view::npos) {
      break;
    }
    const std::string key(trim(list.substr(pos, eq - pos)));
    std::size_t value_start = eq + 1;
    std::string value;
    if (value_start < list.size() && list[value_start] == '"') {
      const auto close = list.find('"', value_start + 1);
      value = std::string(list.substr(value_start + 1, close == std::string_view::npos
                                                           ? std::string_view::npos
                                                           : close - value_start - 1));
      pos = close == std::string_view::npos ? list.size() : list.find(',', close);
    } else {
      const auto comma = list.find(',', value_start);
      value = std::string(
          trim(list.substr(value_start, comma == std::string_view::npos ? std::string_view::npos
                                                                        : comma - value_start)));
      pos = comma;
    }
    out[key] = value;
    if (pos == std::string_view::npos) {
      break;
    }
    ++pos; // past the comma
  }
  return out;
}

std::string_view after_tag(std::string_view line, std::string_view tag) {
  return line.substr(tag.size());
}

std::uint64_t to_u64(std::string_view s) {
  std::uint64_t v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}

// "0x0123..." → 16 raw bytes (shorter values are left-padded, as RFC 8216 numbers).
std::optional<std::string> parse_iv(std::string_view hex) {
  if (hex.starts_with("0x") || hex.starts_with("0X")) {
    hex.remove_prefix(2);
  }
  if (hex.empty() || hex.size() > 32) {
    return std::nullopt;
  }
  std::string padded = std::string(32 - hex.size(), '0') + std::string(hex);
  std::string bytes(16, '\0');
  for (std::size_t i = 0; i < 16; ++i) {
    unsigned value = 0;
    const auto [ptr, ec] =
        std::from_chars(padded.data() + 2 * i, padded.data() + 2 * i + 2, value, 16);
    if (ec != std::errc{} || ptr != padded.data() + 2 * i + 2) {
      return std::nullopt;
    }
    bytes[i] = static_cast<char>(value);
  }
  return bytes;
}

// "len@offset" or "len" (offset continues from `next_offset`).
ByteRange parse_byterange(std::string_view text, std::uint64_t next_offset) {
  const auto at = text.find('@');
  const std::uint64_t length = to_u64(text.substr(0, at));
  const std::uint64_t offset =
      at == std::string_view::npos ? next_offset : to_u64(text.substr(at + 1));
  return {offset, length};
}

Error drm_error(std::string_view what) {
  return Error{ErrorCode::Unsupported,
               "this stream is DRM-protected (" + std::string(what) +
                   "); Lanes does not download protected streams",
               0};
}

} // namespace

std::string resolve_url(std::string_view base, std::string_view uri) {
  net::ensure_curl_initialized();
  std::unique_ptr<CURLU, UrlDeleter> h(curl_url());
  const std::string b(base);
  const std::string u(uri);
  if (!h || curl_url_set(h.get(), CURLUPART_URL, b.c_str(), 0) != CURLUE_OK ||
      curl_url_set(h.get(), CURLUPART_URL, u.c_str(), 0) != CURLUE_OK) {
    return u;
  }
  char *raw = nullptr;
  if (curl_url_get(h.get(), CURLUPART_URL, &raw, 0) != CURLUE_OK) {
    return u;
  }
  std::string out(raw);
  curl_free(raw);
  return out;
}

bool is_master(std::string_view text) {
  return text.find("#EXT-X-STREAM-INF") != std::string_view::npos;
}

Result<MasterPlaylist> parse_master(std::string_view text, std::string_view base_url) {
  const auto lines = lines_of(text);
  if (lines.empty() || !lines.front().starts_with("#EXTM3U")) {
    return make_error(ErrorCode::InvalidArgument, "not an HLS playlist (no #EXTM3U)");
  }
  MasterPlaylist m;
  std::optional<Variant> pending;
  for (const auto line : lines) {
    if (line.starts_with("#EXT-X-STREAM-INF:")) {
      auto a = attributes(after_tag(line, "#EXT-X-STREAM-INF:"));
      Variant v;
      v.bandwidth = to_u64(a["BANDWIDTH"]);
      v.codecs = a["CODECS"];
      v.audio_group = a["AUDIO"];
      if (const auto &res = a["RESOLUTION"]; !res.empty()) {
        const auto x = res.find('x');
        v.width = static_cast<int>(to_u64(std::string_view(res).substr(0, x)));
        if (x != std::string::npos) {
          v.height = static_cast<int>(to_u64(std::string_view(res).substr(x + 1)));
        }
      }
      pending = v;
    } else if (line.starts_with("#EXT-X-MEDIA:")) {
      auto a = attributes(after_tag(line, "#EXT-X-MEDIA:"));
      Rendition r;
      r.type = a["TYPE"];
      r.group_id = a["GROUP-ID"];
      r.name = a["NAME"];
      r.language = a["LANGUAGE"];
      r.uri = a["URI"].empty() ? std::string() : resolve_url(base_url, a["URI"]);
      r.is_default = a["DEFAULT"] == "YES";
      m.renditions.push_back(std::move(r));
    } else if (!line.starts_with("#") && pending) {
      pending->uri = resolve_url(base_url, line);
      m.variants.push_back(std::move(*pending));
      pending.reset();
    }
  }
  if (m.variants.empty()) {
    return make_error(ErrorCode::InvalidArgument, "the HLS master playlist lists no streams");
  }
  return m;
}

Result<Track> parse_media(std::string_view text, std::string_view base_url) {
  const auto lines = lines_of(text);
  if (lines.empty() || !lines.front().starts_with("#EXTM3U")) {
    return make_error(ErrorCode::InvalidArgument, "not an HLS playlist (no #EXTM3U)");
  }
  Track track;
  std::uint64_t sequence = 0;
  int key = -1;
  bool ended = false;
  std::optional<ByteRange> pending_range;
  std::map<std::string, std::uint64_t> next_offset; // per URI, for "len" without "@offset"
  for (const auto line : lines) {
    if (line.starts_with("#EXT-X-MEDIA-SEQUENCE:")) {
      sequence = to_u64(after_tag(line, "#EXT-X-MEDIA-SEQUENCE:"));
    } else if (line.starts_with("#EXT-X-KEY:")) {
      auto a = attributes(after_tag(line, "#EXT-X-KEY:"));
      const std::string &method = a["METHOD"];
      if (method == "NONE") {
        key = -1;
        continue;
      }
      if (method != "AES-128") {
        return make_error(drm_error(method)); // SAMPLE-AES, SAMPLE-AES-CTR, ...
      }
      if (const auto &format = a["KEYFORMAT"]; !format.empty() && format != "identity") {
        return make_error(drm_error(format)); // FairPlay, Widevine, PlayReady key systems
      }
      Key k{resolve_url(base_url, a["URI"]), std::nullopt};
      if (const auto &iv = a["IV"]; !iv.empty()) {
        k.iv = parse_iv(iv);
        if (!k.iv) {
          return make_error(ErrorCode::InvalidArgument, "bad IV in #EXT-X-KEY");
        }
      }
      track.keys.push_back(std::move(k));
      key = static_cast<int>(track.keys.size()) - 1;
    } else if (line.starts_with("#EXT-X-SESSION-KEY:")) {
      auto a = attributes(after_tag(line, "#EXT-X-SESSION-KEY:"));
      if (a["METHOD"] != "NONE" && a["METHOD"] != "AES-128") {
        return make_error(drm_error(a["METHOD"]));
      }
    } else if (line.starts_with("#EXT-X-MAP:")) {
      if (track.init) {
        continue; // one initialization section (changes across discontinuities: not supported)
      }
      auto a = attributes(after_tag(line, "#EXT-X-MAP:"));
      Part init{resolve_url(base_url, a["URI"]), std::nullopt, key, sequence};
      if (const auto &br = a["BYTERANGE"]; !br.empty()) {
        init.range = parse_byterange(br, 0);
      }
      track.init = std::move(init);
    } else if (line.starts_with("#EXT-X-BYTERANGE:")) {
      pending_range = parse_byterange(after_tag(line, "#EXT-X-BYTERANGE:"), 0);
      if (after_tag(line, "#EXT-X-BYTERANGE:").find('@') == std::string_view::npos) {
        pending_range->first = UINT64_MAX; // resolved against the URI below
      }
    } else if (line.starts_with("#EXT-X-ENDLIST") || line == "#EXT-X-PLAYLIST-TYPE:VOD") {
      ended = true;
    } else if (!line.starts_with("#")) {
      Part part{resolve_url(base_url, line), std::nullopt, key, sequence++};
      if (pending_range) {
        if (pending_range->first == UINT64_MAX) {
          pending_range->first = next_offset[part.url];
        }
        next_offset[part.url] = pending_range->first + pending_range->second;
        part.range = pending_range;
        pending_range.reset();
      }
      track.parts.push_back(std::move(part));
    }
  }
  if (!ended) {
    return make_error(ErrorCode::Unsupported,
                      "this is a live stream (no #EXT-X-ENDLIST); live recording is not supported");
  }
  if (track.parts.empty()) {
    return make_error(ErrorCode::InvalidArgument, "the HLS playlist has no segments");
  }
  return track;
}

} // namespace lanes::core::media::hls
