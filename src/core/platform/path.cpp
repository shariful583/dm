// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/platform/path.hpp"

#include <array>
#include <cstdint>
#include <system_error>
#include <vector>

namespace lanes::core::platform {

namespace {

struct CodePoint {
  char32_t value;
  std::string_view bytes; // empty if the sequence was invalid
};

// Decodes the UTF-8 sequence at `s[pos]`, advancing `pos`. Invalid or overlong sequences and
// surrogates yield one byte with empty `bytes`.
CodePoint next_code_point(std::string_view s, std::size_t &pos) {
  const auto lead = static_cast<unsigned char>(s[pos]);
  std::size_t len = 0;
  char32_t cp = 0;
  char32_t min = 0;
  if (lead < 0x80) {
    len = 1;
    cp = lead;
  } else if ((lead & 0xE0) == 0xC0) {
    len = 2;
    cp = lead & 0x1Fu;
    min = 0x80;
  } else if ((lead & 0xF0) == 0xE0) {
    len = 3;
    cp = lead & 0x0Fu;
    min = 0x800;
  } else if ((lead & 0xF8) == 0xF0) {
    len = 4;
    cp = lead & 0x07u;
    min = 0x10000;
  }
  const auto invalid = [&] {
    ++pos;
    return CodePoint{0xFFFD, {}};
  };
  if (len == 0 || pos + len > s.size()) {
    return invalid();
  }
  for (std::size_t i = 1; i < len; ++i) {
    const auto c = static_cast<unsigned char>(s[pos + i]);
    if ((c & 0xC0) != 0x80) {
      return invalid();
    }
    cp = (cp << 6) | (c & 0x3Fu);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return invalid();
  }
  const CodePoint result{cp, s.substr(pos, len)};
  pos += len;
  return result;
}

bool is_forbidden(char32_t cp) {
  if (cp < 0x20 || cp == 0x7F) {
    return true;
  }
  switch (cp) {
  case '<':
  case '>':
  case ':':
  case '"':
  case '/':
  case '\\':
  case '|':
  case '?':
  case '*':
    return true;
  default:
    return false;
  }
}

std::size_t utf16_units(char32_t cp) { return cp >= 0x10000 ? 2 : 1; }

char ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool is_reserved_device_name(std::string_view name) {
  // The device-name check uses the part before the first dot, ignoring trailing spaces.
  std::string_view base = name.substr(0, name.find('.'));
  while (!base.empty() && base.back() == ' ') {
    base.remove_suffix(1);
  }
  std::string upper;
  for (const char c : base) {
    upper += ascii_upper(c);
  }
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
    return true;
  }
  if (upper.size() >= 4 && (upper.starts_with("COM") || upper.starts_with("LPT"))) {
    const std::string_view suffix = std::string_view(upper).substr(3);
    // COM0-9 / LPT0-9, and the superscript digits ¹ ² ³ which Windows also treats as ports.
    if (suffix.size() == 1 && suffix[0] >= '0' && suffix[0] <= '9') {
      return true;
    }
    if (suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3") {
      return true;
    }
  }
  return false;
}

void trim_trailing_dots_and_spaces(std::vector<CodePoint> &cps) {
  while (!cps.empty() && (cps.back().value == '.' || cps.back().value == ' ')) {
    cps.pop_back();
  }
}

std::string join(const std::vector<CodePoint> &cps) {
  std::string out;
  for (const auto &cp : cps) {
    out += cp.bytes;
  }
  return out;
}

} // namespace

std::filesystem::path path_from_utf8(std::string_view utf8) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

std::string path_to_utf8(const std::filesystem::path &path) {
  const std::u8string u8 = path.u8string();
  return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

std::filesystem::path to_long_path(const std::filesystem::path &path) {
#ifdef _WIN32
  const std::wstring &native = path.native();
  if (native.starts_with(L"\\\\?\\") || native.starts_with(L"\\\\.\\")) {
    return path;
  }
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(path, ec);
  if (ec) {
    return path;
  }
  std::wstring normal = abs.lexically_normal().native();
  if (normal.starts_with(L"\\\\")) {
    return std::filesystem::path(L"\\\\?\\UNC\\" + normal.substr(2));
  }
  return std::filesystem::path(L"\\\\?\\" + normal);
#else
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(path, ec);
  return ec ? path : abs.lexically_normal();
#endif
}

std::string sanitize_filename(std::string_view name, std::string_view fallback,
                              std::size_t max_utf16_units) {
  static constexpr CodePoint kUnderscore{'_', "_"};

  std::vector<CodePoint> cps;
  for (std::size_t pos = 0; pos < name.size();) {
    const CodePoint cp = next_code_point(name, pos);
    cps.push_back(cp.bytes.empty() || is_forbidden(cp.value) ? kUnderscore : cp);
  }

  // Leading spaces, trailing dots and spaces.
  std::size_t first = 0;
  while (first < cps.size() && cps[first].value == ' ') {
    ++first;
  }
  cps.erase(cps.begin(), cps.begin() + static_cast<std::ptrdiff_t>(first));
  trim_trailing_dots_and_spaces(cps);

  if (!cps.empty() && is_reserved_device_name(join(cps))) {
    cps.insert(cps.begin(), kUnderscore);
  }

  // Length limit in UTF-16 units, keeping a short extension.
  std::size_t units = 0;
  for (const auto &cp : cps) {
    units += utf16_units(cp.value);
  }
  if (units > max_utf16_units) {
    std::vector<CodePoint> ext;
    for (std::size_t i = cps.size(); i-- > 1;) {
      if (cps[i].value == '.') {
        if (cps.size() - i <= 16) { // ".tar.gz"-style short extensions only
          ext.assign(cps.begin() + static_cast<std::ptrdiff_t>(i), cps.end());
          cps.resize(i);
        }
        break;
      }
    }
    std::size_t ext_units = 0;
    for (const auto &cp : ext) {
      ext_units += utf16_units(cp.value);
    }
    if (ext_units >= max_utf16_units) {
      ext.clear();
      ext_units = 0;
    }
    std::size_t kept = 0;
    std::size_t count = 0;
    for (; count < cps.size(); ++count) {
      if (kept + utf16_units(cps[count].value) + ext_units > max_utf16_units) {
        break;
      }
      kept += utf16_units(cps[count].value);
    }
    cps.resize(count);
    trim_trailing_dots_and_spaces(cps);
    cps.insert(cps.end(), ext.begin(), ext.end());
  }

  std::string out = join(cps);
  return out.empty() ? std::string(fallback) : out;
}

} // namespace lanes::core::platform
