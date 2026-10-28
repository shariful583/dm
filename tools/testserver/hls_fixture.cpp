// SPDX-License-Identifier: GPL-3.0-or-later
#include "hls_fixture.hpp"

#include <string_view>

#include "content.hpp"

namespace lanes::testserver {

namespace {

constexpr std::string_view kPlaylistType = "application/vnd.apple.mpegurl";

// IV for a segment without an explicit IV attribute: the media sequence number as a 128-bit
// big-endian integer (RFC 8216 §5.2).
std::string sequence_iv(std::uint64_t sequence) {
  std::string iv(16, '\0');
  for (int i = 0; i < 8; ++i) {
    iv[15 - i] = static_cast<char>((sequence >> (8 * i)) & 0xFF);
  }
  return iv;
}

std::string hex_iv(std::string_view iv) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out = "0x";
  for (const char c : iv) {
    const auto b = static_cast<unsigned char>(c);
    out += kDigits[b >> 4];
    out += kDigits[b & 0x0F];
  }
  return out;
}

std::string playlist_header(int version, std::uint64_t media_sequence) {
  return "#EXTM3U\n#EXT-X-VERSION:" + std::to_string(version) +
         "\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:" + std::to_string(media_sequence) +
         "\n";
}

} // namespace

HlsFixture make_hls_fixture(const std::string &prefix, const HlsFixtureOptions &options) {
  HlsFixture fx;
  fx.key = make_content(options.seed ^ 0x6B6579, 0, 16);
  fx.files["key.bin"] = {fx.key, "application/octet-stream"};

  fx.files["master.m3u8"] = {
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\"\n"
      "low/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=2500000,RESOLUTION=1280x720,CODECS=\"avc1.4d401f,mp4a.40.2\"\n"
      "high/index.m3u8\n",
      std::string(kPlaylistType)};

  std::uint64_t variant_seed = options.seed;
  const auto segment_payload = [&](int index) {
    return make_content(variant_seed * 1000 + static_cast<std::uint64_t>(index), 0,
                        options.segment_size);
  };

  // low/: explicit IV, relative key URI.
  {
    variant_seed = options.seed + 1;
    const std::string iv = make_content(options.seed ^ 0x6976, 0, 16);
    std::string m3u8 = playlist_header(3, 0);
    m3u8 += "#EXT-X-KEY:METHOD=AES-128,URI=\"../key.bin\",IV=" + hex_iv(iv) + "\n";
    for (int i = 0; i < options.segments; ++i) {
      const std::string name = "seg" + std::to_string(i) + ".ts";
      const std::string plain = segment_payload(i);
      fx.files["low/" + name] = {aes128_cbc_encrypt(fx.key, iv, plain), "video/mp2t"};
      fx.plaintext["low/" + name] = plain;
      m3u8 += "#EXTINF:4.000,\n" + name + "\n";
    }
    fx.files["low/index.m3u8"] = {m3u8 + "#EXT-X-ENDLIST\n", std::string(kPlaylistType)};
  }

  // high/: IV derived from media sequence number, absolute-path key URI.
  {
    variant_seed = options.seed + 2;
    constexpr std::uint64_t kFirstSequence = 10;
    std::string m3u8 = playlist_header(3, kFirstSequence);
    m3u8 += "#EXT-X-KEY:METHOD=AES-128,URI=\"" + prefix + "/key.bin\"\n";
    for (int i = 0; i < options.segments; ++i) {
      const std::string name = "seg" + std::to_string(i) + ".ts";
      const std::string plain = segment_payload(i);
      const std::string iv = sequence_iv(kFirstSequence + static_cast<std::uint64_t>(i));
      fx.files["high/" + name] = {aes128_cbc_encrypt(fx.key, iv, plain), "video/mp2t"};
      fx.plaintext["high/" + name] = plain;
      m3u8 += "#EXTINF:4.000,\n" + name + "\n";
    }
    fx.files["high/index.m3u8"] = {m3u8 + "#EXT-X-ENDLIST\n", std::string(kPlaylistType)};
  }

  // plain/: no encryption.
  {
    variant_seed = options.seed + 3;
    std::string m3u8 = playlist_header(3, 0);
    for (int i = 0; i < options.segments; ++i) {
      const std::string name = "seg" + std::to_string(i) + ".ts";
      const std::string plain = segment_payload(i);
      fx.files["plain/" + name] = {plain, "video/mp2t"};
      fx.plaintext["plain/" + name] = plain;
      m3u8 += "#EXTINF:4.000,\n" + name + "\n";
    }
    fx.files["plain/index.m3u8"] = {m3u8 + "#EXT-X-ENDLIST\n", std::string(kPlaylistType)};
  }

  // fmp4/: init segment via EXT-X-MAP.
  {
    variant_seed = options.seed + 4;
    const std::string init = make_content(variant_seed ^ 0x696E6974, 0, 1024);
    fx.files["fmp4/init.mp4"] = {init, "video/mp4"};
    fx.plaintext["fmp4/init.mp4"] = init;
    std::string m3u8 = playlist_header(7, 0);
    m3u8 += "#EXT-X-MAP:URI=\"init.mp4\"\n";
    for (int i = 0; i < options.segments; ++i) {
      const std::string name = "seg" + std::to_string(i) + ".m4s";
      const std::string plain = segment_payload(i);
      fx.files["fmp4/" + name] = {plain, "video/iso.segment"};
      fx.plaintext["fmp4/" + name] = plain;
      m3u8 += "#EXTINF:4.000,\n" + name + "\n";
    }
    fx.files["fmp4/index.m3u8"] = {m3u8 + "#EXT-X-ENDLIST\n", std::string(kPlaylistType)};
  }

  return fx;
}

} // namespace lanes::testserver
