// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "content.hpp"
#include "core/platform/file.hpp"
#include "core/platform/fs.hpp"
#include "core/platform/path.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
using namespace lanes::core;
using namespace lanes::core::platform;
using lanes::testserver::make_content;

namespace {

std::string read_all(const fs::path &path) {
  auto file = File::open(path, OpenMode::ReadOnly);
  REQUIRE(file);
  const auto size = file->size();
  REQUIRE(size);
  std::string data(static_cast<std::size_t>(*size), '\0');
  const auto n = file->read_at(0, data);
  REQUIRE(n);
  data.resize(*n);
  return data;
}

} // namespace

TEST_CASE("File writes and reads at offsets", "[platform][file]") {
  TempDir dir;
  const fs::path path = dir.path() / "a.part";
  auto file = File::open(path, OpenMode::CreateAlways);
  REQUIRE(file);

  REQUIRE(file->resize(100));
  REQUIRE(file->write_at(90, std::string_view("0123456789")));
  REQUIRE(file->write_at(0, std::string_view("head")));
  CHECK(*file->size() == 100);

  std::string buf(10, '\0');
  REQUIRE(*file->read_at(90, buf) == 10);
  CHECK(buf == "0123456789");
  REQUIRE(*file->read_at(4, buf) == 10);
  CHECK(buf == std::string(10, '\0')); // gap reads as zeros
  CHECK(*file->read_at(95, buf) == 5); // short read at end of file
  CHECK(*file->read_at(500, buf) == 0);
  REQUIRE(file->flush());
}

TEST_CASE("File::open modes and errors", "[platform][file]") {
  TempDir dir;
  const fs::path path = dir.path() / "b.bin";

  const auto missing = File::open(path, OpenMode::ReadOnly);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == ErrorCode::NotFound);
  CHECK(missing.error().message.find("b.bin") != std::string::npos);

  {
    auto created = File::open(path, OpenMode::ReadWrite); // creates
    REQUIRE(created);
    REQUIRE(created->write_at(0, std::string_view("keep")));
  }
  {
    auto reopened = File::open(path, OpenMode::ReadWrite); // keeps content (resume)
    REQUIRE(reopened);
    CHECK(*reopened->size() == 4);

    // A second writer is refused while the first is open: two downloads can't share a .part.
    const auto second = File::open(path, OpenMode::ReadWrite);
    REQUIRE_FALSE(second);
    CHECK(second.error().code == ErrorCode::AccessDenied);
  }
  {
    auto truncated = File::open(path, OpenMode::CreateAlways);
    REQUIRE(truncated);
    CHECK(*truncated->size() == 0);
  }
}

TEST_CASE("preallocate makes a sparse file that uses no disk space", "[platform][file]") {
  TempDir dir;
  const auto volume = volume_info(dir.path());
  REQUIRE(volume);
  if (!volume->supports_sparse) {
    SKIP("temp volume (" << volume->filesystem << ") has no sparse file support");
  }

  const fs::path path = dir.path() / "big.part";
  constexpr std::uint64_t kSize = 4ull << 30; // 4 GiB, instantly
  auto file = File::open(path, OpenMode::CreateAlways);
  REQUIRE(file);
  const auto sparse = file->preallocate(kSize);
  REQUIRE(sparse);
  CHECK(*sparse);
  CHECK(*file->size() == kSize);
  CHECK(*allocated_size(path) < (1u << 20));

  // Writing near the end does not zero-fill everything before it.
  const std::string tail = make_content(5, 0, 1 << 20);
  REQUIRE(file->write_at(kSize - tail.size(), tail));
  REQUIRE(file->flush());
  CHECK(*allocated_size(path) < (8u << 20));

  std::string back(tail.size(), '\0');
  REQUIRE(*file->read_at(kSize - tail.size(), back) == tail.size());
  CHECK(back == tail);

  REQUIRE(file->set_sparse(false)); // completion: becomes an ordinary file
}

