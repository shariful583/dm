// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <tl/expected.hpp>

namespace lanes::core {

// Broad failure classes. Front-ends map these to messages and exit codes; details go in
// Error::message. Extend as engine steps need new classes.
enum class ErrorCode {
  Unknown,
  InvalidArgument,
  Io,
  Network,
  Http,
  Cancelled,
};

// Human-readable, stable name of an ErrorCode (e.g. "invalid_argument"). Thread-safe.
std::string_view to_string(ErrorCode code) noexcept;

// Error value carried by Result. Plain value type; not synchronized.
struct Error {
  ErrorCode code = ErrorCode::Unknown;
  std::string message;
};

// Return type for fallible core operations (tl::expected in core).
template <typename T> using Result = tl::expected<T, Error>;

inline tl::unexpected<Error> make_error(ErrorCode code, std::string message) {
  return tl::unexpected<Error>(Error{code, std::move(message)});
}

} // namespace lanes::core
