// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "content/content_db_internal.h"

namespace aoc::worldd::content {
namespace {

constexpr const char *kProfessionTableSql =
    "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
    "('profession', 'certification_level', 'action_certification')";
constexpr const char *kCurrencyTableSql =
    "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
    "('currency_tier', 'currency_def')";
constexpr const char *kCurrencyTierSql =
    "SELECT guid, name, max_base_quantity FROM currency_tier ORDER BY guid";
constexpr const char *kCurrencyDefSql =
    "SELECT guid, name, tier_guid, ord, item_guid, conversion_rate FROM "
    "currency_def ORDER BY tier_guid, ord, guid";
constexpr const char *kProfessionSql =
    "SELECT p.guid, p.name, p.category, p.leveling_type_guid, "
    "p.skill_tree_guid, l.name FROM profession p LEFT JOIN leveling_type l ON "
    "l.guid = p.leveling_type_guid ORDER BY p.guid";
constexpr const char *kCertificationSql =
    "SELECT guid, name, tier, min_level, max_level, has_max_allowable, "
    "max_allowable FROM certification_level ORDER BY tier";
constexpr const char *kCertificationActionSql =
    "SELECT action_guid, name, profession_guid, action FROM "
    "action_certification ORDER BY action_guid";
constexpr const char *kAutoGrantSql =
    "SELECT tree_guid, tree_name, leveling_type_guid, node_guid, node_name, "
    "level_requirement, skill_guid, skill_name, rank_guid, effect_guid FROM "
    "skill_auto_grant WHERE leveling_type_guid = ?1 ORDER BY "
    "level_requirement, node_guid";
constexpr const char *kRankModSql =
    "SELECT stat_guid, stat_name, value FROM skill_rank_stat_mod WHERE "
    "rank_guid = ?1 ORDER BY ord";
constexpr const char *kTreeSql =
    "SELECT guid, name, kind, leveling_type_guid, class_guid, weapon_line_guid "
    "FROM skill_tree ORDER BY guid";
constexpr const char *kNodeSql =
    "SELECT tree_guid, node_guid, ord, name, skill_guid, skill_name, "
    "level_requirement, point_requirement, auto_granted, auto_hotbar, "
    "all_required, opt, slot_guid FROM skill_node ORDER BY tree_guid, ord, opt";
constexpr const char *kRequireSql =
    "SELECT tree_guid, node_guid, ord, skill_guid, slot_node_guid FROM "
    "skill_node_require ORDER BY tree_guid, node_guid, ord";
constexpr const char *kExclusiveSql =
    "SELECT tree_guid, node_guid, ord, skill_guid FROM skill_node_exclusive "
    "ORDER BY tree_guid, node_guid, ord";
constexpr const char *kSkillRankSql =
    "SELECT skill_guid, ord, rank_guid, name, ability_guid, ability_name, "
    "effect_guid, effect_name, effect_infinite, effect_duration FROM "
    "skill_rank ORDER BY skill_guid, ord";
constexpr const char *kRankCostSql =
    "SELECT rank_guid, ord, point_type_guid, quantity FROM skill_rank_cost "
    "ORDER BY rank_guid, ord";
constexpr const char *kAllRankModSql =
    "SELECT rank_guid, stat_guid, stat_name, value FROM skill_rank_stat_mod "
    "ORDER BY rank_guid, ord";
constexpr const char *kPointTypeSql = "SELECT guid, name FROM skill_point_type";
constexpr const char *kLevelingPointSql =
    "SELECT type_guid, level, point_type_guid, quantity FROM leveling_point";
constexpr const char *kItemWeaponSql =
    "SELECT attack_ability_guid, skill_line_guid, skill_line_name FROM "
    "item_weapon WHERE item_guid = ?1";

int count_schema_tables(sqlite3 *db, const char *sql, std::string &error) {
  Stmt q(db, sql);
  int found = 0, step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW)
    ++found;
  return check_done(db, step, "table query failed", error) ? found : -1;
}

bool read_currency_tiers(sqlite3 *db, CurrencyData &out, std::string &error) {
  Stmt q(db, kCurrencyTierSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "currency_tier", "iti", error))
      return false;
    out.tiers.push_back({col_i64(s, 0), column_text(s, 1), col_i64(s, 2)});
  }
  return check_done(db, step, "currency_tier query failed", error);
}

