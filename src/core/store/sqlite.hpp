// SPDX-License-Identifier: GPL-3.0-or-later
// Internal RAII wrappers over the SQLite C API (RAII for every statement).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace lanes::core::store {

class Statement {
public:
  Statement() = default;

  Statement &bind(int index, std::int64_t value);
  Statement &bind(int index, std::string_view value);
  Statement &bind(int index, std::nullopt_t);
  Statement &bind(int index, const std::optional<std::int64_t> &value);

  // Steps once: true if a row is available, false when done.
  Result<bool> step();
  // Steps until done (for statements without results).
  Result<void> run();

  std::int64_t int64(int column) const;
  std::optional<std::int64_t> optional_int64(int column) const;
  std::string text(int column) const;

private:
  friend class Database;
  struct Deleter {
    void operator()(sqlite3_stmt *s) const noexcept;
  };
  std::unique_ptr<sqlite3_stmt, Deleter> stmt_;
  sqlite3 *db_ = nullptr;
};

// One SQLite connection. Not thread-safe: the owner serializes access.
class Database {
public:
  static Result<Database> open(const std::filesystem::path &path);

  Result<void> exec(const std::string &sql);
  Result<Statement> prepare(std::string_view sql);
  std::int64_t last_insert_id() const;

private:
  struct Deleter {
    void operator()(sqlite3 *db) const noexcept;
  };
  std::unique_ptr<sqlite3, Deleter> db_;
};

} // namespace lanes::core::store