TEST_CASE("concurrent positional writes from many threads", "[platform][file]") {
  TempDir dir;
  const fs::path path = dir.path() / "parallel.part";
  constexpr std::size_t kSegments = 16;
  constexpr std::size_t kSegmentSize = 1 << 20;
  constexpr std::size_t kChunk = 16 * 1024;
  const std::string expected = make_content(77, 0, kSegments * kSegmentSize);

  auto file = File::open(path, OpenMode::CreateAlways);
  REQUIRE(file);
  REQUIRE(file->preallocate(expected.size()));

  // Like download connections: each thread writes its own segment in small chunks.
  std::vector<std::thread> threads;
  std::vector<int> failures(kSegments, 0);
  for (std::size_t s = 0; s < kSegments; ++s) {
    threads.emplace_back([&, s] {
      for (std::size_t off = s * kSegmentSize; off < (s + 1) * kSegmentSize; off += kChunk) {
        if (!file->write_at(off, std::string_view(expected).substr(off, kChunk))) {
          ++failures[s];
        }
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  CHECK(failures == std::vector<int>(kSegments, 0));
  REQUIRE(file->flush());
  file->close();
  CHECK(read_all(path) == expected);
}

TEST_CASE("paths longer than MAX_PATH work", "[platform][file]") {
  TempDir dir;
  fs::path deep = dir.path();
  for (int i = 0; i < 8; ++i) {
    deep /= std::string(40, static_cast<char>('a' + i));
  }
  REQUIRE(path_to_utf8(deep).size() > 300);
  REQUIRE(platform::create_directories(deep));

  const fs::path path = deep / path_from_utf8(sanitize_filename("con.txt"));
  REQUIRE(write_file_atomic(path, "long path content"));
  CHECK(read_all(path) == "long path content");
}

TEST_CASE("write_file_atomic replaces content and leaves no temp file", "[platform][fs]") {
  TempDir dir;
  const fs::path path = dir.path() / "state.lanes.json";
  REQUIRE(write_file_atomic(path, "{\"v\":1}"));
  REQUIRE(write_file_atomic(path, "{\"v\":2}"));
  CHECK(read_all(path) == "{\"v\":2}");
  fs::path tmp = path;
  tmp += ".tmp";
  CHECK_FALSE(fs::exists(tmp));
}

TEST_CASE("atomic_replace overwrites the destination", "[platform][fs]") {
  TempDir dir;
  const fs::path from = dir.path() / "file.part";
  const fs::path to = dir.path() / "file.bin";
  REQUIRE(write_file_atomic(from, "new"));
  REQUIRE(write_file_atomic(to, "old"));
  REQUIRE(atomic_replace(from, to));
  CHECK(read_all(to) == "new");
  CHECK_FALSE(fs::exists(from));

  const auto missing = atomic_replace(dir.path() / "nope", to);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("volume_info and check_can_store", "[platform][fs]") {
  TempDir dir;
  const auto info = volume_info(dir.path() / "not" / "yet" / "created.bin");
  REQUIRE(info);
  CHECK_FALSE(info->filesystem.empty());
  CHECK(info->free_bytes > 0);
  CHECK(info->total_bytes >= info->free_bytes);
  CHECK(path_to_utf8(info->root).find("\\\\?\\") == std::string::npos);

  CHECK(check_can_store(dir.path(), 1024));
  const auto too_big = check_can_store(dir.path(), info->free_bytes + (1ull << 40));
  REQUIRE_FALSE(too_big);
  CHECK(too_big.error().code == ErrorCode::NoSpace);
  CHECK(too_big.error().message.find("free") != std::string::npos);
}

TEST_CASE("file write throughput", "[.][benchmark-disk]") {
  TempDir dir;
  constexpr std::size_t kChunk = 1 << 20;
  constexpr std::uint64_t kTotal = 256ull << 20; // per variant; keep SSD writes small
  const std::string chunk = make_content(1, 0, kChunk);
  for (const bool sparse : {false, true}) {
    auto file = File::open(dir.path() / (sparse ? "s.bin" : "n.bin"), OpenMode::CreateAlways);
    REQUIRE(file);
    if (sparse) {
      REQUIRE(file->preallocate(kTotal));
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t off = 0; off < kTotal; off += kChunk) {
      REQUIRE(file->write_at(off, chunk));
    }
    const double s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    WARN((sparse ? "sparse" : "plain") << " write_at 256 MiB: " << kTotal / s / 1e6 << " MB/s");
  }
}

TEST_CASE("FAT file size limit", "[platform][fs]") {
  CHECK(max_file_size_for_filesystem("FAT32") == 0xFFFFFFFFull);
  CHECK(max_file_size_for_filesystem("fat") == 0xFFFFFFFFull);
  CHECK(max_file_size_for_filesystem("NTFS") == UINT64_MAX);
  CHECK(max_file_size_for_filesystem("exFAT") == UINT64_MAX);
  CHECK(max_file_size_for_filesystem("ReFS") == UINT64_MAX);
}