bool read_currency_defs(sqlite3 *db, CurrencyData &out, std::string &error) {
  Stmt q(db, kCurrencyDefSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "currency_def", "itiiii", error))
      return false;
    const int64_t ord = col_i64(s, 3);
    if (ord < 0 || ord > 255)
      return fail_msg("currency_def " + column_text(s, 1) + " has the place " +
                          std::to_string(ord) + " in its tier",
                      error);
    out.defs.push_back({col_i64(s, 0), column_text(s, 1), col_i64(s, 2),
                        static_cast<int>(ord), col_i64(s, 4), col_i64(s, 5)});
  }
  return check_done(db, step, "currency_def query failed", error);
}

bool read_profession_rows(sqlite3 *db, ProfessionData &out,
                          std::vector<std::string> &leveling_names,
                          std::string &error) {
  Stmt q(db, kProfessionSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "profession", "ittii", error))
      return false;
    ProfessionRow r{col_i64(s, 0), column_text(s, 1), column_text(s, 2),
                    col_i64(s, 3), col_i64(s, 4)};
    if (!column_is_text(s, 5))
      return fail_msg("profession " + r.name + " names the leveling type " +
                          std::to_string(r.leveling_type_guid) +
                          ", which content.db does not have",
                      error);
    leveling_names.push_back(column_text(s, 5));
    out.professions.push_back(std::move(r));
  }
  return check_done(db, step, "profession query failed", error);
}

bool read_certification_rows(sqlite3 *db, ProfessionData &out,
                             std::string &error) {
  Stmt q(db, kCertificationSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "certification_level", "itiiiii", error))
      return false;
    const int64_t tier = col_i64(s, 2), lo = col_i64(s, 3), hi = col_i64(s, 4),
                  allow = col_i64(s, 6);
    if (tier < 0 || tier > 255 || lo < 1 || lo > 255 || hi < lo || hi > 255 ||
        allow < 0 || allow > 1000)
      return fail_msg("certification_level " + column_text(s, 1) +
                          " has tier " + std::to_string(tier) + ", levels " +
                          std::to_string(lo) + " to " + std::to_string(hi) +
                          " and a limit of " + std::to_string(allow),
                      error);
    out.certifications.push_back({col_i64(s, 0), column_text(s, 1),
                                  static_cast<int>(tier), static_cast<int>(lo),
                                  static_cast<int>(hi), col_i64(s, 5) != 0,
                                  static_cast<int>(allow)});
  }
  return check_done(db, step, "certification level query failed", error);
}

bool read_certification_actions(sqlite3 *db, ProfessionData &out,
                                std::string &error) {
  Stmt q(db, kCertificationActionSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "action_certification", "itit", error))
      return false;
    const std::string name = column_text(s, 1), what = column_text(s, 3);
    if (what != "Promote" && what != "Demote")
      return fail_msg("action_certification " + name + " is a '" + what +
                          "' (Promote or Demote)",
                      error);
    out.actions[col_i64(s, 0)] = {col_i64(s, 0), name, col_i64(s, 2),
                                  what == "Promote"};
  }
  return check_done(db, step, "certification action query failed", error);
}

bool read_auto_grant_rows(sqlite3 *db, int64_t leveling_type_guid,
                          std::vector<AutoGrant> &out, std::string &error) {
  Stmt q(db, kAutoGrantSql, leveling_type_guid);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "skill_auto_grant", "i.ii.ii.ii", error))
      return false;
    const int64_t level = col_i64(s, 5);
    if (level < 1 || level > 255)
      return fail_msg("skill_auto_grant has level_requirement " +
                          std::to_string(level),
                      error);
    out.push_back({col_i64(s, 0), column_text(s, 1), col_i64(s, 2),
                   col_i64(s, 3), column_text(s, 4), static_cast<int>(level),
                   col_i64(s, 6), column_text(s, 7), col_i64(s, 8),
                   col_i64(s, 9)});
  }
  return check_done(db, step, "auto grant query failed", error);
}

