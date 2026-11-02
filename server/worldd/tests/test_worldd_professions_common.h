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
// (CoA + C:Y:B)

inline void check_book_load_queries(const pr::Book &book,
                                    const pr::Professions &p) {
  const auto levels = p.levels_by_name();
  CHECK(all_true({levels.size() == 3, levels.at("Mining") == 40,
                  levels.at("Alchemy") == 1, levels.at("Lumberjacking") == 1}),
        "levels_by_name: what GetPlayerProfessionLevel reads");
  const auto rows = p.to_svc();
  CHECK(rows.size() == 3 &&
            all_true({rows[0].profession_guid == kAlchemy,
                      rows[1].profession_guid == kMining,
                      rows[1].certification_tier == 3, rows[1].level == 40,
                      rows[1].xp == 400ll * 41 * 41 - 1, rows[2].level == 1}),
        "to_svc: every profession, in guid order");
  CHECK(
      all_true({pr::Professions().to_svc().empty(), !pr::Professions().valid(),
                !pr::Professions().add_xp(kMining, 5).changed(),
                pr::Professions().level(kMining) == 1}),
      "a set with no book holds nothing and changes nothing");
  const std::vector<pr::Change> d =
      pr::Professions::diff(pr::Professions(book), p);
  CHECK(d.size() == 1 &&
            all_true({d[0].profession == kMining, d[0].before.tier == 0,
                      d[0].after.tier == 3}),
        "diff: only the professions that differ");
}

inline void test_book_and_rules_load(const pr::Book &book,
                                     const pr::Professions &p) {
  check_book_load_queries(book, p);
  std::vector<std::string> notes;
  std::vector<aoc::worldd::svc::ProfessionState> stored = {
      {kMining, 1, 12, 60000},
      {kLumber, 9, 99, -5},
      {kAlchemy, 0, 30, 100},
      {777, 2, 5, 5},
      {kMining, 2, 3, 3}};
  const pr::Professions loaded = pr::Professions::load(book, stored, &notes);
  CHECK(all_true({loaded.tier(kMining) == 1, loaded.level(kMining) == 12,
                  loaded.xp(kMining) == 60000}),
        "a good row is kept");
  CHECK(all_true({loaded.tier(kLumber) == 4, loaded.level(kLumber) == 50,
                  loaded.xp(kLumber) == 400ll * 50 * 50}),
        "a tier past GrandMaster is GrandMaster; the level is held at the "
        "curve and the XP at its threshold");
  CHECK(all_true({loaded.tier(kAlchemy) == 0, loaded.level(kAlchemy) == 10,
                  loaded.xp(kAlchemy) == 40000}),
        "a level past the Novice cap is the cap, with the XP at its threshold");
  CHECK(!notes.empty(), "what was corrected, left out or repeated is said");
  const pr::Professions none = pr::Professions::load(book, {}, nullptr);
  CHECK(all_true({none.valid(), none.level(kMining) == 1,
                  none.tier(kMining) == 0, none.xp(kMining) == 0}),
        "no rows at all is Novice, level 1 everywhere");
  const pr::Professions low = pr::Professions::load(
      book, std::vector<aoc::worldd::svc::ProfessionState>{{kMining, 0, 5, 0}},
      nullptr);
  CHECK(low.level(kMining) == 5 && low.xp(kMining) == 400ll * 5 * 5,
        "a level whose XP trails it keeps the level, the XP at its threshold");
  const char *expr_str =
      "const xp = EvalCurve($#7037:6064$, 25.0);\r\nconst cur = "
      "EvalCurve($#7037:6064$, (1.0 * "
      "GetPlayerProfessionLevel(GetPlayer(), \"Mining\")));\r\nxp - cur + 1";
  CHECK(all_true({pr::profession_named_in(expr_str) == "Mining",
                  pr::profession_named_in("1 + 1").empty(),
                  pr::profession_named_in(
                      "GetPlayerProfessionLevel(GetPlayer(), \"\")")
                      .empty()}),
        "the profession named in an expression's GetPlayerProfessionLevel");
  SAYOK("the rules on a small book");
}

