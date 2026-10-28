// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace lanes::core::platform {

struct VolumeInfo {
  std::filesystem::path root;   // volume mount point, e.g. "C:\"
  std::string filesystem;       // "NTFS", "ReFS", "FAT32", "exFAT", ...
  std::uint64_t free_bytes = 0; // available to the current user (respects quotas)
  std::uint64_t total_bytes = 0;
  bool supports_sparse = false;
  std::uint64_t max_file_size = std::numeric_limits<std::uint64_t>::max();
};

// Volume holding `path`. `path` need not exist; its nearest existing ancestor is used.
// Thread-safe.
Result<VolumeInfo> volume_info(const std::filesystem::path &path);

// Largest file the named file system can hold (FAT/FAT32: 4 GiB - 1; otherwise unlimited).
// Case-insensitive. Thread-safe (pure function).
std::uint64_t max_file_size_for_filesystem(std::string_view filesystem);

// Checks that a file of `size` bytes fits at `path`: NoSpace if the volume lacks free space,
// FileTooLarge if the file system cannot hold a file that big. Messages are user-facing.
// Thread-safe.
Result<void> check_can_store(const std::filesystem::path &path, std::uint64_t size);

// Creates `dir` and missing parents (long-path aware). Succeeds if it already exists.
Result<void> create_directories(const std::filesystem::path &dir);

// Renames `from` to `to`, replacing `to` if it exists, atomically and durably
// (MoveFileExW REPLACE_EXISTING | WRITE_THROUGH / rename). Retries briefly while another process
// (antivirus, indexer) holds the file. Thread-safe.
Result<void> atomic_replace(const std::filesystem::path &from, const std::filesystem::path &to);

// Writes `data` to `<path>.tmp`, flushes it to disk, then atomic_replace()s it onto `path`, so
// `path` always holds either the old or the complete new content. Thread-safe for distinct paths.
Result<void> write_file_atomic(const std::filesystem::path &path, std::string_view data);

// Bytes the file actually occupies on disk (less than its size while sparse). Thread-safe.
Result<std::uint64_t> allocated_size(const std::filesystem::path &path);

} // namespace lanes::core::platform
