// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

#include "core/error.hpp"

namespace lanes::core::verify {

enum class HashAlgorithm { Md5, Sha256 };

// An expected file hash (checksum verification).
struct Checksum {
  HashAlgorithm algorithm = HashAlgorithm::Sha256;
  std::string hex; // lowercase
};

std::string_view to_string(HashAlgorithm algorithm) noexcept; // "MD5", "SHA-256"

// Validates and normalizes a hex digest for `algorithm` (length, hex digits; case-insensitive).
// InvalidArgument otherwise. Thread-safe.
Result<Checksum> make_checksum(HashAlgorithm algorithm, std::string_view hex);

// Lowercase hex digest of the file's content. Cancelled if `stop` is requested. Thread-safe.
Result<std::string> hash_file(const std::filesystem::path &path, HashAlgorithm algorithm,
                              std::stop_token stop = {});

} // namespace lanes::core::verify
