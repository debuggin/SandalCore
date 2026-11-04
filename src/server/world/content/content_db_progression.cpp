// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "content/content_db_internal.h"

#include <cctype>
#include <cstdlib>

namespace aoc::worldd::content {
namespace {

constexpr const char *kSetBonusSql =
    "SELECT tier_ord, pieces, stat_guid, common, uncommon, rare, heroic, epic, "
    "legendary, artifact FROM set_bonus WHERE set_guid = ?1 "
    "ORDER BY tier_ord, bonus_ord";
constexpr const char *kEquipSlotSql = "SELECT slot, name FROM equip_slot";
constexpr const char *kItemUseSql =
    "SELECT item_guid, activation, consume_on_use, cooldown, "
    "disabled_in_combat, cast_ability_guid FROM item_use WHERE item_guid = ?1";
constexpr const char *kItemActionSql =
    "SELECT action_guid FROM item_action WHERE item_guid = ?1 ORDER BY ord";
constexpr const char *kItemHasTagSql =
    "SELECT 1 FROM item_tag WHERE item_guid = ?1 AND (tag = ?2 OR tag LIKE ?2 "
    "|| '.%') LIMIT 1";
constexpr const char *kActionSql =
    "SELECT guid, name, response_type, class_guid, class_name, "
    "remove_up_to_quantity FROM action_response WHERE guid = ?1";
constexpr const char *kActionRewardSql =
    "SELECT reward_table_guid FROM action_reward WHERE action_guid = ?1 ORDER "
    "BY ord";
constexpr const char *kRewardTableSql =
    "SELECT guid, name, predicate, level_below, modelled FROM reward_table "
    "WHERE guid = ?1";
constexpr const char *kRewardItemSql =
    "SELECT item_guid, variation_guid, quantity FROM reward_item WHERE "
    "table_guid = ?1 ORDER BY ord";
constexpr const char *kRewardXpSql =
    "SELECT experience_type, target_level, margin, curve_guid, mult_stat_guid, "
    "expr, expression FROM reward_xp WHERE table_guid = ?1 ORDER BY ord";
constexpr const char *kLevelingTypeSql =
    "SELECT guid, name, experience_type, level_cap, uses_debt, "
    "xp_mult_stat_guid FROM leveling_type WHERE name = ?1";
constexpr const char *kLevelingCurveSql =
    "SELECT level, xp FROM leveling_curve WHERE type_guid = ?1 ORDER BY level";
constexpr const char *kVariationQualitySql =
    "SELECT e.text FROM item_variation v JOIN ab_expr e ON e.id = "
    "v.quality_expr WHERE v.guid = ?1";
constexpr const char *kGatherTableSql =
    "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
    "('gather_def', 'gather_tool', 'item_variation')";
constexpr const char *kGatherDefSql =
    "SELECT guid, name, empty_duration, override_respawn, interaction_guid, "
    "interact_radius, interact_tolerance, allow_in_combat, profession_guid, "
    "tool_slot, cert_guid, cert_tier, base_duration, duration_expr, "
    "durability_expr, reward_guid FROM gather_def ORDER BY guid";
constexpr const char *kGatherToolSql =
    "SELECT item_guid, item_name, profession_guid, cert_guid, cert_tier, "
    "tool_slot, required_profession, required_cert FROM gather_tool ORDER BY "
    "item_guid";
constexpr const char *kItemVariationSql =
    "SELECT guid, quality_expr, durability_expr FROM item_variation ORDER BY "
    "guid";
constexpr const char *kFoliageTableSql =
    "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
    "('foliage_def', 'foliage_state', 'gather_interaction', "
    "'gather_interaction_def')";
constexpr const char *kFoliageDefSql =
    "SELECT guid, name, empty_duration, dynamic_guid, paint_state, "
    "gatherable_state, min_state, max_state, state0_guid, state1_guid, "
    "state2_guid FROM foliage_def ORDER BY guid";
constexpr const char *kFoliageStateSql =
    "SELECT guid, name, lifecycle_duration, interaction_guid, "
    "override_distance, override_angle, collision_radius FROM foliage_state "
    "ORDER BY guid";
constexpr const char *kGatherInteractionSql =
    "SELECT guid, name, target_state, profession_guid, cert_guid, cert_tier, "
    "tool_slot, base_duration, duration_expr, durability_expr, reward_guid, "
    "interaction_guid, interact_radius, interact_tolerance, allow_in_combat "
    "FROM gather_interaction ORDER BY guid";
constexpr const char *kGatherInteractionDefSql =
    "SELECT interaction_guid, name, gather_guid, interact_radius, "
    "interact_tolerance, discovery_distance, allow_in_combat FROM "
    "gather_interaction_def ORDER BY interaction_guid";
constexpr const char *kLevelingNamesSql =
    "SELECT name FROM leveling_type WHERE experience_type = ?1 AND "
    "substr(name, 1, length(?2)) = ?2 ORDER BY guid";
constexpr const char *kStatCurveSql =
    "SELECT name, level, value, interp FROM stat_curve WHERE guid = ?1 ORDER "
    "BY ord";

bool fail_set(int64_t guid, const std::string &msg, std::string &error) {
  error = "content.db's set " + std::to_string(guid) + " " + msg;
  return false;
}
bool read_set_bonus_entry(sqlite3_stmt *s, int64_t guid, stats::SetEntry &e,
                          std::string &error) {
  e.stat = sqlite3_column_int64(s, 2);
  for (int r = 0; r < stats::kRarities; ++r) {
    const int type = sqlite3_column_type(s, 3 + r);
    if (type == SQLITE_NULL)
      continue;
    if (type != SQLITE_FLOAT && type != SQLITE_INTEGER)
      return fail_set(guid,
                      "has a bonus of stat " + std::to_string(e.stat) +
                          " that is not a number",
                      error);
    e.has[static_cast<size_t>(r)] = true;
    e.value[static_cast<size_t>(r)] = sqlite3_column_double(s, 3 + r);
  }
  return true;
}
bool read_set_bonus_row(sqlite3_stmt *s, int64_t guid, int64_t &tier_ord,
                        stats::SetBonus &out, std::string &error) {
  if (!column_is_int(s, 0) || !column_is_int(s, 1) || !column_is_int(s, 2))
    return fail_set(
        guid, "has a tier order, piece count or stat that is not an integer",
        error);
  const int64_t ord = sqlite3_column_int64(s, 0);
  if (ord < 0)
    return fail_set(guid, "has a negative tier order", error);
  if (out.tiers.empty() || ord != tier_ord) {
    const int64_t pieces = sqlite3_column_int64(s, 1);
    if (pieces < 1 || pieces > 83)
      return fail_set(
          guid, "has a tier of " + std::to_string(pieces) + " pieces", error);
    out.tiers.push_back({static_cast<int>(pieces), {}});
    tier_ord = ord;
  }
  stats::SetEntry e;
  if (!read_set_bonus_entry(s, guid, e, error))
    return false;
  out.tiers.back().entries.push_back(e);
  return true;
}
int read_reward_table_header(sqlite3 *db, int64_t guid, RewardTable &t,
                             std::string &error) {
  Stmt q(db, kRewardTableSql, guid);
  const int step = q.step();
  if (step == SQLITE_DONE)
    return 0;
  if (step != SQLITE_ROW)
    return fail_db(db, "reward table query failed", error) ? 0 : -1;
  sqlite3_stmt *s = q.get();
  if (!check_cols(s, "reward_table " + std::to_string(guid), "?ttii", error))
    return -1;
  const int64_t below = sqlite3_column_int64(s, 3);
  if (below < -1 || below > 255)
    return fail_msg("reward_table " + std::to_string(guid) +
                        " has level_below " + std::to_string(below),
                    error)
               ? 0
               : -1;
  t = {col_i64(s, 0), col_str(s, 1), col_str(s, 2), static_cast<int>(below),
       col_i64(s, 4) != 0};
  return 1;
}
bool read_reward_items(sqlite3 *db, int64_t guid, RewardTable &t,
                       std::string &error) {
  Stmt q(db, kRewardItemSql, guid);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "reward_item of table " + std::to_string(guid), "iii",
                    error))
      return false;
    const int64_t qty = sqlite3_column_int64(s, 2);
    if (qty < 1 || qty > 1000000)
      return fail_msg("reward_item of table " + std::to_string(guid) +
                          " grants " + std::to_string(qty) + " of an item",
                      error);
    t.items.push_back(
        {col_i64(s, 0), col_i64(s, 1), static_cast<int32_t>(qty)});
  }
  return check_done(db, step, "reward item query failed", error);
}
bool read_reward_xp(sqlite3 *db, int64_t guid, RewardTable &t,
                    std::string &error) {
  Stmt q(db, kRewardXpSql, guid);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "reward_xp of table " + std::to_string(guid), "tiriiit",
                    error))
      return false;
    const int64_t target = sqlite3_column_int64(s, 1);
    if (target < 0 || target > 255)
      return fail_msg("reward_xp of table " + std::to_string(guid) +
                          " has target_level " + std::to_string(target),
                      error);
    t.experience.push_back({col_str(s, 0), static_cast<int>(target),
                            col_real(s, 2), col_i64(s, 3), col_i64(s, 4),
                            col_i64(s, 5), col_str(s, 6)});
  }
  return check_done(db, step, "reward xp query failed", error);
}
bool read_leveling_type_header(sqlite3 *db, const std::string &name,
                               LevelingType &t, std::string &error) {
  Stmt q(db, kLevelingTypeSql);
  if (q.ok())
    sqlite3_bind_text(q.get(), 1, name.c_str(), static_cast<int>(name.size()),
                      SQLITE_TRANSIENT);
  const int step = q.step();
  if (step == SQLITE_DONE)
    return fail_msg("has no leveling type named '" + name + "'", error);
  if (step != SQLITE_ROW)
    return fail_db(db, "leveling type query failed", error);
  sqlite3_stmt *s = q.get();
  if (!check_cols(s, "leveling_type '" + name + "'", "ittiii", error))
    return false;
  const int64_t cap = sqlite3_column_int64(s, 3);
  if (cap < 0 || cap > 255)
    return fail_msg("leveling_type '" + name + "' has level_cap " +
                        std::to_string(cap),
                    error);
  t = {col_i64(s, 0), col_str(s, 1),      col_str(s, 2),
       col_int(s, 3), col_i64(s, 4) != 0, col_i64(s, 5)};
  return true;
}
bool read_leveling_curve(sqlite3 *db, const std::string &name, LevelingType &t,
                         std::string &error) {
  Stmt q(db, kLevelingCurveSql, t.guid);
  int step = SQLITE_ERROR;
  int64_t last_level = 0, last_xp = -1;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!column_is_int(s, 0) || !column_is_int(s, 1))
      return fail_msg("leveling_curve of '" + name +
                          "' has a key that is not a whole level at a whole XP "
                          "total",
                      error);
    const int64_t level = sqlite3_column_int64(s, 0),
                  xp = sqlite3_column_int64(s, 1);
    if (level < 1 || level > 255 || level <= last_level || xp < 0 ||
        xp < last_xp)
      return fail_msg(
          "leveling_curve of '" + name +
              "' is not ascending whole levels from 1 at non-decreasing XP "
              "(level " +
              std::to_string(level) + " at " + std::to_string(xp) + ")",
          error);
    last_level = level;
    last_xp = xp;
    t.curve.emplace_back(static_cast<int>(level), xp);
  }
  if (!check_done(db, step, "leveling curve query failed", error))
    return false;
  if (t.curve.empty() || t.curve.front().first != 1)
    return fail_msg(
        "leveling_curve of '" + name + "' does not start at level 1", error);
  return true;
}
int count_present_tables(sqlite3 *db, const char *sql, std::string &error) {
  Stmt q(db, sql);
  int found = 0, step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW)
    ++found;
  return check_done(db, step, "table query failed", error) ? found : -1;
}
bool foliage_state_ok(int64_t v) {
  return v >= kFoliageInfancy && v <= kFoliageEmpty;
}
bool read_foliage_defs(sqlite3 *db, FoliageData &out, std::string &error) {
  Stmt q(db, kFoliageDefSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "foliage_def", "itriiiiiiii", error))
      return false;
    const int64_t paint = col_i64(s, 4), gath = col_i64(s, 5),
                  lo = col_i64(s, 6), hi = col_i64(s, 7);
    const double empty_dur = col_real(s, 2);
    if (!foliage_state_ok(paint) || !foliage_state_ok(gath) ||
        !foliage_state_ok(lo) || !foliage_state_ok(hi) || !(empty_dur >= 0))
      return fail_msg("foliage_def " + col_str(s, 1) +
                          " has a state outside Infancy..Empty or a negative "
                          "empty duration",
                      error);
    FoliageDefRow r{col_i64(s, 0), col_str(s, 1), empty_dur,     col_i64(s, 3),
                    col_int(s, 4), col_int(s, 5), col_int(s, 6), col_int(s, 7),
                    col_i64(s, 8), col_i64(s, 9), col_i64(s, 10)};
    out.defs_by_name.emplace(r.name, r.guid);
    out.defs[r.guid] = std::move(r);
  }
  return check_done(db, step, "foliage_def query failed", error);
}
bool read_foliage_states(sqlite3 *db, FoliageData &out, std::string &error) {
  Stmt q(db, kFoliageStateSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "foliage_state", "itrirrr", error))
      return false;
    FoliageStateRow r{col_i64(s, 0), col_str(s, 1),  col_real(s, 2),
                      col_i64(s, 3), col_real(s, 4), col_real(s, 5),
                      col_real(s, 6)};
    if (!(r.lifecycle_duration >= 0) || !(r.collision_radius >= 0))
      return fail_msg("foliage_state " + r.name +
                          " has a negative lifecycle duration or collision "
                          "radius",
                      error);
    out.states[r.guid] = std::move(r);
  }
  return check_done(db, step, "foliage_state query failed", error);
}
bool read_gather_interactions(sqlite3 *db, FoliageData &out,
                              std::string &error) {
  Stmt q(db, kGatherInteractionSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "gather_interaction", "itiiiiiriiiirri", error))
      return false;
    const int64_t target = col_i64(s, 2), tier = col_i64(s, 5),
                  slot = col_i64(s, 6);
    if (!foliage_state_ok(target) || tier < 0 || tier > 4 || slot < -1 ||
        slot > 255)
      return fail_msg("gather_interaction " + col_str(s, 1) +
                          " has target state " + std::to_string(target) +
                          ", certification tier " + std::to_string(tier) +
                          " and tool slot " + std::to_string(slot),
                      error);
    GatherInteractionRow r{
        col_i64(s, 0),   col_str(s, 1),   col_int(s, 2),      col_i64(s, 3),
        col_i64(s, 4),   col_int(s, 5),   col_int(s, 6),      col_real(s, 7),
        col_i64(s, 8),   col_i64(s, 9),   col_i64(s, 10),     col_i64(s, 11),
        col_real(s, 12), col_real(s, 13), col_i64(s, 14) != 0};
    if (!(r.base_duration >= 0) || !(r.interact_radius >= 0) ||
        !(r.interact_tolerance >= 0))
      return fail_msg("gather_interaction " + r.name +
                          " has a negative or no-number duration or range",
                      error);
    out.interactions[r.guid] = std::move(r);
  }
  return check_done(db, step, "gather_interaction query failed", error);
}
bool read_gather_interaction_defs(sqlite3 *db, FoliageData &out,
                                  std::string &error) {
  Stmt q(db, kGatherInteractionDefSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "gather_interaction_def", "itirrri", error))
      return false;
    GatherInteractionDefRow r{col_i64(s, 0),     col_str(s, 1),  col_i64(s, 2),
                              col_real(s, 3),    col_real(s, 4), col_real(s, 5),
                              col_i64(s, 6) != 0};
    out.interaction_defs[r.guid] = std::move(r);
  }
  return check_done(db, step, "gather_interaction_def query failed", error);
}
bool read_gather_defs(sqlite3 *db, GatherData &out, std::string &error) {
  Stmt q(db, kGatherDefSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "gather_def", "itrrirriiiiiriii", error))
      return false;
    const int64_t slot = col_i64(s, 9), tier = col_i64(s, 11);
    if (slot < 0 || slot > 255 || tier < 0 || tier > 4)
      return fail_msg("gather_def " + col_str(s, 1) + " has tool slot " +
                          std::to_string(slot) + " and certification tier " +
                          std::to_string(tier),
                      error);
    GatherDefRow r{
        col_i64(s, 0),   col_str(s, 1),  col_real(s, 2), col_real(s, 3),
        col_i64(s, 4),   col_real(s, 5), col_real(s, 6), col_i64(s, 7) != 0,
        col_i64(s, 8),   col_int(s, 9),  col_i64(s, 10), col_int(s, 11),
        col_real(s, 12), col_i64(s, 13), col_i64(s, 14), col_i64(s, 15)};
    if (!(r.interact_radius >= 0) || !(r.interact_tolerance >= 0) ||
        !(r.base_duration >= 0) || !(r.empty_duration >= 0) ||
        !(r.override_respawn >= 0))
      return fail_msg("gather_def " + r.name +
                          " has a negative or no-number range, duration or "
                          "respawn",
                      error);
    out.defs[r.guid] = std::move(r);
  }
  return check_done(db, step, "gather_def query failed", error);
}
bool read_gather_tools(sqlite3 *db, GatherData &out, std::string &error) {
  Stmt q(db, kGatherToolSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "gather_tool", "itiiiiii", error))
      return false;
    const int64_t tier = col_i64(s, 4), slot = col_i64(s, 5);
    if (tier < 0 || tier > 4 || slot < 0 || slot > 255)
      return fail_msg("gather_tool " + col_str(s, 1) +
                          " has certification tier " + std::to_string(tier) +
                          " and slot " + std::to_string(slot),
                      error);
    GatherToolRow r{col_i64(s, 0), col_str(s, 1), col_i64(s, 2), col_i64(s, 3),
                    col_int(s, 4), col_int(s, 5), col_i64(s, 6), col_i64(s, 7)};
    out.tools[r.item_guid] = std::move(r);
  }
  return check_done(db, step, "gather_tool query failed", error);
}
bool read_item_variations(sqlite3 *db, GatherData &out, std::string &error) {
  Stmt q(db, kItemVariationSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "item_variation", "iii", error))
      return false;
    const int64_t guid = col_i64(s, 0);
    out.quality_expr[guid] = col_i64(s, 1);
    out.durability_expr[guid] = col_i64(s, 2);
  }
  return check_done(db, step, "item_variation query failed", error);
}

