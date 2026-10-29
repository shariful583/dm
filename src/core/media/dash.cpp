// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/dash.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>

#include <pugixml.hpp>

#include "core/media/hls.hpp" // resolve_url

namespace lanes::core::media::dash {

namespace {

constexpr std::size_t kMaxSegments = 200'000;

std::uint64_t to_u64(std::string_view s, std::uint64_t fallback = 0) {
  std::uint64_t v = fallback;
  if (!s.empty()) {
    std::from_chars(s.data(), s.data() + s.size(), v);
  }
  return v;
}

std::optional<ByteRange> parse_range(std::string_view text) { // "first-last"
  const auto dash = text.find('-');
  if (text.empty() || dash == std::string_view::npos) {
    return std::nullopt;
  }
  const std::uint64_t first = to_u64(text.substr(0, dash));
  const std::uint64_t last = to_u64(text.substr(dash + 1));
  if (last < first) {
    return std::nullopt;
  }
  return ByteRange{first, last - first + 1};
}

// Follows BaseURL elements: each one may be relative to the previous.
std::string with_base(const std::string &base, const pugi::xml_node &node) {
  const pugi::xml_node b = node.child("BaseURL");
  return b ? hls::resolve_url(base, b.text().get()) : base;
}

// SegmentTemplate attributes; the Representation's override the AdaptationSet's.
struct Template {
  std::string media;
  std::string initialization;
  std::uint64_t start_number = 1;
  std::uint64_t timescale = 1;
  std::uint64_t duration = 0;
  pugi::xml_node timeline;
  bool present = false;

