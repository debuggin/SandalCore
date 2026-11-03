// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "content/content_db.h"
#include "content/glaze_schemas.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace aoc::worldd::content {

/// A prepared statement, finalized on every path out.
class Stmt {
public:
  Stmt(sqlite3 *db, const char *sql)
      : rc_(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr)) {}
  Stmt(sqlite3 *db, const char *sql, int64_t b1) : Stmt(db, sql) {
    if (ok())
      sqlite3_bind_int64(stmt_, 1, b1);
  }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt &) = delete;
  Stmt &operator=(const Stmt &) = delete;

  bool ok() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }
  int step() const { return ok() ? sqlite3_step(stmt_) : SQLITE_ERROR; }
  sqlite3_stmt *get() const { return stmt_; }

private:
  sqlite3_stmt *stmt_ = nullptr;
  int rc_ = SQLITE_OK;
};

std::string column_text(sqlite3_stmt *s, int col);
std::string db_message(sqlite3 *db);

inline bool column_is_int(sqlite3_stmt *s, int col) { return sqlite3_column_type(s, col) == SQLITE_INTEGER; }
inline bool column_is_real(sqlite3_stmt *s, int col) {
  const int t = sqlite3_column_type(s, col);
  return t == SQLITE_FLOAT || t == SQLITE_INTEGER;
}
inline bool column_is_text(sqlite3_stmt *s, int col) { return sqlite3_column_type(s, col) == SQLITE_TEXT; }
inline int col_int(sqlite3_stmt *s, int c, int d = 0) {
  return column_is_int(s, c) ? static_cast<int>(sqlite3_column_int64(s, c)) : d;
}
inline int64_t col_i64(sqlite3_stmt *s, int c, int64_t d = 0) {
  return column_is_int(s, c) ? sqlite3_column_int64(s, c) : d;
}
inline double col_real(sqlite3_stmt *s, int c, double d = 0.0) {
  return column_is_real(s, c) ? sqlite3_column_double(s, c) : d;
}
inline std::string col_str(sqlite3_stmt *s, int c) { return column_is_text(s, c) ? column_text(s, c) : std::string{}; }

struct GuidQuery {
  const char *sql;
  const char *what;
};

bool fail_msg(const std::string &msg, std::string &error);
bool fail_db(sqlite3 *db, const std::string &what, std::string &error);
bool check_done(sqlite3 *db, int step, const std::string &what, std::string &error);
bool check_cols(sqlite3_stmt *s, const std::string &table, const char *kinds, std::string &error);
void read_rarity_bounds(sqlite3 *db, int64_t guid, items::ItemRarityFacts &out);
void read_quality_curve(sqlite3 *db, int64_t curve, items::ItemRarityFacts &out);
void read_item_rarity(sqlite3 *db, int64_t guid, ItemRecord &rec);
bool guid_list(sqlite3 *db, GuidQuery query, int64_t parent, std::vector<int64_t> &out, std::string &error);

int64_t json_int(const nlohmann::json &o, std::string_view key);
double json_num(const nlohmann::json &o, std::string_view key);
bool json_flag(const nlohmann::json &o, std::string_view key);
std::string json_str(const nlohmann::json &o, std::string_view key);

} // namespace aoc::worldd::content
