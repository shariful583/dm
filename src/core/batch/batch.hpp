// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

namespace lanes::core::batch {

inline constexpr std::size_t kMaxExpansion = 10'000;

// True if `url` contains at least one range group (see expand_pattern).
bool has_pattern(std::string_view url);

// Expands range groups:
//   [1-12]    1, 2, ..., 12
//   [001-120] 001, 002, ..., 120 (zero-padded to the width of the first number)
//   [a-f]     a, b, ..., f   (single letters, either case)
// Several groups combine (first group varies slowest). Brackets that are not a range — such as
// an IPv6 host "[::1]" — are kept as they are. Descending ranges count down.
// InvalidArgument if the result would exceed kMaxExpansion URLs. Thread-safe.
Result<std::vector<std::string>> expand_pattern(std::string_view url);

// URLs from a text list: one per line; blank lines and lines starting with '#' are skipped;
// surrounding whitespace is trimmed. Thread-safe.
std::vector<std::string> parse_url_list(std::string_view text);

} // namespace lanes::core::batch