bool read_auto_grant_rank_mods(sqlite3 *db, AutoGrant &g, std::string &error) {
  if (g.rank_guid == 0)
    return true;
  Stmt q(db, kRankModSql, g.rank_guid);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!column_is_int(s, 0) || !column_is_real(s, 2))
      return fail_msg("skill_rank_stat_mod of rank " +
                          std::to_string(g.rank_guid) +
                          " has a column of the wrong storage class",
                      error);
    g.mods.push_back({col_i64(s, 0), column_text(s, 1), col_real(s, 2)});
  }
  return check_done(db, step, "rank mod query failed", error);
}

bool read_auto_grant_mods(sqlite3 *db, std::vector<AutoGrant> &out,
                          std::string &error) {
  for (AutoGrant &g : out) {
    if (!read_auto_grant_rank_mods(db, g, error))
      return false;
  }
  return true;
}

bool read_point_types(sqlite3 *db, TalentData &out, std::string &error) {
  Stmt q(db, kPointTypeSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!check_cols(q.get(), "skill_point_type", "it", error))
      return false;
    out.point_types[column_text(q.get(), 1)] = col_i64(q.get(), 0);
  }
  return check_done(db, step, "skill_point_type query failed", error);
}

bool read_leveling_points(sqlite3 *db, TalentData &out, std::string &error) {
  Stmt q(db, kLevelingPointSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "leveling_point", "iiii", error))
      return false;
    const int64_t level = col_i64(s, 1), qty = col_i64(s, 3);
    if (level < 1 || level > 255 || qty < 0 || qty > 1000)
      return fail_msg("leveling_point has a level or quantity out of range",
                      error);
    out.points[col_i64(s, 0)][static_cast<int>(level)][col_i64(s, 2)] = qty;
  }
  return check_done(db, step, "leveling_point query failed", error);
}

bool read_rank_costs(sqlite3 *db,
                     std::map<int64_t, std::vector<SkillCost>> &costs,
                     std::string &error) {
  Stmt q(db, kRankCostSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "skill_rank_cost", "iiii", error))
      return false;
    const int64_t qty = col_i64(s, 3);
    if (qty < 1 || qty > 1000)
      return fail_msg("skill_rank_cost has a quantity out of range", error);
    costs[col_i64(s, 0)].push_back({col_i64(s, 2), qty});
  }
  return check_done(db, step, "skill_rank_cost query failed", error);
}

bool read_all_rank_mods(sqlite3 *db,
                        std::map<int64_t, std::vector<RankStatMod>> &mods,
                        std::string &error) {
  Stmt q(db, kAllRankModSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "skill_rank_stat_mod", "ii.r", error))
      return false;
    mods[col_i64(s, 0)].push_back(
        {col_i64(s, 1), column_text(s, 2), col_real(s, 3)});
  }
  return check_done(db, step, "skill_rank_stat_mod query failed", error);
}

struct SkillRankBuildMaps {
  std::map<int64_t, std::vector<SkillCost>> costs;
  std::map<int64_t, std::vector<RankStatMod>> mods;
  std::map<int64_t, std::vector<SkillRankRow>> ranks_of_skill;
};

bool read_skill_rank_row(sqlite3_stmt *s, SkillRankBuildMaps &maps,
                         std::string &error) {
  if (!check_cols(s, "skill_rank", "iii.i.i.ir", error))
    return false;
  SkillRankRow r{col_i64(s, 2),     col_int(s, 1),      column_text(s, 3),
                 col_i64(s, 4),     column_text(s, 5),  col_i64(s, 6),
                 column_text(s, 7), col_i64(s, 8) != 0, col_real(s, 9)};
  if (!(r.effect_duration >= 0) || r.effect_duration > 1e7)
    return fail_msg("skill_rank has an effect duration out of range", error);
  if (const auto c = maps.costs.find(r.guid); c != maps.costs.end())
    r.costs = c->second;
  if (r.effect != 0 && r.effect_infinite) {
    if (const auto m = maps.mods.find(r.guid); m != maps.mods.end())
      r.mods = m->second;
  }
  maps.ranks_of_skill[col_i64(s, 0)].push_back(std::move(r));
  return true;
}

