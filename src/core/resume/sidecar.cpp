// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/resume/sidecar.hpp"

#include <system_error>

#include <nlohmann/json.hpp>

#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"

namespace lanes::core::resume {

std::uint64_t ResumeState::done_bytes() const {
  std::uint64_t total = 0;
  for (const auto &s : segments) {
    total += s.done;
  }
  return total;
}

std::filesystem::path sidecar_path(const std::filesystem::path &final_path) {
  std::filesystem::path p = final_path;
  p += ".lanes.json";
  return p;
}

std::string to_json(const ResumeState &state) {
  nlohmann::json segments = nlohmann::json::array();
  for (const auto &s : state.segments) {
    segments.push_back({s.start, s.end, s.done});
  }
  const nlohmann::json j = {
      {"version", ResumeState::kVersion},
      {"url", state.url},
      {"final_url", state.final_url},
      {"filename", state.filename},
      {"size", state.size},
      {"etag", state.etag},
      {"last_modified", state.last_modified},
      {"segments", segments}, // [start, end, done] triples
  };
  return j.dump(1);
}

Result<ResumeState> from_json(std::string_view json) {
  const auto invalid = [](const std::string &why) {
    return make_error(ErrorCode::InvalidArgument, "invalid resume file: " + why);
  };
  const nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return invalid("not JSON");
  }
  try {
    if (j.at("version").get<int>() != ResumeState::kVersion) {
      return invalid("unknown version");
    }
    ResumeState state;
    state.url = j.at("url").get<std::string>();
    state.final_url = j.at("final_url").get<std::string>();
    state.filename = j.at("filename").get<std::string>();
    state.size = j.at("size").get<std::uint64_t>();
    state.etag = j.at("etag").get<std::string>();
    state.last_modified = j.at("last_modified").get<std::string>();
    std::uint64_t next = 0;
    for (const auto &s : j.at("segments")) {
      if (!s.is_array() || s.size() != 3) {
        return invalid("bad segment");
      }
      SavedSegment seg{s[0].get<std::uint64_t>(), s[1].get<std::uint64_t>(),
                       s[2].get<std::uint64_t>()};
      if (seg.start != next || seg.end < seg.start || seg.end >= state.size ||
          seg.done > seg.length()) {
        return invalid("segments do not tile the file");
      }
      next = seg.end + 1;
      state.segments.push_back(seg);
    }
    if (state.size == 0 || next != state.size || state.filename.empty()) {
      return invalid("segments do not cover the file");
    }
    return state;
  } catch (const nlohmann::json::exception &e) {
    return invalid(e.what());
  }
}

Result<void> save(const std::filesystem::path &sidecar, const ResumeState &state) {
  return platform::write_file_atomic(sidecar, to_json(state));
}

Result<std::optional<ResumeState>> load(const std::filesystem::path &sidecar) {
  std::error_code ec;
  if (!std::filesystem::exists(platform::to_long_path(sidecar), ec)) {
    return std::optional<ResumeState>{};
  }
  auto text = platform::read_small_file(sidecar, 4 << 20);
  if (!text) {
    return make_error(text.error());
  }
  auto state = from_json(*text);
  if (!state) {
    return make_error(state.error());
  }
  return std::optional<ResumeState>(std::move(*state));
}

} // namespace lanes::core::resume
