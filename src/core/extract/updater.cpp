// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/extract/updater.hpp"

#include <charconv>
#include <ctime>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/engine/download.hpp"
#include "core/extract/ytdlp.hpp"
#include "core/media/http.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "core/verify/checksum.hpp"

namespace lanes::core::extract {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

std::vector<long long> parts(std::string_view v) {
  std::vector<long long> out;
  while (!v.empty()) {
    const auto dot = v.find('.');
    long long n = 0;
    const std::string_view piece = v.substr(0, dot);
    std::from_chars(piece.data(), piece.data() + piece.size(), n);
    out.push_back(n);
    if (dot == std::string_view::npos) {
      break;
    }
    v.remove_prefix(dot + 1);
  }
  return out;
}

std::int64_t now_seconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

Result<std::string> fetch_text(const std::string &url, std::size_t max) {
  net::RequestOptions r;
  r.url = url;
  r.headers = {{"Accept", "application/vnd.github+json"}};
  auto got = media::fetch_small(r, max);
  if (!got) {
    return make_error(got.error());
  }
  return std::move(got->body);
}

} // namespace

bool newer_version(std::string_view a, std::string_view b) {
  const auto pa = parts(a);
  const auto pb = parts(b);
  return pa > pb; // lexicographic over the numbers
}

Result<UpdateResult> update_ytdlp(const UpdateOptions &options, std::stop_token stop) {
  const fs::path folder = options.folder.empty() ? tools_dir() : options.folder;
  if (auto ok = platform::create_directories(folder); !ok) {
    return make_error(ok.error());
  }
  const fs::path state_path = folder / "update.json";
  UpdateResult result;

  auto current = find_ytdlp();
  std::string current_version;
  if (current) {
    if (auto v = ytdlp_version(*current)) {
      current_version = *v;
    }
  }
  result.version = current_version;

  if (!options.force) {
    if (auto text = platform::read_small_file(state_path, 4096)) {
      const json state = json::parse(*text, nullptr, false);
      const auto last = state.is_object() ? state.value("last_check", std::int64_t{0}) : 0;
      if (now_seconds() - last < std::chrono::seconds(options.min_interval).count()) {
        return result; // checked recently
      }
    }
  }
  result.checked = true;

  auto release_text = fetch_text(options.releases_url, 4u << 20);
  if (!release_text) {
    return make_error(release_text.error());
  }
  const json release = json::parse(*release_text, nullptr, false);
  if (!release.is_object() || !release.contains("tag_name")) {
    return make_error(ErrorCode::InvalidArgument, "unexpected answer from the release server");
  }
  const std::string tag = release.value("tag_name", std::string());
  std::string exe_url;
  std::string sums_url;
  for (const auto &asset : release.value("assets", json::array())) {
    const std::string name = asset.value("name", std::string());
    if (name == "yt-dlp.exe") {
      exe_url = asset.value("browser_download_url", std::string());
    } else if (name == "SHA2-256SUMS") {
      sums_url = asset.value("browser_download_url", std::string());
    }
  }
  (void)platform::write_file_atomic(state_path,
                                    json{{"last_check", now_seconds()}, {"latest", tag}}.dump());
  if (!current_version.empty() && !newer_version(tag, current_version)) {
    return result; // up to date
  }
  if (exe_url.empty() || sums_url.empty()) {
    return make_error(ErrorCode::NotFound, "the yt-dlp release has no yt-dlp.exe or checksums");
  }

  // The expected checksum: "<hex>  yt-dlp.exe".
  auto sums = fetch_text(sums_url, 1u << 20);
  if (!sums) {
    return make_error(sums.error());
  }
  std::string expected;
  std::size_t pos = 0;
  while (pos < sums->size()) {
    auto end = sums->find('\n', pos);
    if (end == std::string::npos) {
      end = sums->size();
    }
    std::string line = sums->substr(pos, end - pos);
    pos = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    const auto space = line.find(' ');
    if (space != std::string::npos && line.ends_with(" yt-dlp.exe") &&
        line.substr(line.find_last_of(" *") + 1) == "yt-dlp.exe") {
      expected = line.substr(0, space);
      break;
    }
  }
  if (expected.size() != 64) {
    return make_error(ErrorCode::NotFound, "no checksum for yt-dlp.exe in the release");
  }

  engine::DownloadOptions download;
  download.request.url = exe_url;
  download.directory = folder;
  download.filename = "yt-dlp.exe.new";
  download.if_file_exists = engine::ExistingFile::Overwrite;
  download.connections = 4;
  auto checksum = verify::make_checksum(verify::HashAlgorithm::Sha256, expected);
  if (!checksum) {
    return make_error(checksum.error());
  }
  download.checksum = *checksum; // a mismatch fails here, before anything is replaced
  auto fetched = engine::download_file(download, stop);
  if (!fetched) {
    (void)platform::remove_file(folder / "yt-dlp.exe.new.part");
    return make_error(fetched.error());
  }

  const fs::path target = folder / "yt-dlp.exe";
  const fs::path previous = folder / "yt-dlp.exe.old";
  std::error_code ec;
  if (fs::exists(target, ec)) {
    if (auto ok = platform::atomic_replace(target, previous); !ok) {
      return make_error(ok.error());
    }
  }
  if (auto ok = platform::atomic_replace(fetched->path, target); !ok) {
    return make_error(ok.error());
  }
  auto version = ytdlp_version(target);
  if (!version) { // it does not run: go back
    (void)rollback_ytdlp(folder);
    return make_error(ErrorCode::Unknown, "the new yt-dlp does not run; kept the previous one");
  }
  result.updated = true;
  result.version = *version;
  return result;
}

Result<void> rollback_ytdlp(const fs::path &given) {
  const fs::path folder = given.empty() ? tools_dir() : given;
  const fs::path target = folder / "yt-dlp.exe";
  const fs::path previous = folder / "yt-dlp.exe.old";
  std::error_code ec;
  if (fs::exists(previous, ec)) {
    return platform::atomic_replace(previous, target);
  }
  if (fs::exists(target, ec)) {
    return platform::remove_file(target); // back to the yt-dlp bundled with Lanes
  }
  return make_error(ErrorCode::NotFound, "there is no earlier yt-dlp to go back to");
}

} // namespace lanes::core::extract
