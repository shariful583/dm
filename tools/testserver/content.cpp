// SPDX-License-Identifier: GPL-3.0-or-later
#include "content.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace lanes::testserver {

namespace {

constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// Word `block` (8 bytes, little-endian) of the file with `seed`.
constexpr std::uint64_t content_word(std::uint64_t seed, std::uint64_t block) noexcept {
  return splitmix64(seed * 0xD1B54A32D192ED03ULL ^ block);
}

std::string to_hex(const unsigned char *data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex(size * 2, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    hex[2 * i] = kDigits[data[i] >> 4];
    hex[2 * i + 1] = kDigits[data[i] & 0x0F];
  }
  return hex;
}

struct CipherCtxDeleter {
  void operator()(EVP_CIPHER_CTX *ctx) const noexcept { EVP_CIPHER_CTX_free(ctx); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

std::string aes128_cbc(bool encrypt, std::string_view key, std::string_view iv,
                       std::string_view input) {
  if (key.size() != 16 || iv.size() != 16) {
    throw std::invalid_argument("AES-128 key and IV must be 16 bytes");
  }
  CipherCtx ctx(EVP_CIPHER_CTX_new());
  if (!ctx ||
      EVP_CipherInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr,
                        reinterpret_cast<const unsigned char *>(key.data()),
                        reinterpret_cast<const unsigned char *>(iv.data()), encrypt ? 1 : 0) != 1) {
    throw std::runtime_error("AES init failed");
  }
  std::string out(input.size() + 16, '\0');
  int written = 0;
  int final_written = 0;
  auto *out_ptr = reinterpret_cast<unsigned char *>(out.data());
  if (EVP_CipherUpdate(ctx.get(), out_ptr, &written,
                       reinterpret_cast<const unsigned char *>(input.data()),
                       static_cast<int>(input.size())) != 1 ||
      EVP_CipherFinal_ex(ctx.get(), out_ptr + written, &final_written) != 1) {
    throw std::runtime_error("AES operation failed");
  }
  out.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(final_written));
  return out;
}

} // namespace

void fill_content(std::uint64_t seed, std::uint64_t offset, std::span<char> out) {
  std::size_t pos = 0;
  std::uint64_t block = offset / 8;
  // Leading partial word.
  if (const auto skip = static_cast<unsigned>(offset % 8); skip != 0 && !out.empty()) {
    const std::uint64_t word = content_word(seed, block++);
    for (unsigned b = skip; b < 8 && pos < out.size(); ++b) {
      out[pos++] = static_cast<char>((word >> (8 * b)) & 0xFF);
    }
  }
  // Whole words.
  for (; pos + 8 <= out.size(); pos += 8) {
    const std::uint64_t word = content_word(seed, block++);
    for (unsigned b = 0; b < 8; ++b) {
      out[pos + b] = static_cast<char>((word >> (8 * b)) & 0xFF);
    }
  }
  // Trailing partial word.
  if (pos < out.size()) {
    const std::uint64_t word = content_word(seed, block);
    for (unsigned b = 0; pos < out.size(); ++b) {
      out[pos++] = static_cast<char>((word >> (8 * b)) & 0xFF);
    }
  }
}

std::string make_content(std::uint64_t seed, std::uint64_t offset, std::size_t size) {
  std::string data(size, '\0');
  fill_content(seed, offset, data);
  return data;
}

std::string content_sha256(std::uint64_t seed, std::uint64_t size) {
  Sha256 hash;
  std::string buffer(std::size_t{1} << 20, '\0');
  for (std::uint64_t offset = 0; offset < size;) {
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
    fill_content(seed, offset, std::span<char>(buffer.data(), n));
    hash.update(std::string_view(buffer.data(), n));
    offset += n;
  }
  return hash.final_hex();
}

std::string sha256_hex(std::string_view data) {
  Sha256 hash;
  hash.update(data);
  return hash.final_hex();
}

void Sha256::CtxDeleter::operator()(evp_md_ctx_st *ctx) const noexcept { EVP_MD_CTX_free(ctx); }

Sha256::Sha256() : ctx_(EVP_MD_CTX_new()) {
  if (!ctx_ || EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("SHA-256 init failed");
  }
}

void Sha256::update(std::string_view data) {
  if (EVP_DigestUpdate(ctx_.get(), data.data(), data.size()) != 1) {
    throw std::runtime_error("SHA-256 update failed");
  }
}

std::string Sha256::final_hex() {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(ctx_.get(), digest.data(), &size) != 1) {
    throw std::runtime_error("SHA-256 final failed");
  }
  return to_hex(digest.data(), size);
}

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
  unsigned int size = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char *>(data.data()), data.size(), mac.data(),
           &size) == nullptr) {
    throw std::runtime_error("HMAC failed");
  }
  return to_hex(mac.data(), size);
}

std::string aes128_cbc_encrypt(std::string_view key, std::string_view iv, std::string_view plain) {
  return aes128_cbc(true, key, iv, plain);
}

std::string aes128_cbc_decrypt(std::string_view key, std::string_view iv, std::string_view cipher) {
  return aes128_cbc(false, key, iv, cipher);
}

} // namespace lanes::testserver