inline void check_book_lookup_and_caps(const pr::Book &book) {
  CHECK(book.size() == 3 && book.all()[0].guid == kAlchemy &&
            book.all()[2].guid == kLumber,
        "the professions are kept in guid order of the data");
  CHECK(book.find(kMining) &&
            all_true({book.find(kMining)->name == "Mining",
                      book.named("Mining") == book.find(kMining),
                      book.named("MINING") == book.find(kMining), !book.find(1),
                      !book.named("Fishing")}),
        "find and named (the name without regard to case)");
  CHECK(all_true({book.tier(0).max_level == 10, book.tier(1).max_level == 20,
                  book.tier(4).max_level == 50, book.tier(9).tier == 4,
                  book.tier(-1).tier == 0}),
        "the tiers' caps; a tier past the ends is clamped");
  CHECK(book.tier_of_guid(kCertGuid[2]) == 2 && book.tier_of_guid(5) == -1,
        "the tier of a certification guid");
  CHECK(book.action(7001) && book.action(7001)->promote && book.action(7002) &&
            !book.action(7002)->promote && !book.action(7003),
        "the certification actions");
}

inline void check_book_xp_and_novice_cap(const pr::Book &book,
                                         pr::Professions &p) {
  const pr::Profession &mining = *book.find(kMining);
  CHECK(all_true({book.max_xp(mining, 0) == 400ll * 11 * 11 - 1,
                  book.max_xp(mining, 1) == 400ll * 21 * 21 - 1,
                  book.max_xp(mining, 4) == 400ll * 50 * 50}),
        "the XP a tier holds: one below the threshold of the level past its "
        "cap; the curve's last total at level 50");
  CHECK(all_true({book.level_of(mining, 0, 0) == 1,
                  book.level_of(mining, 1599, 0) == 1,
                  book.level_of(mining, 1600, 0) == 2,
                  book.level_of(mining, 40000, 0) == 10,
                  book.level_of(mining, 900000, 0) == 10,
                  book.level_of(mining, 900000, 1) == 20,
                  book.level_of(mining, 900000, 4) == 47}),
        "the level of an XP total: the curve's, held at the tier's cap");
  CHECK(all_true({p.valid(), p.level(kMining) == 1, p.tier(kMining) == 0,
                  p.xp(kMining) == 0, p.level(12345) == 1, p.tier(12345) == 0}),
        "a new set is Novice, level 1, no XP (an unknown profession too)");
  CHECK(all_true({p.can_use(kMining, 0, 1), !p.can_use(kMining, 1, 1),
                  !p.can_use(kMining, 0, 2), !p.can_use(999, 0, 1)}),
        "can_use: tier and level at least; an unknown profession can use "
        "nothing");
  pr::Change c = p.add_xp(kMining, 1600);
  CHECK(all_true({c.profession == kMining, c.before.level == 1,
                  c.after.level == 2, c.leveled(), !c.promoted(),
                  p.level(kMining) == 2, p.xp(kMining) == 1600}),
        "1,600 XP is level 2");
  CHECK(!p.add_xp(kMining, 0).changed() && p.xp(kMining) == 1600,
        "no XP is no change");
  CHECK(!p.add_xp(kMining, -5).changed() && p.xp(kMining) == 1600,
        "XP is never taken away");
  c = p.add_xp(999, 100);
  CHECK(!c.changed() && c.profession == 0,
        "XP to an unknown profession changes nothing");
  c = p.add_xp(kMining, 100000);
  CHECK(all_true({c.after.level == 10, c.after.xp == 48399,
                  p.xp(kMining) == 48399, p.level(kMining) == 10}),
        "a Novice's XP is held at the Novice cap (level 10, one XP below level "
        "11)");
  CHECK(!p.add_xp(kMining, 5).changed(), "a Novice at the cap gains nothing");
}

