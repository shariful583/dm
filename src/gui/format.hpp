// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

#include <QString>

namespace lanes::gui {

// "512 B", "1.5 MiB", "2.0 GiB". Thread-safe.
QString format_bytes(std::uint64_t bytes);
// "1.2 MiB/s".
QString format_speed(double bytes_per_second);
// "4.2s", "35s", "2:05", "1:02:03"; "" when unknown (negative / not finite / > 99 h).
QString format_duration(double seconds);
// "500K", "2M", "1.5G", "1048576" → bytes; nullopt when invalid. Units are binary (K = 1024).
std::optional<std::uint64_t> parse_size(const QString &text);

} // namespace lanes::gui
