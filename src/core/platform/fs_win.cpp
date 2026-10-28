// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/fs.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <thread>

#include "core/platform/file.hpp"
#include "core/platform/path.hpp"
#include "core/platform/win/win32.hpp"

namespace lanes::core::platform {

namespace {

std::string format_bytes(std::uint64_t bytes) {
  static constexpr std::array<const char *, 5> kUnits{"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < kUnits.size()) {
    value /= 1024.0;
    ++unit;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
  return buf;
}

// Nearest existing ancestor of `path` (or `path` itself), as an absolute path.
std::filesystem::path existing_ancestor(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::path current = std::filesystem::absolute(path, ec);
  if (ec) {
    current = path;
  }
  while (!current.empty() && !std::filesystem::exists(to_long_path(current), ec)) {
    const auto parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
  return current;
}

bool is_transient_lock(DWORD code) {
  return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ||
         code == ERROR_LOCK_VIOLATION;
}

} // namespace

std::uint64_t max_file_size_for_filesystem(std::string_view filesystem) {
  std::string upper;
  for (const char c : filesystem) {
    upper += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  if (upper == "FAT" || upper == "FAT12" || upper == "FAT16" || upper == "FAT32") {
    return 0xFFFFFFFFull; // 4 GiB - 1
  }
  return std::numeric_limits<std::uint64_t>::max();
}

Result<VolumeInfo> volume_info(const std::filesystem::path &path) {
  const std::filesystem::path existing = existing_ancestor(path);

  // Plain form when it fits, so the reported root reads "C:\" rather than "\\?\C:\".
  const std::filesystem::path query =
      existing.native().size() < MAX_PATH ? existing : to_long_path(existing);
  std::array<wchar_t, MAX_PATH + 1> root{};
  if (!::GetVolumePathNameW(query.c_str(), root.data(), static_cast<DWORD>(root.size()))) {
    return make_error(win::last_error("find volume", path));
  }

  VolumeInfo info;
  info.root = std::filesystem::path(root.data());

  std::array<wchar_t, MAX_PATH + 1> fs_name{};
  DWORD flags = 0;
  if (!::GetVolumeInformationW(root.data(), nullptr, 0, nullptr, nullptr, &flags, fs_name.data(),
                               static_cast<DWORD>(fs_name.size()))) {
    return make_error(win::last_error("query volume", info.root));
  }
  info.filesystem = win::narrow(fs_name.data());
  info.supports_sparse = (flags & FILE_SUPPORTS_SPARSE_FILES) != 0;
  info.max_file_size = max_file_size_for_filesystem(info.filesystem);

  ULARGE_INTEGER free_to_caller{};
  ULARGE_INTEGER total{};
  if (!::GetDiskFreeSpaceExW(root.data(), &free_to_caller, &total, nullptr)) {
    return make_error(win::last_error("query free space", info.root));
  }
  info.free_bytes = free_to_caller.QuadPart;
  info.total_bytes = total.QuadPart;
  return info;
}

Result<void> check_can_store(const std::filesystem::path &path, std::uint64_t size) {
  auto info = volume_info(path);
  if (!info) {
    return make_error(info.error());
  }
  const std::string where = path_to_utf8(info->root);
  if (size > info->max_file_size) {
    return make_error(ErrorCode::FileTooLarge, "The file (" + format_bytes(size) +
                                                   ") is too large for the " + info->filesystem +
                                                   " drive " + where +
                                                   ", which can only hold files up "
                                                   "to " +
                                                   format_bytes(info->max_file_size) +
                                                   ". Choose a folder on an NTFS or exFAT drive.");
  }
  if (size > info->free_bytes) {
    return make_error(ErrorCode::NoSpace, "Not enough free space on " + where +
                                              ": the file needs " + format_bytes(size) + ", only " +
                                              format_bytes(info->free_bytes) + " is free.");
  }
  return {};
}

Result<void> create_directories(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(to_long_path(dir), ec);
  if (ec) {
    return make_error(win::win32_error(static_cast<DWORD>(ec.value()), "create folder", dir));
  }
  return {};
}

Result<void> atomic_replace(const std::filesystem::path &from, const std::filesystem::path &to) {
  const std::wstring src = to_long_path(from).native();
  const std::wstring dst = to_long_path(to).native();
  constexpr int kAttempts = 20;
  for (int attempt = 1;; ++attempt) {
    if (::MoveFileExW(src.c_str(), dst.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return {};
    }
    const DWORD code = ::GetLastError();
    if (!is_transient_lock(code) || attempt == kAttempts) {
      return make_error(win::win32_error(code, "replace file", to));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25 * attempt));
  }
}

Result<void> write_file_atomic(const std::filesystem::path &path, std::string_view data) {
  std::filesystem::path tmp = path;
  tmp += L".tmp";
  {
    auto file = File::open(tmp, OpenMode::CreateAlways);
    if (!file) {
      return make_error(file.error());
    }
    if (auto written = file->write_at(0, data); !written) {
      return written;
    }
    if (auto flushed = file->flush(); !flushed) {
      return flushed;
    }
  }
  return atomic_replace(tmp, path);
}

Result<std::uint64_t> allocated_size(const std::filesystem::path &path) {
  DWORD high = 0;
  ::SetLastError(NO_ERROR);
  const DWORD low = ::GetCompressedFileSizeW(to_long_path(path).c_str(), &high);
  if (low == INVALID_FILE_SIZE && ::GetLastError() != NO_ERROR) {
    return make_error(win::last_error("get allocated size", path));
  }
  return (static_cast<std::uint64_t>(high) << 32) | low;
}

} // namespace lanes::core::platform