inline void check_book_promote_and_demote(const pr::Book &book,
                                          pr::Professions &p) {
  bool skipped = false;
  pr::Change c = p.promote(kLumber, true, &skipped);
  CHECK(skipped && !c.changed() && p.tier(kLumber) == 0,
        "a Promote below the tier's cap is skipped");
  c = p.promote(kMining, true, &skipped);
  CHECK(all_true({!skipped, c.promoted(), c.after.tier == 1,
                  c.after.level == 10, p.tier(kMining) == 1, c.moved()}),
        "a Promote at the cap is one tier up, the level kept");
  c = p.add_xp(kMining, 1000000);
  CHECK(c.after.level == 20 && c.after.xp == 400ll * 21 * 21 - 1,
        "an Apprentice's XP is held at the Apprentice cap");
  c = p.promote(kMining, false);
  CHECK(c.after.tier == 2 && p.tier(kMining) == 2,
        "a Promote that does not need the cap");
  for (int i = 0; i < 2; ++i) {
    p.add_xp(kMining, 100000000);
    p.promote(kMining);
  }
  p.add_xp(kMining, 100000000);
  c = p.promote(kMining, true, &skipped);
  CHECK(all_true({p.tier(kMining) == 4, p.level(kMining) == 50,
                  p.xp(kMining) == 1000000, !c.changed()}),
        "a GrandMaster stays; level 50 holds the curve's last total");
  c = p.demote(kMining);
  CHECK(all_true({c.demoted(), c.after.tier == 3, c.after.level == 40,
                  c.after.xp == 400ll * 41 * 41 - 1}),
        "a Demote is one tier down: the level and the XP are held at the lower "
        "cap");
  pr::Professions n(book);
  CHECK(!n.demote(kMining).changed() && !n.demote(1).changed(),
        "a Novice stays; an unknown profession changes nothing");
  c = n.raise_xp_to(kAlchemy, 5000);
  CHECK(all_true({c.after.xp == 5000, c.after.level == 3,
                  !n.raise_xp_to(kAlchemy, 100).changed(),
                  n.xp(kAlchemy) == 5000}),
        "raise_xp_to raises, never lowers");
}

inline void test_book_and_rules() {
  std::printf("the professions' rules on a small book\n");
  std::string error;
  const aoc::worldd::content::ProfessionData data = small_data();
  const auto book = pr::Book::from(data, error);
  CHECK(book.has_value(), "the small book builds");
  if (!book) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  check_book_lookup_and_caps(*book);
  pr::Professions p(*book);
  check_book_xp_and_novice_cap(*book, p);
  check_book_promote_and_demote(*book, p);
  test_book_and_rules_load(*book, p);
}

// C-O-A + C-Y-B

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

inline void
check_reader_mining_scroll(const aoc::worldd::content::ContentDb &db,
                           const pr::Book &book, int64_t scroll_id) {
  std::string error;
  const auto use = db.item_use(scroll_id, error);
  CHECK(use && use->actions.size() == 6,
        "it has 6 actions: XP, Promote, XP, Promote, XP, the T3 tool");
  if (!use || use->actions.size() != 6) {
    return;
  }
  const auto xp_action = db.action_response(use->actions[0], error);
  const auto table = xp_action && !xp_action->reward_tables.empty()
                         ? db.reward_table(xp_action->reward_tables[0], error)
                         : std::nullopt;
  const bool xp_ok = table && table->experience.size() == 1 &&
                     table->experience[0].experience_type == "Profession";
  CHECK(xp_ok && table->experience[0].expr > 0 &&
            pr::profession_named_in(table->experience[0].expression) ==
                "Mining",
        "its first action's reward is a Profession XP line with a compiled "
        "expression for Mining");
  const auto cert_action = db.action_response(use->actions[1], error);
  const bool cert_ok = cert_action &&
                       cert_action->response_type == "SetCertificationLevel" &&
                       book.action(cert_action->guid);
  CHECK(cert_ok && book.action(cert_action->guid)->promote &&
            book.action(cert_action->guid)->profession == kMining,
        "its second action is a Promote of Mining");
}

inline void test_reader_scroll_and_xp(const aoc::worldd::content::ContentDb &db,
                                      const pr::Book &book) {
  const auto mining25 =
      items_like(db.path(), "GM_Scroll_MakeMeLevel25Artisanship_Mining");
  CHECK(mining25.size() == 1, "the Mining Level 25 scroll is in the file");
  if (mining25.size() == 1) {
    check_reader_mining_scroll(db, book, mining25[0].first);
  }
  sqlite3 *raw = nullptr;
  int lines = 0, named = 0, compiled = 0;
  if (sqlite3_open_v2(db.path().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) ==
      SQLITE_OK) {
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2(raw,
                           "SELECT expression, expr FROM reward_xp WHERE "
                           "experience_type = 'Profession'",
                           -1, &s, nullptr) == SQLITE_OK) {
      while (sqlite3_step(s) == SQLITE_ROW) {
        ++lines;
        const std::string name = pr::profession_named_in(
            reinterpret_cast<const char *>(sqlite3_column_text(s, 0)));
        named += book.named(name) != nullptr;
        compiled += sqlite3_column_int64(s, 1) > 0;
      }
    }
    sqlite3_finalize(s);
    sqlite3_close(raw);
  }
  CHECK(lines == 88 && named == 88 && compiled == 88,
        "all 88 Profession XP lines name one of the 22 professions and have a "
        "compiled expression");
  SAYOK("the real content.db's professions");
}

