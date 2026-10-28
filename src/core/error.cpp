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
  case ErrorCode::NotFound:
    return "not_found";
  case ErrorCode::AccessDenied:
    return "access_denied";
  case ErrorCode::AlreadyExists:
    return "already_exists";
  case ErrorCode::NoSpace:
    return "no_space";
  case ErrorCode::FileTooLarge:
    return "file_too_large";
  case ErrorCode::Unsupported:
    return "unsupported";
  case ErrorCode::Timeout:
    return "timeout";
  case ErrorCode::Network:
    return "network";
  case ErrorCode::Http:
    return "http";
  case ErrorCode::Cancelled:
    return "cancelled";
  case ErrorCode::RemoteChanged:
    return "remote_changed";
  case ErrorCode::LinkExpired:
    return "link_expired";
  case ErrorCode::ChecksumMismatch:
    return "checksum_mismatch";
  }
  return "unknown";
}

} // namespace lanes::core
