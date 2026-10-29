// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/store/sqlite.hpp"

#include <sqlite3.h>

#include "core/platform/path.hpp"

namespace lanes::core::store {

namespace {

Error db_error(sqlite3 *db, std::string_view what) {
  return Error{ErrorCode::Io,
               std::string(what) + ": " + (db != nullptr ? sqlite3_errmsg(db) : "out of memory"),
               0};
}

} // namespace

void Statement::Deleter::operator()(sqlite3_stmt *s) const noexcept { sqlite3_finalize(s); }
void Database::Deleter::operator()(sqlite3 *db) const noexcept { sqlite3_close_v2(db); }

Statement &Statement::bind(int index, std::int64_t value) {
  sqlite3_bind_int64(stmt_.get(), index, value);
  return *this;
}

Statement &Statement::bind(int index, std::string_view value) {
  sqlite3_bind_text(stmt_.get(), index, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
  return *this;
}

Statement &Statement::bind(int index, std::nullopt_t) {
  sqlite3_bind_null(stmt_.get(), index);
  return *this;
}

Statement &Statement::bind(int index, const std::optional<std::int64_t> &value) {
  return value ? bind(index, *value) : bind(index, std::nullopt);
}

Result<bool> Statement::step() {
  const int rc = sqlite3_step(stmt_.get());
  if (rc == SQLITE_ROW) {
    return true;
  }
  if (rc == SQLITE_DONE) {
    return false;
  }
  return make_error(db_error(db_, "database query failed"));
}

Result<void> Statement::run() {
  for (;;) {
    auto row = step();
    if (!row) {
      return make_error(row.error());
    }
    if (!*row) {
      return {};
    }
  }
}

std::int64_t Statement::int64(int column) const {
  return sqlite3_column_int64(stmt_.get(), column);
}

std::optional<std::int64_t> Statement::optional_int64(int column) const {
  if (sqlite3_column_type(stmt_.get(), column) == SQLITE_NULL) {
    return std::nullopt;
  }
  return int64(column);
}

std::string Statement::text(int column) const {
  const auto *p = sqlite3_column_text(stmt_.get(), column);
  const int n = sqlite3_column_bytes(stmt_.get(), column);
  return p != nullptr ? std::string(reinterpret_cast<const char *>(p), static_cast<std::size_t>(n))
                      : std::string();
}

Result<Database> Database::open(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  const std::string utf8 = platform::path_to_utf8(path);
  const int rc =
      sqlite3_open_v2(utf8.c_str(), &raw,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  Database db;
  db.db_.reset(raw);
  if (rc != SQLITE_OK) {
    return make_error(db_error(raw, "cannot open database '" + utf8 + "'"));
  }
  sqlite3_busy_timeout(raw, 5000);
  return db;
}

Result<void> Database::exec(const std::string &sql) {
  char *message = nullptr;
  if (sqlite3_exec(db_.get(), sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
    std::string text = message != nullptr ? message : "unknown error";
    sqlite3_free(message);
    return make_error(ErrorCode::Io, "database error: " + text);
  }
  return {};
}

Result<Statement> Database::prepare(std::string_view sql) {
  sqlite3_stmt *raw = nullptr;
  if (sqlite3_prepare_v2(db_.get(), sql.data(), static_cast<int>(sql.size()), &raw, nullptr) !=
      SQLITE_OK) {
    return make_error(db_error(db_.get(), "cannot prepare query"));
  }
  Statement s;
  s.stmt_.reset(raw);
  s.db_ = db_.get();
  return s;
}

std::int64_t Database::last_insert_id() const { return sqlite3_last_insert_rowid(db_.get()); }

} // namespace lanes::core::store