inline void check_reader_order_and_curves(
    const aoc::worldd::content::ProfessionData &data) {
  const int64_t retail_order[22] = {
      108651600696952ll, 109460729190018ll, 109460729190017ll,
      109460729190016ll, 109460729190015ll, 109460729190014ll,
      109460729190013ll, 109460729190012ll, 109460747540099ll,
      109460747540100ll, 109460747540101ll, 109460747540102ll,
      109460747540103ll, 109460747540104ll, 109460747540105ll,
      109460747540106ll, 109460747540107ll, 109460719621753ll,
      109273709237880ll, 109460724995707ll, 109460713264760ll,
      109460722964090ll};
  bool order = data.professions.size() == 22;
  for (size_t i = 0; order && i < 22; ++i) {
    order = data.professions[i].leveling_type_guid == retail_order[i];
  }
  CHECK(order, "the professions' leveling types are in the order of the retail "
               "join's certification map");
  bool curves = true;
  for (const aoc::worldd::content::ProfessionRow &p : data.professions) {
    const auto &c = p.leveling.curve;
    curves = curves && c.size() == 50 &&
             all_true({c[0].second == 0, c[9].second == 40000,
                       c[49].second == 1000000, c[24].second == 250000});
  }
  CHECK(curves, "every profession's curve has 50 levels: 0 at level 1, 40,000 "
                "at 10, 250,000 at 25, 1,000,000 at 50");
}

inline void check_reader_tiers_and_actions(
    const aoc::worldd::content::ProfessionData &data) {
  const int max[5] = {10, 20, 30, 40, 50};
  const int allow[5] = {0, 5, 4, 3, 2};
  bool tiers = data.certifications.size() == 5;
  for (int t = 0; t < 5 && tiers; ++t) {
    const auto &cert = data.certifications[static_cast<size_t>(t)];
    tiers =
        all_true({cert.tier == t, cert.max_level == max[t],
                  cert.max_allowable == allow[t], cert.guid == kCertGuid[t]});
  }
  CHECK(tiers, "the certification levels: Novice 10, Apprentice 20, Journeyman "
               "30, Master 40, GrandMaster 50; at most 5, 4, 3, 2 professions "
               "above Novice");
  int promotes = 0, demotes = 0;
  for (const auto &[guid, a] : data.actions) {
    (a.promote ? promotes : demotes)++;
  }
  CHECK(data.actions.size() == 98 && promotes == 70 && demotes == 28,
        "98 certification actions: 70 Promote, 28 Demote");
}

inline void test_reader(const aoc::worldd::content::ContentDb &db) {
  std::printf("the real content.db's professions\n");
  std::string error;
  const auto data = db.profession_data(error);
  CHECK(data.has_value(), "profession_data reads");
  if (!data) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  CHECK(data->professions.size() == 22 && data->certifications.size() == 5,
        "22 professions and 5 certification levels");
  check_reader_order_and_curves(*data);
  check_reader_tiers_and_actions(*data);
  const auto book = pr::Book::from(*data, error);
  CHECK(book.has_value(), "the real book builds");
  if (!book) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  test_reader_scroll_and_xp(db, *book);
}
// C O A / Cyb

namespace model {
struct M {
  int tier = 0;
  int level = 1;
  int64_t xp = 0;
};
inline int64_t curve(int l) { return l <= 1 ? 0 : 400ll * l * l; }
inline int cap(int tier) { return 10 * (tier + 1); }
inline int64_t hold(int tier) {
  return tier >= 4 ? curve(50) : curve(cap(tier) + 1) - 1;
}
inline int level_of(int64_t xp, int tier) {
  int l = 1;
  while (l < cap(tier) && xp >= curve(l + 1)) {
    ++l;
  }
  return l;
}
inline void xp_line(M &m, int target) {
  const int64_t delta = curve(target) - curve(m.level) + 1;
  if (delta > 0) {
    m.xp = std::min(hold(m.tier), m.xp + delta);
  }
  m.level = level_of(m.xp, m.tier);
}
inline void promote(M &m) {
  if (m.tier < 4 && m.level == cap(m.tier)) {
    ++m.tier;
  }
}
} // namespace model

