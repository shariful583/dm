// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/error.hpp"

namespace lanes::core {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::Unknown:
    return "unknown";
  case ErrorCode::InvalidArgument:
    return "invalid_argument";
  case ErrorCode::Io:
    return "io";
  case ErrorCode::Network:
    return "network";
  case ErrorCode::Http:
    return "http";
  case ErrorCode::Cancelled:
    return "cancelled";
  }
  return "unknown";
}

} // namespace lanes::core
