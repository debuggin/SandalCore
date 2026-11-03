// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "content/content_db.h"
#include "crafting/artisan.h"
#include "crafting/rewards.h"
#include "session/ability_catalog.h"
#include "session/item_use.h"
#include "session/player_world.h"
#include "session/professions.h"

#include <sqlite3.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace pr = aoc::worldd::professions;
namespace cr = aoc::worldd::crafting;

inline int g_pass = 0;
inline int g_fail = 0;

inline void CHECK(bool cond, const char *msg) {
  if (cond) {
    g_pass++;
  } else {
    std::printf("  [FAIL] %s\n", msg);
    g_fail++;
  }
}

#define SAYOK(msg) std::printf("  [ok ] %s\n", msg)

inline bool all_true(std::initializer_list<bool> bs) {
  for (bool b : bs) {
    if (!b) {
      return false;
    }
  }
  return true;
}

inline bool contains(const std::string &s, const char *part) {
  return s.find(part) != std::string::npos;
}

inline constexpr int64_t kMining = 108752769996425ll;
inline constexpr int64_t kLumber = 108752769996426ll;
inline constexpr int64_t kAlchemy = 108752757020295ll;
inline constexpr int64_t kLtMining = 109460719621753ll;
inline constexpr int64_t kLtLumber = 109273709237880ll;
inline constexpr int64_t kLtAlchemy = 109460747540106ll;
inline constexpr int64_t kCertGuid[5] = {108906339653240ll, 108906340767353ll,
                                         108906342012538ll, 108906343061115ll,
                                         108906343782012ll};

inline aoc::worldd::content::LevelingType curve400(int64_t guid,
                                                   const char *name) {
  aoc::worldd::content::LevelingType t;
  t.guid = guid;
  t.name = name;
  t.experience_type = "Gathering";
  for (int l = 1; l <= 50; ++l) {
    t.curve.emplace_back(l, l == 1 ? 0 : 400ll * l * l);
  }
  return t;
}

inline aoc::worldd::content::ProfessionData small_data() {
  aoc::worldd::content::ProfessionData d;
  d.professions.push_back({kAlchemy, "Alchemy", "Processing", kLtAlchemy, 1,
                           curve400(kLtAlchemy, "LevelingTypeDef_Alchemist")});
  d.professions.push_back({kMining, "Mining", "Gathering", kLtMining, 2,
                           curve400(kLtMining, "LevelingTypeDef_Miner")});
  d.professions.push_back({kLumber, "Lumberjacking", "Gathering", kLtLumber, 3,
                           curve400(kLtLumber, "LevelingTypeDef_Lumberjack")});
  const char *names[5] = {"Novice", "Apprentice", "Journeyman", "Master",
                          "GrandMaster"};
  const int allow[5] = {0, 5, 4, 3, 2};
  for (int t = 0; t < 5; ++t) {
    d.certifications.push_back({kCertGuid[t], names[t], t, t == 0 ? 1 : 10 * t,
                                10 * (t + 1), t > 0, allow[t]});
  }
  d.actions[7001] = {7001, "Promote Mining", kMining, true};
  d.actions[7002] = {7002, "Demote Mining", kMining, false};
  return d;
}

inline std::vector<std::pair<int64_t, std::string>>
items_like(const std::string &path, const char *pattern) {
  std::vector<std::pair<int64_t, std::string>> out;
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    return out;
  }
  sqlite3_stmt *s = nullptr;
  if (sqlite3_prepare_v2(
          db, "SELECT guid, name FROM item WHERE name LIKE ?1 ORDER BY name",
          -1, &s, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(s, 1, pattern, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {
      out.emplace_back(
          sqlite3_column_int64(s, 0),
          reinterpret_cast<const char *>(sqlite3_column_text(s, 1)));
    }
  }
  sqlite3_finalize(s);
  sqlite3_close(db);
  return out;
}

void run_professions_artisan_tests(const std::string &path,
                                   const aoc::worldd::content::ContentDb &db,
                                   aoc::worldd::PlayerWorld &world);
