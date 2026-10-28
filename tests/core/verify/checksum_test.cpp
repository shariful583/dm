// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "../platform/temp_dir.hpp"
#include "core/platform/fs.hpp"
#include "core/verify/checksum.hpp"

using namespace lanes::core;
using namespace lanes::core::verify;

TEST_CASE("file hashes match known answers", "[verify]") {
  TempDir dir;
  const auto path = dir.path() / "abc.txt";
  REQUIRE(platform::write_file_atomic(path, "abc"));
  CHECK(*hash_file(path, HashAlgorithm::Sha256) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(*hash_file(path, HashAlgorithm::Md5) == "900150983cd24fb0d6963f7d28e17f72");

  const auto empty = dir.path() / "empty";
  REQUIRE(platform::write_file_atomic(empty, ""));
  CHECK(*hash_file(empty, HashAlgorithm::Md5) == "d41d8cd98f00b204e9800998ecf8427e");

  CHECK(hash_file(dir.path() / "missing", HashAlgorithm::Md5).error().code == ErrorCode::NotFound);
}

TEST_CASE("checksum input is validated and normalized", "[verify]") {
  const auto upper = make_checksum(HashAlgorithm::Md5, "900150983CD24FB0D6963F7D28E17F72");
  REQUIRE(upper);
  CHECK(upper->hex == "900150983cd24fb0d6963f7d28e17f72");
  CHECK_FALSE(make_checksum(HashAlgorithm::Md5, "1234"));
  CHECK_FALSE(make_checksum(HashAlgorithm::Sha256, std::string(64, 'g')));
  CHECK_FALSE(make_checksum(HashAlgorithm::Sha256, std::string(32, 'a')));
}
