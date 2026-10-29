// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/batch/batch.hpp"

#include <charconv>

namespace lanes::core::batch {

namespace {

// One "[from-to]" group of a pattern.
struct Group {
  std::size_t begin = 0; // position of '['
  std::size_t end = 0;   // position after ']'
  std::vector<std::string> values;
  bool too_many = false;
};

bool all_digits(std::string_view s) {
  return !s.empty() && s.find_first_not_of("0123456789") == std::string_view::npos;
}

bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Fills g.values from the text inside the brackets; false if it is not a range.
bool parse_range(std::string_view inside, Group &g) {
  const auto dash = inside.find('-');
  if (dash == std::string_view::npos || dash == 0 || dash + 1 == inside.size()) {
    return false;
  }
  const std::string_view a = inside.substr(0, dash);
  const std::string_view b = inside.substr(dash + 1);
  if (all_digits(a) && all_digits(b) && a.size() <= 9 && b.size() <= 9) {
    long long from = 0;
    long long to = 0;
    std::from_chars(a.data(), a.data() + a.size(), from);
    std::from_chars(b.data(), b.data() + b.size(), to);
    const std::size_t width = a.size() > 1 && a.front() == '0' ? a.size() : 0;
    const long long step = from <= to ? 1 : -1;
    if ((to - from) * step + 1 > static_cast<long long>(kMaxExpansion)) {
      g.too_many = true;
      return true;
    }
    for (long long v = from;; v += step) {
      std::string text = std::to_string(v);
      if (text.size() < width) {
        text.insert(0, width - text.size(), '0');
      }
      g.values.push_back(std::move(text));
      if (v == to) {
        return true;
      }
    }
  }
  if (a.size() == 1 && b.size() == 1 && is_letter(a[0]) && is_letter(b[0]) &&
      ((a[0] >= 'a') == (b[0] >= 'a'))) { // same case
    const int step = a[0] <= b[0] ? 1 : -1;
    for (char c = a[0];; c = static_cast<char>(c + step)) {
      g.values.emplace_back(1, c);
      if (c == b[0]) {
        return true;
      }
    }
  }
  return false;
}

std::vector<Group> find_groups(std::string_view url) {
  std::vector<Group> groups;
  for (std::size_t pos = 0; (pos = url.find('[', pos)) != std::string_view::npos;) {
    const auto close = url.find(']', pos + 1);
    if (close == std::string_view::npos) {
      break;
    }
    Group g;
    g.begin = pos;
    g.end = close + 1;
    if (parse_range(url.substr(pos + 1, close - pos - 1), g)) {
      groups.push_back(std::move(g));
    }
    pos = close + 1;
  }
  return groups;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

} // namespace

bool has_pattern(std::string_view url) { return !find_groups(url).empty(); }

Result<std::vector<std::string>> expand_pattern(std::string_view url) {
  const std::vector<Group> groups = find_groups(url);
  std::size_t total = 1;
  for (const auto &g : groups) {
    total *= g.too_many ? kMaxExpansion + 1 : g.values.size();
    if (total > kMaxExpansion) {
      return make_error(ErrorCode::InvalidArgument, "the pattern expands to more than " +
                                                        std::to_string(kMaxExpansion) +
                                                        " addresses");
    }
  }
  std::vector<std::string> out;
  out.reserve(total);
  std::vector<std::size_t> index(groups.size(), 0);
  for (std::size_t n = 0; n < total; ++n) {
    std::string expanded;
    std::size_t last = 0;
    for (std::size_t g = 0; g < groups.size(); ++g) {
      expanded.append(url.substr(last, groups[g].begin - last));
      expanded += groups[g].values[index[g]];
      last = groups[g].end;
    }
    expanded.append(url.substr(last));
    out.push_back(std::move(expanded));
    // Odometer: the last group varies fastest.
    for (std::size_t g = groups.size(); g-- > 0;) {
      if (++index[g] < groups[g].values.size()) {
        break;
      }
      index[g] = 0;
    }
  }
  return out;
}

std::vector<std::string> parse_url_list(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) { // UTF-8 byte order mark (Notepad)
    text.remove_prefix(3);
  }
  std::vector<std::string> urls;
  std::size_t start = 0;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view line = trim(text.substr(start, end - start));
    if (!line.empty() && line.front() != '#') {
      urls.emplace_back(line);
    }
    start = end + 1;
  }
  return urls;
}

} // namespace lanes::core::batch
