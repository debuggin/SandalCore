// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "content/content_db.h"
#include "content/glaze_schemas.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sqlite3.h>

namespace aoc::worldd::content {

inline int parse_env_bool_flag(const char *val) {
  if (!val || !*val)
    return -1;
  std::string s;
  for (const unsigned char c : std::string_view(val)) {
    if (!std::isspace(c))
      s.push_back(static_cast<char>(std::tolower(c)));
  }
  if (s == "1" || s == "true" || s == "yes" || s == "on")
    return 1;
  if (s == "0" || s == "false" || s == "no" || s == "off")
    return 0;
  return -1;
}

inline bool should_cache_file_in_memory(const char *specific_env,
                                        const char *filename_token) {
  if (specific_env && *specific_env) {
    const int flag = parse_env_bool_flag(std::getenv(specific_env));
    if (flag >= 0)
      return flag == 1;
  }
  if (parse_env_bool_flag(
          std::getenv("AOC_WORLD_CACHE_ALL_CONTENT_IN_MEMORY")) == 1)
    return true;
  if (const char *files = std::getenv("AOC_WORLD_CACHE_FILES");
      files && *files) {
    std::string haystack, needle;
    for (const unsigned char c : std::string_view(files))
      haystack.push_back(static_cast<char>(std::tolower(c)));
    if (haystack.find("all") != std::string::npos)
      return true;
    if (filename_token && *filename_token) {
      for (const unsigned char c : std::string_view(filename_token))
        needle.push_back(static_cast<char>(std::tolower(c)));
      if (haystack.find(needle) != std::string::npos)
        return true;
    }
  }
  return false;
}

inline void apply_readonly_sqlite_pragmas(sqlite3 *db) {
  if (!db)
    return;
  sqlite3_exec(db,
               "PRAGMA mmap_size = 536870912; PRAGMA cache_size = -16384; "
               "PRAGMA temp_store = MEMORY; PRAGMA query_only = ON;",
               nullptr, nullptr, nullptr);
}

inline sqlite3 *open_content_sqlite_db(const std::string &path, int flags,
                                       bool allow_in_memory_preload,
                                       bool *out_in_memory = nullptr,
                                       std::string *out_error = nullptr) {
  if (out_in_memory)
    *out_in_memory = false;
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK || !db) {
    if (out_error) {
      const char *m = db ? sqlite3_errmsg(db) : nullptr;
      *out_error = m ? m : "out of memory";
    }
    if (db)
      sqlite3_close(db);
    return nullptr;
  }
  apply_readonly_sqlite_pragmas(db);
  if (allow_in_memory_preload &&
      should_cache_file_in_memory("AOC_WORLD_CACHE_CONTENT_DB_IN_MEMORY",
                                  "content.db")) {
    std::error_code ec;
    const std::filesystem::path fs_path(std::u8string(
        reinterpret_cast<const char8_t *>(path.data()), path.size()));
    const auto fsize = std::filesystem::file_size(fs_path, ec);
    if (!ec && fsize > (1u << 20)) {
      sqlite3 *mem_db = nullptr;
      if (sqlite3_open_v2(":memory:", &mem_db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                              SQLITE_OPEN_FULLMUTEX,
                          nullptr) == SQLITE_OK &&
          mem_db) {
        sqlite3_backup *bk = sqlite3_backup_init(mem_db, "main", db, "main");
        if (bk) {
          sqlite3_backup_step(bk, -1);
          sqlite3_backup_finish(bk);
        }
        if (bk && sqlite3_errcode(mem_db) == SQLITE_OK) {
          sqlite3_exec(mem_db,
                       "PRAGMA cache_size = -16384; PRAGMA temp_store = "
                       "MEMORY; PRAGMA query_only = ON;",
                       nullptr, nullptr, nullptr);
          sqlite3_close(db);
          if (out_in_memory)
            *out_in_memory = true;
          return mem_db;
        }
        sqlite3_close(mem_db);
      }
    }
  }
  return db;
}

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
void read_rarity_bounds(sqlite3 *db, int64_t guid, items::ItemRarityFacts &out);
void read_quality_curve(sqlite3 *db, int64_t curve, items::ItemRarityFacts &out);
void read_item_rarity(sqlite3 *db, int64_t guid, ItemRecord &rec);
inline bool guid_list(sqlite3 *db, GuidQuery query, int64_t parent,
                      std::vector<int64_t> &out, std::string &error) {
  Stmt q(db, query.sql, parent);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW)
    out.push_back(sqlite3_column_int64(q.get(), 0));
  return check_done(db, step, std::string(query.what) + " query failed", error);
}

inline int64_t json_int(const nlohmann::json &o, std::string_view key) {
  return glaze::extract_int(o, key);
}
inline double json_num(const nlohmann::json &o, std::string_view key) {
  return glaze::extract_num(o, key);
}
inline bool json_flag(const nlohmann::json &o, std::string_view key) {
  return glaze::extract_flag(o, key);
}
inline std::string json_str(const nlohmann::json &o, std::string_view key) {
  return glaze::extract_str(o, key);
}

} // namespace aoc::worldd::content
