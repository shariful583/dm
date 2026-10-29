// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/format.hpp"

#include <array>
#include <cmath>

namespace lanes::gui {

namespace {

QString scaled(double value, const char *suffix) {
  static constexpr std::array<const char *, 5> kUnits{"B", "KiB", "MiB", "GiB", "TiB"};
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < kUnits.size()) {
    value /= 1024.0;
    ++unit;
  }
  return QString::number(value, 'f', unit == 0 ? 0 : 1) + ' ' + kUnits[unit] + suffix;
}

} // namespace

QString format_bytes(std::uint64_t bytes) { return scaled(static_cast<double>(bytes), ""); }

QString format_speed(double bytes_per_second) {
  return scaled(std::max(bytes_per_second, 0.0), "/s");
}

QString format_duration(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0 || seconds > 99.0 * 3600) {
    return {};
  }
  if (seconds < 9.95) {
    return QString::number(seconds, 'f', 1) + 's';
  }
  const auto total = static_cast<long long>(std::llround(seconds));
  const long long h = total / 3600;
  const long long m = (total / 60) % 60;
  const long long s = total % 60;
  if (h > 0) {
    return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
  }
  if (m > 0) {
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
  }
  return QString::number(s) + 's';
}

std::optional<std::uint64_t> parse_size(const QString &text) {
  QString t = text.trimmed().toLower();
  if (t.endsWith("/s")) {
    t.chop(2);
  }
  double factor = 1;
  for (const auto &[suffix, f] : std::array<std::pair<const char *, double>, 9>{{
           {"kib", 1024.0},
           {"mib", 1024.0 * 1024},
           {"gib", 1024.0 * 1024 * 1024},
           {"kb", 1024.0},
           {"mb", 1024.0 * 1024},
           {"gb", 1024.0 * 1024 * 1024},
           {"k", 1024.0},
           {"m", 1024.0 * 1024},
           {"g", 1024.0 * 1024 * 1024},
       }}) {
    if (t.endsWith(QLatin1StringView(suffix))) {
      t.chop(static_cast<qsizetype>(std::char_traits<char>::length(suffix)));
      factor = f;
      break;
    }
  }
  if (t.endsWith('b')) {
    t.chop(1);
  }
  bool ok = false;
  const double number = t.trimmed().toDouble(&ok);
  if (!ok || number < 0 || !std::isfinite(number)) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(std::llround(number * factor));
}

} // namespace lanes::gui