  void merge(const pugi::xml_node &t) {
    if (!t) {
      return;
    }
    present = true;
    if (auto a = t.attribute("media")) {
      media = a.value();
    }
    if (auto a = t.attribute("initialization")) {
      initialization = a.value();
    }
    if (auto a = t.attribute("startNumber")) {
      start_number = to_u64(a.value(), 1);
    }
    if (auto a = t.attribute("timescale")) {
      timescale = std::max<std::uint64_t>(1, to_u64(a.value(), 1));
    }
    if (auto a = t.attribute("duration")) {
      duration = to_u64(a.value());
    }
    if (auto tl = t.child("SegmentTimeline")) {
      timeline = tl;
    }
  }
};

// Expands $RepresentationID$, $Number$, $Time$, $Bandwidth$ (with optional %0Nd) and $$.
std::string fill(const std::string &pattern, const std::string &id, std::uint64_t number,
                 std::uint64_t time, std::uint64_t bandwidth) {
  std::string out;
  for (std::size_t i = 0; i < pattern.size();) {
    if (pattern[i] != '$') {
      out += pattern[i++];
      continue;
    }
    const auto close = pattern.find('$', i + 1);
    if (close == std::string::npos) {
      out += pattern.substr(i);
      break;
    }
    const std::string token = pattern.substr(i + 1, close - i - 1);
    i = close + 1;
    if (token.empty()) {
      out += '$';
      continue;
    }
    const auto percent = token.find('%');
    const std::string name = token.substr(0, percent);
    int width = 0;
    if (percent != std::string::npos) { // "%05d"
      std::string_view f(token);
      f.remove_prefix(percent + 1);
      if (!f.empty() && f.front() == '0') {
        f.remove_prefix(1);
      }
      std::from_chars(f.data(), f.data() + f.size(), width);
    }
    std::string value;
    if (name == "RepresentationID") {
      value = id;
    } else if (name == "Number") {
      value = std::to_string(number);
    } else if (name == "Time") {
      value = std::to_string(time);
    } else if (name == "Bandwidth") {
      value = std::to_string(bandwidth);
    } else {
      value = "$" + token + "$";
    }
    if (width > 0 && name != "RepresentationID" && value.size() < static_cast<std::size_t>(width)) {
      value.insert(0, static_cast<std::size_t>(width) - value.size(), '0');
    }
    out += value;
  }
  return out;
}

Result<Track> template_track(const Template &t, const std::string &base, const std::string &id,
                             std::uint64_t bandwidth, double period_seconds) {
  Track track;
  if (!t.initialization.empty()) {
    track.init = Part{hls::resolve_url(base, fill(t.initialization, id, 0, 0, bandwidth))};
  }
  std::uint64_t number = t.start_number;
  if (t.timeline) {
    std::uint64_t time = 0;
    const double period_end = period_seconds * static_cast<double>(t.timescale);
    for (const auto &s : t.timeline.children("S")) {
      if (auto a = s.attribute("t")) {
        time = to_u64(a.value());
      }
      const std::uint64_t d = to_u64(s.attribute("d").value());
      if (d == 0) {
        return make_error(ErrorCode::InvalidArgument, "SegmentTimeline entry without a duration");
      }
      long long repeat = s.attribute("r").as_llong(0);
      if (repeat < 0) { // until the end of the period
        repeat = period_end > static_cast<double>(time)
                     ? static_cast<long long>(std::ceil((period_end - static_cast<double>(time)) /
                                                        static_cast<double>(d))) -
                           1
                     : 0;
      }
      for (long long k = 0; k <= repeat; ++k) {
        track.parts.push_back(
            Part{hls::resolve_url(base, fill(t.media, id, number++, time, bandwidth))});
        time += d;
        if (track.parts.size() > kMaxSegments) {
          return make_error(ErrorCode::InvalidArgument, "too many DASH segments");
        }
      }
    }
  } else if (t.duration > 0) {
    if (period_seconds <= 0) {
      return make_error(ErrorCode::InvalidArgument, "DASH manifest without a duration");
    }
    const double segment_seconds =
        static_cast<double>(t.duration) / static_cast<double>(t.timescale);
    const auto count =
        static_cast<std::uint64_t>(std::ceil(period_seconds / segment_seconds - 1e-9));
    if (count > kMaxSegments) {
      return make_error(ErrorCode::InvalidArgument, "too many DASH segments");
    }
    for (std::uint64_t k = 0; k < count; ++k) {
      track.parts.push_back(
          Part{hls::resolve_url(base, fill(t.media, id, number++, k * t.duration, bandwidth))});
    }
  } else {
    return make_error(ErrorCode::InvalidArgument,
                      "SegmentTemplate without SegmentTimeline or duration");
  }
  return track;
}

std::string kind_of(const pugi::xml_node &as, const pugi::xml_node &rep) {
  for (const char *attr : {"contentType", "mimeType"}) {
    for (const auto &node : {rep, as}) {
      const std::string v = node.attribute(attr).value();
      for (const char *k : {"video", "audio", "text"}) {
        if (v.starts_with(k)) {
          return k;
        }
      }
    }
  }
  return {};
}

} // namespace

double parse_iso_duration(std::string_view text) {
  if (!text.starts_with("P")) {
    return -1;
  }
  text.remove_prefix(1);
  double total = 0;
  bool in_time = false;
  while (!text.empty()) {
    if (text.front() == 'T') {
      in_time = true;
      text.remove_prefix(1);
      continue;
    }
    std::size_t n = 0;
    while (n < text.size() && ((text[n] >= '0' && text[n] <= '9') || text[n] == '.')) {
      ++n;
    }
    if (n == 0 || n == text.size()) {
      return -1;
    }
    double value = 0;
    if (std::from_chars(text.data(), text.data() + n, value).ec != std::errc{}) {
      return -1;
    }
    const char unit = text[n];
    if (unit == 'D' && !in_time) {
      total += value * 86400;
    } else if (unit == 'H' && in_time) {
      total += value * 3600;
    } else if (unit == 'M' && in_time) {
      total += value * 60;
    } else if (unit == 'S' && in_time) {
      total += value;
    } else {
      return -1; // years/months are not used for media durations
    }
    text.remove_prefix(n + 1);
  }
  return total;
}

Result<Manifest> parse_mpd(std::string_view xml, std::string_view mpd_url) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) {
    return make_error(ErrorCode::InvalidArgument, "not a DASH manifest (invalid XML)");
  }
  const pugi::xml_node mpd = doc.child("MPD");
  if (!mpd) {
    return make_error(ErrorCode::InvalidArgument, "not a DASH manifest (no <MPD>)");
  }
  if (std::string_view(mpd.attribute("type").value()) == "dynamic") {
    return make_error(ErrorCode::Unsupported,
                      "this is a live DASH stream; live recording is not supported");
  }
  const pugi::xml_node period = mpd.child("Period");
  if (!period) {
    return make_error(ErrorCode::InvalidArgument, "the DASH manifest has no <Period>");
  }
  Manifest manifest;
  manifest.duration_seconds =
      parse_iso_duration(mpd.attribute("mediaPresentationDuration").value());
  if (manifest.duration_seconds <= 0) {
    manifest.duration_seconds = parse_iso_duration(period.attribute("duration").value());
  }
  const std::string period_base = with_base(with_base(std::string(mpd_url), mpd), period);

  for (const auto &as : period.children("AdaptationSet")) {
    const std::string as_base = with_base(period_base, as);
    Template as_template;
    as_template.merge(as.child("SegmentTemplate"));
    for (const auto &rep : as.children("Representation")) {
      if (as.child("ContentProtection") || rep.child("ContentProtection")) {
        return make_error(ErrorCode::Unsupported, "this stream is DRM-protected "
                                                  "(ContentProtection); Lanes does not download "
                                                  "protected streams");
      }
      Representation r;
      r.id = rep.attribute("id").value();
      r.kind = kind_of(as, rep);
      r.bandwidth = to_u64(rep.attribute("bandwidth").value());
      r.width = rep.attribute("width").as_int(as.attribute("width").as_int(0));
      r.height = rep.attribute("height").as_int(as.attribute("height").as_int(0));
      r.codecs = rep.attribute("codecs") ? rep.attribute("codecs").value()
                                         : as.attribute("codecs").value();
      r.mime_type = rep.attribute("mimeType") ? rep.attribute("mimeType").value()
                                              : as.attribute("mimeType").value();
      r.language = as.attribute("lang").value();
      const std::string base = with_base(as_base, rep);

      Template t = as_template;
      t.merge(rep.child("SegmentTemplate"));
      const pugi::xml_node list =
          rep.child("SegmentList") ? rep.child("SegmentList") : as.child("SegmentList");
      if (t.present) {
        auto track = template_track(t, base, r.id, r.bandwidth, manifest.duration_seconds);
        if (!track) {
          return make_error(track.error());
        }
        r.track = std::move(*track);
      } else if (list) {
        if (const auto init = list.child("Initialization")) {
          const char *source = init.attribute("sourceURL").value();
          r.track.init = Part{*source != '\0' ? hls::resolve_url(base, source) : base,
                              parse_range(init.attribute("range").value())};
        }
        for (const auto &seg : list.children("SegmentURL")) {
          const char *media = seg.attribute("media").value();
          r.track.parts.push_back(Part{*media != '\0' ? hls::resolve_url(base, media) : base,
                                       parse_range(seg.attribute("mediaRange").value())});
        }
      } else {
        r.track.parts.push_back(Part{base}); // one self-contained file (SegmentBase / plain)
      }
      if (r.track.parts.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "DASH representation '" + r.id + "' has no segments");
      }
      manifest.representations.push_back(std::move(r));
    }
  }
  if (manifest.representations.empty()) {
    return make_error(ErrorCode::InvalidArgument, "the DASH manifest lists no streams");
  }
  return manifest;
}

} // namespace lanes::core::media::dash
