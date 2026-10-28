// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "core/error.hpp"

namespace lanes::core::platform {

enum class OpenMode {
  ReadOnly,     // must exist; other processes may read, write and delete
  ReadWrite,    // open existing or create; other processes may only read
  CreateAlways, // create or truncate; other processes may only read
};

// A file opened for positional I/O, as used for `.part` files: each connection writes at its own
// offset. Paths are converted with to_long_path, so they may exceed MAX_PATH.
//
// Thread safety: write_at, read_at, flush and size may be called concurrently from several threads
// on the same File. open, resize, set_sparse, preallocate, close and moves must not overlap with
// any other call.
class File {
public:
  static Result<File> open(const std::filesystem::path &path, OpenMode mode);

  File() = default;
  ~File();
  File(File &&other) noexcept;
  File &operator=(File &&other) noexcept;
  File(const File &) = delete;
  File &operator=(const File &) = delete;

  bool is_open() const noexcept;
  const std::filesystem::path &path() const noexcept { return path_; }

  Result<std::uint64_t> size() const;
  // Sets the file length; growing leaves the new range unwritten (reads as zeros).
  Result<void> resize(std::uint64_t size);

  // Turns the sparse attribute on or off. Returns false (not an error) if the file system has no
  // sparse support (FAT32, exFAT). Turning it off on a fully written file is cheap and leaves an
  // ordinary file.
  Result<bool> set_sparse(bool sparse);

  // Sizes the file to `size` without writing zeros: sparse where supported, so unwritten ranges
  // take no disk space and writing at a high offset does not first zero-fill everything before
  // it. Returns whether the file is sparse.
  Result<bool> preallocate(std::uint64_t size);

  // Writes all of `data` at `offset`. Does not move any shared file position.
  Result<void> write_at(std::uint64_t offset, std::span<const char> data);
  // Reads up to out.size() bytes at `offset`; returns the count (less at end of file).
  Result<std::size_t> read_at(std::uint64_t offset, std::span<char> out) const;

  // Forces written data to stable storage (FlushFileBuffers / fdatasync).
  Result<void> flush();

  void close() noexcept;

private:
  std::filesystem::path path_;
#ifdef _WIN32
  void *handle_ = nullptr; // HANDLE
#else
  int fd_ = -1;
#endif
};

} // namespace lanes::core::platform
