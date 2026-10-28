// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/verify/checksum.hpp"

#include <array>
#include <memory>

#include <openssl/evp.h>

#include "core/platform/file.hpp"

namespace lanes::core::verify {

namespace {

struct MdCtxDeleter {
  void operator()(EVP_MD_CTX *ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};

std::size_t digest_hex_length(HashAlgorithm algorithm) {
  return algorithm == HashAlgorithm::Md5 ? 32 : 64;
}

} // namespace

std::string_view to_string(HashAlgorithm algorithm) noexcept {
  return algorithm == HashAlgorithm::Md5 ? "MD5" : "SHA-256";
}

Result<Checksum> make_checksum(HashAlgorithm algorithm, std::string_view hex) {
  std::string normalized;
  for (const char c : hex) {
    if (c >= 'A' && c <= 'F') {
      normalized += static_cast<char>(c - 'A' + 'a');
    } else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
      normalized += c;
    } else {
      return make_error(ErrorCode::InvalidArgument,
                        std::string(to_string(algorithm)) + " must be hexadecimal");
    }
  }
  if (normalized.size() != digest_hex_length(algorithm)) {
    return make_error(ErrorCode::InvalidArgument, std::string(to_string(algorithm)) + " must be " +
                                                      std::to_string(digest_hex_length(algorithm)) +
                                                      " hex digits");
  }
  return Checksum{algorithm, normalized};
}

Result<std::string> hash_file(const std::filesystem::path &path, HashAlgorithm algorithm,
                              std::stop_token stop) {
  auto file = platform::File::open(path, platform::OpenMode::ReadOnly);
  if (!file) {
    return make_error(file.error());
  }
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
  const EVP_MD *md = algorithm == HashAlgorithm::Md5 ? EVP_md5() : EVP_sha256();
  if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) {
    return make_error(ErrorCode::Unknown, "cannot initialize hashing");
  }
  std::string buffer(4 << 20, '\0');
  for (std::uint64_t offset = 0;;) {
    if (stop.stop_requested()) {
      return make_error(ErrorCode::Cancelled, "verification cancelled");
    }
    auto read = file->read_at(offset, buffer);
    if (!read) {
      return make_error(read.error());
    }
    if (*read == 0) {
      break;
    }
    EVP_DigestUpdate(ctx.get(), buffer.data(), *read);
    offset += *read;
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  EVP_DigestFinal_ex(ctx.get(), digest.data(), &size);
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  for (unsigned int i = 0; i < size; ++i) {
    hex += kDigits[digest[i] >> 4];
    hex += kDigits[digest[i] & 0x0F];
  }
  return hex;
}

} // namespace lanes::core::verify