bool read_skill_ranks(sqlite3 *db, SkillRankBuildMaps &maps,
                      std::string &error) {
  Stmt q(db, kSkillRankSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!read_skill_rank_row(q.get(), maps, error))
      return false;
  }
  return check_done(db, step, "skill_rank query failed", error);
}

using GateMap = std::map<std::pair<int64_t, int64_t>, std::vector<int64_t>>;
struct SkillGateMaps {
  GateMap requires_of, requires_nodes_of, exclusive_of;
  std::map<int64_t, size_t> tree_index;
};

struct GateQuerySpec {
  const char *sql;
  const char *table;
};

bool read_skill_gate_table(sqlite3 *db, GateQuerySpec spec, GateMap &into,
                           GateMap *slot_into, std::string &error) {
  Stmt q(db, spec.sql);
  int step = SQLITE_ERROR;
  const char *kinds = slot_into ? "iiiii" : "iiii";
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, spec.table, kinds, error))
      return false;
    const std::pair<int64_t, int64_t> key{col_i64(s, 0), col_i64(s, 1)};
    if (slot_into && col_i64(s, 4) != 0)
      (*slot_into)[key].push_back(col_i64(s, 4));
    else
      into[key].push_back(col_i64(s, 3));
  }
  return check_done(db, step, std::string(spec.table) + " query failed", error);
}

bool read_skill_trees(sqlite3 *db, TalentData &out, SkillGateMaps &gates,
                      std::string &error) {
  Stmt q(db, kTreeSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "skill_tree", "i..iii", error))
      return false;
    SkillTreeRow t{col_i64(s, 0), column_text(s, 1), column_text(s, 2),
                   col_i64(s, 3), col_i64(s, 4),     col_i64(s, 5)};
    gates.tree_index[t.guid] = out.trees.size();
    out.trees.push_back(std::move(t));
  }
  return check_done(db, step, "skill_tree query failed", error);
}

bool read_skill_node_row(sqlite3_stmt *s, TalentData &out,
                         const SkillGateMaps &gates,
                         const SkillRankBuildMaps &ranks, std::string &error) {
  if (!check_cols(s, "skill_node", "iii.i.iiiiiii", error))
    return false;
  const auto tree = gates.tree_index.find(col_i64(s, 0));
  if (tree == gates.tree_index.end())
    return fail_msg("skill_node names a tree that is none", error);
  const int64_t level = col_i64(s, 6), points = col_i64(s, 7);
  if (level < 0 || level > 255 || points < 0 || points > 1000)
    return fail_msg("skill_node has a level or point requirement out of range",
                    error);
  SkillNodeRow n{col_i64(s, 1),
                 col_int(s, 2),
                 column_text(s, 3),
                 col_i64(s, 4),
                 column_text(s, 5),
                 static_cast<int>(level),
                 static_cast<int>(points),
                 col_i64(s, 8) != 0,
                 col_i64(s, 9) != 0,
                 col_i64(s, 10) != 0,
                 col_int(s, 11),
                 col_i64(s, 12)};
  const std::pair<int64_t, int64_t> key{col_i64(s, 0), n.node};
  if (const auto r = gates.requires_of.find(key); r != gates.requires_of.end())
    n.requires_skills = r->second;
  if (const auto r = gates.requires_nodes_of.find(key);
      r != gates.requires_nodes_of.end())
    n.requires_nodes = r->second;
  if (const auto e = gates.exclusive_of.find(key);
      e != gates.exclusive_of.end())
    n.exclusive_skills = e->second;
  if (const auto r = ranks.ranks_of_skill.find(n.skill);
      r != ranks.ranks_of_skill.end())
    n.ranks = r->second;
  out.trees[tree->second].nodes.push_back(std::move(n));
  return true;
}

