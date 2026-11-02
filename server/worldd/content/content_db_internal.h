// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "content/content_db.h"

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

inline std::string column_text(sqlite3_stmt *s, int col) {
  const unsigned char *t = sqlite3_column_text(s, col);
  return t ? std::string(reinterpret_cast<const char *>(t),
                         static_cast<size_t>(sqlite3_column_bytes(s, col)))
           : std::string();
}
inline std::string db_message(sqlite3 *db) {
  const char *m = sqlite3_errmsg(db);
  return m ? m : "unknown SQLite error";
}
inline bool column_is_int(sqlite3_stmt *s, int col) {
  return sqlite3_column_type(s, col) == SQLITE_INTEGER;
}
inline bool column_is_real(sqlite3_stmt *s, int col) {
  const int t = sqlite3_column_type(s, col);
  return t == SQLITE_FLOAT || t == SQLITE_INTEGER;
}
inline bool column_is_text(sqlite3_stmt *s, int col) {
  return sqlite3_column_type(s, col) == SQLITE_TEXT;
}
inline int col_int(sqlite3_stmt *s, int c, int d = 0) {
  return column_is_int(s, c) ? static_cast<int>(sqlite3_column_int64(s, c)) : d;
}
inline int64_t col_i64(sqlite3_stmt *s, int c, int64_t d = 0) {
  return column_is_int(s, c) ? sqlite3_column_int64(s, c) : d;
}
inline double col_real(sqlite3_stmt *s, int c, double d = 0.0) {
  return column_is_real(s, c) ? sqlite3_column_double(s, c) : d;
}
inline std::string col_str(sqlite3_stmt *s, int c) {
  return column_is_text(s, c) ? column_text(s, c) : std::string{};
}
inline bool fail_msg(const std::string &msg, std::string &error) {
  error = "content.db " + msg;
  return false;
}
inline bool fail_db(sqlite3 *db, const std::string &what, std::string &error) {
  return fail_msg(what + ": " + db_message(db), error);
}
inline bool check_done(sqlite3 *db, int step, const std::string &what,
                       std::string &error) {
  return step == SQLITE_DONE ? true : fail_db(db, what, error);
}
inline bool check_cols(sqlite3_stmt *s, const std::string &table,
                       const char *kinds, std::string &error) {
  for (int c = 0; kinds[c] != '\0'; ++c) {
    const char k = kinds[c];
    if ((k == 'i' && !column_is_int(s, c)) ||
        (k == 'r' && !column_is_real(s, c)) ||
        (k == 't' && !column_is_text(s, c)))
      return fail_msg(table + " has a column of the wrong storage class",
                      error);
  }
  return true;
}

inline constexpr const char *kRarityBoundsSql =
    "SELECT rarity_min, rarity_max FROM item_vendor WHERE item_guid = ?1";
inline constexpr const char *kStatRowSql =
    "SELECT common_min, common_max, uncommon_min, uncommon_max, rare_min, "
    "rare_max, heroic_min, heroic_max, epic_min, epic_max, "
    "legendary_min, legendary_max, artifact_min, artifact_max FROM "
    "stat_block_stat WHERE block_guid = ?1 AND stat_guid = ?2";
inline constexpr const char *kItemGearSql =
    "SELECT stat_block_guid, quality_curve_guid FROM item WHERE guid = ?1";
inline constexpr const char *kQualityCurveSql =
    "SELECT ord, x, y FROM quality_curve WHERE guid = ?1 ORDER BY ord";

inline void read_rarity_bounds(sqlite3 *db, int64_t guid,
                               items::ItemRarityFacts &out) {
  Stmt q(db, kRarityBoundsSql, guid);
  if (q.step() == SQLITE_ROW && column_is_int(q.get(), 0) &&
      column_is_int(q.get(), 1)) {
    const int hi = items::kRarityCount - 1;
    out.rarity_min = std::clamp(col_int(q.get(), 0), 0, hi);
    out.rarity_max = std::clamp(col_int(q.get(), 1), 0, hi);
  }
}
inline void read_quality_curve(sqlite3 *db, int64_t curve,
                               items::ItemRarityFacts &out) {
  out.has_curve = false;
  Stmt q(db, kQualityCurveSql, curve);
  std::array<items::QualityBand, items::kRarityCount> bands{};
  int n = 0, step = SQLITE_ERROR;
  while (curve != 0 && (step = q.step()) == SQLITE_ROW) {
    if (n >= items::kRarityCount || col_i64(q.get(), 0, -1) != n ||
        !column_is_real(q.get(), 1) || !column_is_real(q.get(), 2))
      return;
    bands[static_cast<size_t>(n++)] = {col_real(q.get(), 1),
                                       col_real(q.get(), 2)};
  }
  if (step == SQLITE_DONE && n == items::kRarityCount) {
    out.bands = bands;
    out.has_curve = true;
  }
}
inline void read_item_rarity(sqlite3 *db, int64_t guid, ItemRecord &rec) {
  Stmt g(db, kItemGearSql, guid);
  if (g.step() != SQLITE_ROW)
    return;
  const int64_t block = col_i64(g.get(), 0), curve = col_i64(g.get(), 1);
  read_rarity_bounds(db, guid, rec);
  read_quality_curve(db, curve, rec);
  Stmt q(db, kStatRowSql, block);
  if (block == 0 || !q.ok())
    return;
  sqlite3_bind_int64(q.get(), 2, items::kMaxDurabilityStat);
  if (q.step() != SQLITE_ROW)
    return;
  items::RarityRange row;
  for (int r = 0; r < items::kRarityCount; ++r) {
    if (!column_is_real(q.get(), 2 * r) || !column_is_real(q.get(), 2 * r + 1))
      return;
    row.min[static_cast<size_t>(r)] = col_real(q.get(), 2 * r);
    row.max[static_cast<size_t>(r)] = col_real(q.get(), 2 * r + 1);
  }
  rec.durability_stat = row;
}
struct GuidQuery {
  const char *sql;
  const char *what;
};
inline bool guid_list(sqlite3 *db, GuidQuery query, int64_t parent,
                      std::vector<int64_t> &out, std::string &error) {
  Stmt q(db, query.sql, parent);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW)
    out.push_back(sqlite3_column_int64(q.get(), 0));
  return check_done(db, step, std::string(query.what) + " query failed", error);
}

} // namespace aoc::worldd::content