std::optional<int32_t> parse_variation_quality_text(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.pop_back();
  size_t from = 0;
  while (from < text.size() &&
         std::isspace(static_cast<unsigned char>(text[from])))
    ++from;
  text.erase(0, from);
  if (text.empty() || text.size() > 3 ||
      !std::all_of(text.begin(), text.end(),
                   [](unsigned char c) { return std::isdigit(c) != 0; }))
    return std::nullopt;
  const int value = std::atoi(text.c_str());
  if (value < items::kMinQuality || value > items::kMaxQuality)
    return std::nullopt;
  return static_cast<int32_t>(value);
}

} // namespace

std::optional<stats::SetBonus> ContentDb::set_bonus(int64_t guid,
                                                    std::string &error) const {
  error.clear();
  Stmt q(db_, kSetBonusSql, guid);
  stats::SetBonus out;
  int64_t tier_ord = -1;
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!read_set_bonus_row(q.get(), guid, tier_ord, out, error))
      return std::nullopt;
  }
  if (!check_done(db_, step, "set bonus query failed", error) ||
      out.tiers.empty())
    return std::nullopt;
  return out;
}
std::optional<std::map<int, std::string>>
ContentDb::equip_slot_names(std::string &error) const {
  Stmt q(db_, kEquipSlotSql);
  std::map<int, std::string> out;
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW)
    out[sqlite3_column_int(q.get(), 0)] = column_text(q.get(), 1);
  if (!check_done(db_, step, "equip slot query failed", error))
    return std::nullopt;
  return out;
}
std::optional<ItemUse> ContentDb::item_use(int64_t item_guid,
                                           std::string &error) const {
  error.clear();
  ItemUse u;
  {
    Stmt q(db_, kItemUseSql, item_guid);
    const int step = q.step();
    if (step == SQLITE_DONE)
      return std::nullopt;
    if (step != SQLITE_ROW) {
      fail_db(db_, "item use query failed", error);
      return std::nullopt;
    }
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "item_use " + std::to_string(item_guid), "?tirii",
                    error))
      return std::nullopt;
    u = {col_i64(s, 0),      col_str(s, 1), col_i64(s, 2) != 0,
         col_real(s, 3),     col_i64(s, 4) != 0, col_i64(s, 5)};
  }
  if (!guid_list(db_, {kItemActionSql, "item action"}, item_guid, u.actions,
                 error))
    return std::nullopt;
  return u;
}
bool ContentDb::item_has_tag(int64_t item_guid, const std::string &tag,
                             std::string &error) const {
  error.clear();
  Stmt q(db_, kItemHasTagSql, item_guid);
  if (!q.ok())
    return fail_db(db_, "item tag query failed", error);
  sqlite3_bind_text(q.get(), 2, tag.c_str(), -1, SQLITE_TRANSIENT);
  const int step = q.step();
  if (step == SQLITE_ROW)
    return true;
  if (step != SQLITE_DONE)
    fail_db(db_, "item tag query failed", error);
  return false;
}
int64_t ContentDb::consumable_ability(int64_t item_guid,
                                      std::string &error) const {
  const auto use = item_use(item_guid, error);
  if (!use || use->activation != "CastAbility" || !use->consume_on_use ||
      use->cast_ability == 0)
    return 0;
  for (const char *tag : {"Item.Consumable.Scroll", "Item.Consumable.Elixir"}) {
    if (item_has_tag(item_guid, tag, error))
      return use->cast_ability;
    if (!error.empty())
      return 0;
  }
  return 0;
}
std::optional<ActionResponse>
ContentDb::action_response(int64_t guid, std::string &error) const {
  error.clear();
  ActionResponse a;
  {
    Stmt q(db_, kActionSql, guid);
    const int step = q.step();
    if (step == SQLITE_DONE)
      return std::nullopt;
    if (step != SQLITE_ROW) {
      fail_db(db_, "action response query failed", error);
      return std::nullopt;
    }
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "action_response " + std::to_string(guid), "?tti?i",
                    error))
      return std::nullopt;
    a = {col_i64(s, 0), col_str(s, 1), col_str(s, 2),
         col_i64(s, 3), col_str(s, 4), col_i64(s, 5) != 0};
  }
  if (!guid_list(db_, {kActionRewardSql, "action reward"}, guid,
                 a.reward_tables, error))
    return std::nullopt;
  return a;
}
std::optional<RewardTable> ContentDb::reward_table(int64_t guid,
                                                   std::string &error) const {
  error.clear();
  RewardTable t;
  if (read_reward_table_header(db_, guid, t, error) <= 0 ||
      !read_reward_items(db_, guid, t, error) ||
      !read_reward_xp(db_, guid, t, error))
    return std::nullopt;
  return t;
}
std::optional<int32_t> ContentDb::variation_quality(int64_t variation,
                                                    std::string &error) const {
  error.clear();
  Stmt q(db_, kVariationQualitySql, variation);
  if (variation == 0 || !q.ok())
    return std::nullopt;
  const int step = q.step();
  if (step == SQLITE_DONE)
    return std::nullopt;
  if (step != SQLITE_ROW) {
    fail_db(db_, "item_variation query failed", error);
    return std::nullopt;
  }
  return parse_variation_quality_text(column_text(q.get(), 0));
}
std::optional<LevelingType>
ContentDb::leveling_type_named(const std::string &name,
                               std::string &error) const {
  LevelingType t;
  if (!read_leveling_type_header(db_, name, t, error) ||
      !read_leveling_curve(db_, name, t, error))
    return std::nullopt;
  return t;
}
std::vector<std::string>
ContentDb::leveling_type_names(const std::string &experience_type,
                               const std::string &prefix,
                               std::string &error) const {
  error.clear();
  std::vector<std::string> out;
  Stmt q(db_, kLevelingNamesSql);
  if (q.ok()) {
    sqlite3_bind_text(q.get(), 1, experience_type.c_str(),
                      static_cast<int>(experience_type.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(q.get(), 2, prefix.c_str(),
                      static_cast<int>(prefix.size()), SQLITE_TRANSIENT);
  }
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!column_is_text(q.get(), 0)) {
      fail_msg("leveling_type has a name that is not text", error);
      return {};
    }
    out.push_back(column_text(q.get(), 0));
  }
  if (!check_done(db_, step, "leveling type query failed", error))
    return {};
  return out;
}
std::optional<CurveRow> ContentDb::stat_curve(int64_t guid,
                                              std::string &error) const {
  error.clear();
  Stmt q(db_, kStatCurveSql, guid);
  CurveRow out{guid, {}, {}};
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "stat_curve " + std::to_string(guid), "trrt", error))
      return std::nullopt;
    out.name = col_str(s, 0);
    out.keys.push_back({col_real(s, 1), col_real(s, 2), col_str(s, 3)});
  }
  if (!check_done(db_, step, "stat curve query failed", error) ||
      out.keys.empty())
    return std::nullopt;
  return out;
}
std::optional<FoliageData> ContentDb::foliage_data(std::string &error) const {
  error.clear();
  const int found = count_present_tables(db_, kFoliageTableSql, error);
  if (found <= 0)
    return std::nullopt;
  if (found != 4) {
    fail_msg("has some of the foliage tables (foliage_def, foliage_state, "
             "gather_interaction, gather_interaction_def) and not all: rebuild "
             "it with this checkout's contentc",
             error);
    return std::nullopt;
  }
  FoliageData out;
  if (!read_foliage_defs(db_, out, error) ||
      !read_foliage_states(db_, out, error) ||
      !read_gather_interactions(db_, out, error) ||
      !read_gather_interaction_defs(db_, out, error))
    return std::nullopt;
  if (out.defs.empty() || out.states.empty() || out.interactions.empty()) {
    fail_msg("foliage tables: a table is empty", error);
    return std::nullopt;
  }
  return out;
}
std::optional<GatherData> ContentDb::gather_data(std::string &error) const {
  error.clear();
  const int found = count_present_tables(db_, kGatherTableSql, error);
  if (found <= 0)
    return std::nullopt;
  if (found != 3) {
    fail_msg("has some of the gathering tables (gather_def, gather_tool, "
             "item_variation) and not all: rebuild it with this checkout's "
             "contentc",
             error);
    return std::nullopt;
  }
  GatherData out;
  if (!read_gather_defs(db_, out, error) ||
      !read_gather_tools(db_, out, error) ||
      !read_item_variations(db_, out, error))
    return std::nullopt;
  return out;
}

} // namespace aoc::worldd::content