bool read_skill_nodes(sqlite3 *db, TalentData &out, const SkillGateMaps &gates,
                      const SkillRankBuildMaps &ranks, std::string &error) {
  Stmt q(db, kNodeSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!read_skill_node_row(q.get(), out, gates, ranks, error))
      return false;
  }
  return check_done(db, step, "skill_node query failed", error);
}

} // namespace

std::optional<CurrencyData> ContentDb::currency_data(std::string &error) const {
  error.clear();
  const int found = count_schema_tables(db_, kCurrencyTableSql, error);
  if (found <= 0)
    return std::nullopt;
  if (found != 2) {
    fail_msg("has one of the currency tables (currency_tier, currency_def) and "
             "not both: rebuild it with this checkout's contentc",
             error);
    return std::nullopt;
  }
  CurrencyData out;
  if (!read_currency_tiers(db_, out, error) ||
      !read_currency_defs(db_, out, error))
    return std::nullopt;
  return out;
}

std::optional<ProfessionData>
ContentDb::profession_data(std::string &error) const {
  error.clear();
  const int found = count_schema_tables(db_, kProfessionTableSql, error);
  if (found <= 0)
    return std::nullopt;
  if (found != 3) {
    fail_msg("has some of the profession tables (profession, "
             "certification_level, action_certification) and not all: rebuild "
             "it with this checkout's contentc",
             error);
    return std::nullopt;
  }
  ProfessionData out;
  std::vector<std::string> leveling_names;
  if (!read_profession_rows(db_, out, leveling_names, error))
    return std::nullopt;
  for (size_t i = 0; i < out.professions.size(); ++i) {
    std::string why;
    auto type = leveling_type_named(leveling_names[i], why);
    if (!type) {
      error = "the leveling type of profession " + out.professions[i].name +
              ": " + why;
      return std::nullopt;
    }
    out.professions[i].leveling = std::move(*type);
  }
  if (!read_certification_rows(db_, out, error) ||
      !read_certification_actions(db_, out, error))
    return std::nullopt;
  return out;
}

std::optional<std::vector<AutoGrant>>
ContentDb::auto_grants(int64_t leveling_type_guid, std::string &error) const {
  std::vector<AutoGrant> out;
  if (!read_auto_grant_rows(db_, leveling_type_guid, out, error) ||
      !read_auto_grant_mods(db_, out, error))
    return std::nullopt;
  return out;
}

std::optional<TalentData> ContentDb::talent_data(std::string &error) const {
  TalentData out;
  if (!read_point_types(db_, out, error) ||
      !read_leveling_points(db_, out, error))
    return std::nullopt;
  SkillRankBuildMaps ranks;
  if (!read_rank_costs(db_, ranks.costs, error) ||
      !read_all_rank_mods(db_, ranks.mods, error) ||
      !read_skill_ranks(db_, ranks, error))
    return std::nullopt;
  SkillGateMaps gates;
  if (!read_skill_gate_table(db_, {kRequireSql, "skill_node_require"},
                             gates.requires_of, &gates.requires_nodes_of,
                             error) ||
      !read_skill_gate_table(db_, {kExclusiveSql, "skill_node_exclusive"},
                             gates.exclusive_of, nullptr, error) ||
      !read_skill_trees(db_, out, gates, error) ||
      !read_skill_nodes(db_, out, gates, ranks, error))
    return std::nullopt;
  return out;
}

std::optional<ItemWeaponRow> ContentDb::item_weapon(int64_t item_guid,
                                                    std::string &error) const {
  Stmt q(db_, kItemWeaponSql, item_guid);
  const int step = q.step();
  if (step == SQLITE_DONE)
    return std::nullopt;
  if (step != SQLITE_ROW) {
    fail_db(db_, "item_weapon query failed", error);
    return std::nullopt;
  }
  if (!check_cols(q.get(), "item_weapon", "ii", error))
    return std::nullopt;
  return ItemWeaponRow{col_i64(q.get(), 0), col_i64(q.get(), 1),
                       column_text(q.get(), 2)};
}

} // namespace aoc::worldd::content
