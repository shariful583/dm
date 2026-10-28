// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

struct evp_md_ctx_st; // OpenSSL EVP_MD_CTX

namespace lanes::testserver {

// Deterministic pseudo-random file content: byte `i` of a file is a pure function of (seed, i), so
// any range can be generated independently and files of any size need no memory or disk.
// Thread-safe (pure function).
void fill_content(std::uint64_t seed, std::uint64_t offset, std::span<char> out);

// Generated content as a string. Intended for small sizes in tests. Thread-safe.
std::string make_content(std::uint64_t seed, std::uint64_t offset, std::size_t size);

// Lowercase hex SHA-256 of `size` bytes of generated content starting at offset 0. Thread-safe.
std::string content_sha256(std::uint64_t seed, std::uint64_t size);

// Lowercase hex SHA-256 of `data`. Thread-safe.
std::string sha256_hex(std::string_view data);

// Incremental SHA-256. Not thread-safe; use one instance per thread.
class Sha256 {
public:
  Sha256();

  void update(std::string_view data);
  // Returns lowercase hex digest. The object must not be updated afterwards.
  std::string final_hex();

private:
  struct CtxDeleter {
    void operator()(evp_md_ctx_st *ctx) const noexcept;
  };
  std::unique_ptr<evp_md_ctx_st, CtxDeleter> ctx_;
};

// Lowercase hex HMAC-SHA256 of `data` under `key`. Thread-safe.
std::string hmac_sha256_hex(std::string_view key, std::string_view data);

// AES-128-CBC with PKCS#7 padding (the HLS `METHOD=AES-128` scheme). `key` and `iv` must be 16
// bytes. Thread-safe. Throw std::runtime_error on failure (test tooling only).
std::string aes128_cbc_encrypt(std::string_view key, std::string_view iv, std::string_view plain);
std::string aes128_cbc_decrypt(std::string_view key, std::string_view iv, std::string_view cipher);

} // namespace lanes::testserver