inline std::string normalized(std::string s) {
  std::string out;
  for (char c : s) {
    if (c != ' ' && c != '_') {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (out == "scribing") {
    out = "scribe";
  }
  return out;
}

inline const pr::Profession *find_profession_by_suffix(const pr::Book &book,
                                                       const std::string &sfx) {
  for (const pr::Profession &p : book.all()) {
    if (normalized(p.name) == sfx) {
      return &p;
    }
  }
  return nullptr;
}

inline model::M simulate_scroll_model(int level) {
  model::M m;
  model::xp_line(m, level);
  if (level >= 15) {
    model::promote(m);
    model::xp_line(m, level);
  }
  if (level >= 25) {
    model::promote(m);
    model::xp_line(m, level);
  }
  return m;
}

inline bool check_scroll_plan_matches(const aoc::worldd::use::Plan &plan,
                                      const pr::Profession &prof,
                                      const model::M &m, bool tool) {
  const pr::State got = plan.professions_after.valid()
                            ? plan.professions_after.state(prof.guid)
                            : pr::State{};
  return plan.profession_changes.size() == 1 &&
         all_true({plan.ok, plan.consume, got.level == m.level,
                   got.tier == m.tier, got.xp == m.xp,
                   plan.items.size() == (tool ? 1u : 0u),
                   plan.profession_changes[0].profession == prof.guid,
                   plan.adventuring_xp == 0, !plan.set_class});
}

inline bool check_other_professions_untouched(
    const pr::Book &book, const aoc::worldd::use::Plan &plan, int64_t target) {
  bool untouched = true;
  for (const pr::Profession &other : book.all()) {
    if (other.guid != target) {
      untouched =
          untouched && plan.professions_after.state(other.guid) == pr::State{};
    }
  }
  return untouched;
}

struct ScrollSweepCounts {
  int checked = 0;
  int wrong = 0;
  int tools = 0;
  int sets = 0;
};

inline void sweep_scrolls_at_level(const aoc::worldd::content::ContentDb &db,
                                   const pr::Book &book,
                                   const aoc::worldd::use::Context &ctx,
                                   int level, ScrollSweepCounts &c) {
  const std::set<std::string> gathering = {"fishing", "herbalism", "hunting",
                                           "lumberjacking", "mining"};
  const std::string pattern =
      "GM_Scroll_MakeMeLevel" + std::to_string(level) + "Artisanship_%";
  for (const auto &[guid, name] : items_like(db.path(), pattern.c_str())) {
    const std::string suffix =
        normalized(name.substr(name.find("Artisanship_") + 12));
    if (suffix == "all") {
      continue;
    }
    const pr::Profession *prof = find_profession_by_suffix(book, suffix);
    ++c.checked;
    if (!prof) {
      ++c.wrong;
      continue;
    }
    const aoc::worldd::use::Plan plan =
        aoc::worldd::use::plan_use(db, guid, ctx);
    const model::M m = simulate_scroll_model(level);
    const bool tool = level == 25 && gathering.count(suffix) > 0;
    c.wrong += check_scroll_plan_matches(plan, *prof, m, tool) ? 0 : 1;
    CHECK(check_other_professions_untouched(book, plan, prof->guid),
          "a scroll changes only its own profession");
    c.tools += tool && plan.items.size() == 1 ? 1 : 0;
    c.sets += plan.ok ? 1 : 0;
  }
}

inline void test_scrolls_single(const std::string &path,
                                const aoc::worldd::content::ContentDb &db,
                                const pr::Book &book,
                                const aoc::worldd::use::Context &ctx) {
  ScrollSweepCounts c;
  for (const int level : {9, 15, 25}) {
    sweep_scrolls_at_level(db, book, ctx, level, c);
  }
  CHECK(c.checked == 66 && c.wrong == 0,
        "the 22 + 22 + 22 single-profession scrolls: the final level, tier and "
        "XP are the model's (Level 9: level 9 Novice; Level 15: level 15 "
        "Apprentice; Level 25: level 25 Journeyman)");
  CHECK(c.checked == 66 && c.sets == 66 && c.tools == 5,
        "all 66 are used; the Level 25 scrolls of Fishing, Herbalism, Hunting, "
        "Lumberjacking and Mining give a T3 tool each");
  const auto mining =
      items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
  const aoc::worldd::use::Plan plan =
      aoc::worldd::use::plan_use(db, mining[0].first, ctx);
  CHECK(
      plan.ok && plan.items.size() == 1 &&
          plan.items[0].record == 6064629376467927053ll &&
          plan.items[0].quantity == 1,
      "the Mining Level 25 scroll gives Gear_Artisan_Mining_Tool_Pickaxe_T3_0");
  CHECK(contains(plan.summary, "Mining") && contains(plan.summary, "25") &&
            contains(plan.summary, "Journeyman"),
        "its summary says the profession, the level and the tier");
}
// C O A : Cyb

inline void check_scroll_all_plan(const std::string &path,
                                  const aoc::worldd::content::ContentDb &db,
                                  const pr::Book &book,
                                  const aoc::worldd::use::Context &ctx) {
  const auto all = items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_All");
  CHECK(all.size() == 1, "the Level 25 _All scroll is in the file");
  if (all.size() != 1) {
    return;
  }
  const aoc::worldd::use::Plan plan =
      aoc::worldd::use::plan_use(db, all[0].first, ctx);
  bool every = plan.ok && plan.professions_after.valid() &&
               plan.profession_changes.size() == 22;
  for (const pr::Profession &p : book.all()) {
    const pr::State s = plan.professions_after.state(p.guid);
    every = every && s.level == 25 && s.tier == 2 && s.xp == 250000;
  }
  CHECK(every,
        "_All: all 22 professions are level 25, Journeyman, 250,000 XP "
        "(the data's single Promote leaves them Apprentice at level 10)");
  bool modelled = false, ignores = false;
  for (const std::string &n : plan.notes) {
    modelled = modelled || contains(n, "modelled");
    ignores = ignores || contains(n, "MaximumAllowable");
  }
  CHECK(modelled && ignores && plan.items.empty(),
        "_All says it is modelled and that a GM item ignores the "
        "certification's limit; it gives no item");
  std::printf("    _All: %s\n", plan.summary.c_str());
}

inline void check_scroll_refusal_cases(
    const std::string &path, const aoc::worldd::content::ContentDb &db,
    const pr::Book &book, const aoc::worldd::use::Context &ctx) {
  const auto mining =
      items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
  const pr::State s = aoc::worldd::use::plan_use(db, mining[0].first, ctx)
                          .professions_after.state(kMining);
  CHECK(s.level == 25 && s.tier == 2 && s.xp == 266400,
        "Level 25 from Novice: 250,001 held at 48,399; +210,001 held at "
        "176,399; +90,001 is 266,400 (level 25)");
  const auto l9 = items_like(path, "GM_Scroll_MakeMeLevel9Artisanship_Mining");
  aoc::worldd::use::Context later = ctx;
  later.held =
      pr::Professions::load(book,
                            std::vector<aoc::worldd::svc::ProfessionState>{
                                {kMining, 1, 11, 400ll * 11 * 11}},
                            nullptr);
  const aoc::worldd::use::Plan again =
      aoc::worldd::use::plan_use(db, l9[0].first, later);
  CHECK(!again.ok && contains(again.refusal, "Mining") &&
            contains(again.refusal, "nothing"),
        "a Level 9 scroll for a level 11 Mining refuses: nothing would change "
        "(the scroll stays)");
  CHECK(!again.consume || !again.ok, "a refusal consumes nothing");
  const auto l25 =
      items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Alchemy");
  aoc::worldd::use::Context done = ctx;
  done.held = pr::Professions::load(
      book,
      std::vector<aoc::worldd::svc::ProfessionState>{{kAlchemy, 2, 25, 250001}},
      nullptr);
  const aoc::worldd::use::Plan nothing =
      aoc::worldd::use::plan_use(db, l25[0].first, done);
  CHECK(!nothing.ok && contains(nothing.refusal, "Alchemy"),
        "a Level 25 scroll for a level 25+ Alchemist (no tool in it) refuses");
  aoc::worldd::use::Context mined = ctx;
  mined.held =
      pr::Professions::load(book,
                            std::vector<aoc::worldd::svc::ProfessionState>{
                                {kMining, 3, 33, 400ll * 33 * 33}},
                            nullptr);
  const aoc::worldd::use::Plan tool_only =
      aoc::worldd::use::plan_use(db, mining[0].first, mined);
  CHECK(tool_only.ok && tool_only.items.size() == 1,
        "a Level 25 Mining scroll for a Master miner still gives the tool");
}

inline void check_scroll_mid_and_missing_ctx(
    const std::string &path, const aoc::worldd::content::ContentDb &db,
    const pr::Book &book, const aoc::worldd::use::Context &ctx) {
  const auto mining =
      items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
  const auto l15 =
      items_like(path, "GM_Scroll_MakeMeLevel15Artisanship_Mining");
  aoc::worldd::use::Context mid = ctx;
  mid.held =
      pr::Professions::load(book,
                            std::vector<aoc::worldd::svc::ProfessionState>{
                                {kMining, 1, 12, 400ll * 12 * 12}},
                            nullptr);
  CHECK(mid.held.level(kMining) == 12 && mid.held.tier(kMining) == 1,
        "(set up: Apprentice, level 12)");
  const aoc::worldd::use::Plan p15 =
      aoc::worldd::use::plan_use(db, l15[0].first, mid);
  const pr::State s15 = p15.professions_after.state(kMining);
  bool skipped = false;
  for (const std::string &n : p15.notes) {
    skipped = skipped || contains(n, "skipped");
  }
  CHECK(p15.ok && s15.level == 15 && s15.tier == 1 && skipped,
        "a Level 15 scroll for an Apprentice at level 12: level 15, still "
        "Apprentice, the Promote below the cap is skipped and said so");
  aoc::worldd::use::Context bare = ctx, no_run = ctx;
  bare.professions = nullptr;
  bare.held = pr::Professions();
  no_run.profession_xp = nullptr;
  const aoc::worldd::use::Plan no_book =
      aoc::worldd::use::plan_use(db, mining[0].first, bare);
  const aoc::worldd::use::Plan no_runner =
      aoc::worldd::use::plan_use(db, mining[0].first, no_run);
  CHECK(!no_book.ok && contains(no_book.refusal, "profession"),
        "without professions worldd refuses a profession item and says why");
  CHECK(!no_runner.ok && contains(no_runner.refusal, "expression"),
        "without an expression runner a profession XP reward is refused and "
        "says why");
}

inline void test_scrolls_all_and_refusals(
    const std::string &path, const aoc::worldd::content::ContentDb &db,
    const pr::Book &book, const aoc::worldd::use::Context &ctx,
    const pr::XpRunner &runner) {
  check_scroll_all_plan(path, db, book, ctx);
  check_scroll_refusal_cases(path, db, book, ctx);
  check_scroll_mid_and_missing_ctx(path, db, book, ctx);
  // [C.O.A ~ Cyb]
  const auto mining =
      items_like(path, "GM_Scroll_MakeMeLevel25Artisanship_Mining");
  std::string e, why;
  const auto use_m = db.item_use(mining[0].first, e);
  const auto action = db.action_response(use_m->actions[0], e);
  const auto table = db.reward_table(action->reward_tables[0], e);
  const int64_t expr = table->experience[0].expr;
  const auto at1 = runner(expr, {{"Mining", 1}}, why);
  const auto at10 = runner(expr, {{"Mining", 10}}, why);
  const auto at25 = runner(expr, {{"Mining", 25}}, why);
  CHECK(at1 && at10 && at25 &&
            all_true({std::fabs(*at1 - 250001.0) < 0.5,
                      std::fabs(*at10 - 210001.0) < 0.5,
                      std::fabs(*at25 - 1.0) < 0.5}),
        "the Mining Level 25 expression: xp - cur + 1 is 250,001 at level 1, "
        "210,001 at level 10 and 1 at level 25");
  CHECK(!runner(0, {}, why) && !why.empty(),
        "an expression id of none cannot run");
  SAYOK("the 67 scrolls");
}

inline const aoc::worldd::content::CraftRecipe *
recipe_named(const aoc::worldd::content::CraftRecipeData &d, const char *name) {
  for (const auto &[guid, r] : d.recipes) {
    if (r.name == name) {
      return &r;
    }
  }
  return nullptr;
}
