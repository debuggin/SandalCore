// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "content/content_db_internal.h"

#include <tuple>

namespace aoc::worldd::content {
namespace {

constexpr const char *kNpcAssetSql =
    "SELECT guid, name, asset_ref, archetype_path, faction_guid, aggro_radius, "
    "leash_radius, override_aggro, override_leash, scale_min, scale_max, "
    "dialogue_root_guid, interact_component_guid, interaction_def_guid, "
    "appearance_guid, dev_only, filter, tags FROM npc_asset ORDER BY guid";
constexpr const char *kNpcAssetLootSql =
    "SELECT asset_guid, table_guid FROM npc_asset_loot ORDER BY asset_guid, "
    "ord";
constexpr const char *kNpcSetMemberAssetSql =
    "SELECT set_guid, asset_guid, weight FROM npc_asset_set_member ORDER BY "
    "set_guid, ord";
constexpr const char *kNpcAssetSetSql = "SELECT guid, tags FROM npc_asset_set";
constexpr const char *kNpcInstanceSql =
    "SELECT guid, name, level_min, level_max, dynamic_leveling, respawn_time, "
    "respawn_variance, respawn_rule, patrol_route_guid, ambient_behavior, "
    "min_radius, max_radius, arrival_distance, wander_speed_mult, "
    "wander_delay, wander_delay_deviation, speed_expr, snapping_rule, "
    "orientation_type, aggro_radius, override_aggro, area_weight, vendor_guid, "
    "interact_component_guid, interact_response_guid, dialogue_guid, group_id, "
    "spawn_location_type, activation_range, blacklisted, num_waypoints, "
    "max_angle, follow_distance, formation, idle_anim, align_to_slope, "
    "dev_only, filter_override_guid, spawner_priority, tags "
    "FROM npc_instance ORDER BY guid";
constexpr const char *kNpcInstanceSetSql =
    "SELECT instance_guid, set_guid FROM npc_instance_asset_set ORDER BY "
    "instance_guid, ord";
constexpr const char *kNpcInstanceLootSql =
    "SELECT instance_guid, table_guid FROM npc_instance_loot ORDER BY "
    "instance_guid, ord";
constexpr const char *kNpcSetSql =
    "SELECT guid, name, area_guid, activation_range, blacklisted, dev_only, "
    "filter_override_guid, tags FROM npc_set ORDER BY guid";
constexpr const char *kNpcWorldSpawnSql =
    "SELECT guid, name, culture, map_name, dev_only, filter_override_guid FROM "
    "npc_world_spawn ORDER BY guid";
constexpr const char *kNpcFilterSql =
    "SELECT guid, filter FROM npc_filter ORDER BY guid";
constexpr const char *kNpcGateSql =
    "SELECT set_guid, source FROM npc_gate ORDER BY set_guid, source";
constexpr const char *kNpcSetMemberSql =
    "SELECT set_guid, key, instance_guid, x, y, z, yaw, random_rotation FROM "
    "npc_set_member ORDER BY set_guid, key";
constexpr const char *kNpcPlacementSql =
    "SELECT spawn_guid, key, set_guid, x, y, z, yaw, min_level, max_level FROM "
    "npc_placement ORDER BY spawn_guid, key";
constexpr const char *kNpcAreaSql =
    "SELECT guid, name, min_spawns, max_spawns FROM npc_area ORDER BY guid";
constexpr const char *kNpcRouteSql =
    "SELECT guid, name, looping, start_closest, relative_city_node, "
    "relative_encounter, tx, ty, tz, qx, qy, qz, qw, sx, sy, sz, points, "
    "waypoints, pauses, pathing_set FROM npc_route ORDER BY guid";
constexpr const char *kNpcAreaCircleSql =
    "SELECT area_guid, x, y, z, extent, weight, max_spawns FROM "
    "npc_area_circle ORDER BY area_guid, ord";

bool parse_number_array(const std::string &text, std::vector<double> &out) {
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_array())
    return false;
  for (const auto &v : j) {
    if (!v.is_number())
      return false;
    out.push_back(v.get<double>());
  }
  return true;
}

bool parse_vec3_array(const std::string &text,
                      std::vector<std::array<double, 3>> &out) {
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_array())
    return false;
  for (const auto &v : j) {
    if (!v.is_array() || v.size() != 3 || !v[0].is_number() ||
        !v[1].is_number() || !v[2].is_number())
      return false;
    out.push_back({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
  }
  return true;
}

bool parse_string_array(const std::string &text,
                        std::vector<std::string> &out) {
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_array())
    return false;
  for (const auto &v : j) {
    if (!v.is_string())
      return false;
    out.push_back(v.get<std::string>());
  }
  return true;
}

bool json_string_vec(const nlohmann::json &j, const char *key,
                     std::vector<std::string> &v) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array())
    return false;
  for (const auto &e : *it) {
    if (!e.is_string())
      return false;
    v.push_back(e.get<std::string>());
  }
  return true;
}

bool json_int_vec(const nlohmann::json &j, const char *key,
                  std::vector<int64_t> &v) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_array())
    return false;
  for (const auto &e : *it) {
    if (!e.is_number_integer())
      return false;
    v.push_back(e.get<int64_t>());
  }
  return true;
}

bool json_double_field(const nlohmann::json &j, const char *key, double &v) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number())
    return false;
  v = it->get<double>();
  return true;
}

bool json_string_field(const nlohmann::json &j, const char *key,
                       std::string &v) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_string())
    return false;
  v = it->get<std::string>();
  return true;
}

bool parse_pop_filter(const std::string &text, PopFilter &out) {
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object())
    return false;
  return json_string_vec(j, "biomes", out.biomes) &&
         json_string_vec(j, "cultures", out.cultures) &&
         json_int_vec(j, "node_ids", out.node_ids) &&
         json_string_field(j, "node_level_min", out.node_level_min) &&
         json_string_field(j, "node_level_max", out.node_level_max) &&
         json_string_vec(j, "node_types", out.node_types) &&
         json_string_vec(j, "required_tags", out.required_tags) &&
         json_string_vec(j, "seasons", out.seasons) &&
         json_double_field(j, "time_of_day_start", out.time_of_day_start) &&
         json_double_field(j, "time_of_day_end", out.time_of_day_end) &&
         json_string_vec(j, "weather", out.weather) &&
         json_int_vec(j, "zones_of_influence", out.zones);
}

bool npc_bad(const char *table, std::string &error) {
  return fail_msg(
      std::string(table) + " has a column of the wrong storage class", error);
}

bool npc_done(sqlite3 *db, int step, const char *what, std::string &error) {
  return step == SQLITE_DONE
             ? true
             : fail_msg(std::string(what) + " query failed: " + db_message(db) +
                            " (the NPC population tables are contentc version "
                            "9's: rebuild the file with this checkout's "
                            "contentc)",
                        error);
}

bool read_npc_assets(sqlite3 *db, PopulationData &out, std::string &error) {
  Stmt q(db, kNpcAssetSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "npc_asset", "i...irriirriiiii", error))
      return false;
    NpcAssetRow a{col_i64(s, 0),     column_text(s, 1),  column_text(s, 2),
                  column_text(s, 3), col_i64(s, 4),      col_real(s, 5),
                  col_real(s, 6),    col_i64(s, 7) != 0, col_i64(s, 8) != 0,
                  col_real(s, 9),    col_real(s, 10),    col_i64(s, 11),
                  col_i64(s, 12),    col_i64(s, 13),     col_i64(s, 14)};
    a.dev_only = col_i64(s, 15) != 0;
    if (!parse_pop_filter(column_text(s, 16), a.filter) ||
        !parse_string_array(column_text(s, 17), a.tags))
      return npc_bad("npc_asset", error);
    out.assets[a.guid] = std::move(a);
  }
  return npc_done(db, step, "npc_asset", error);
}

bool read_npc_asset_loot(sqlite3 *db, PopulationData &out, std::string &error) {
  Stmt q(db, kNpcAssetLootSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!check_cols(q.get(), "npc_asset_loot", "ii", error))
      return false;
    if (const auto it = out.assets.find(col_i64(q.get(), 0));
        it != out.assets.end())
      it->second.loot_tables.push_back(col_i64(q.get(), 1));
  }
  return npc_done(db, step, "npc_asset_loot", error);
}

bool parse_npc_asset_set_row(sqlite3_stmt *s, PopulationData &out,
                             std::string &error) {
  if (!column_is_int(s, 0))
    return npc_bad("npc_asset_set", error);
  const int64_t guid = col_i64(s, 0);
  out.asset_sets[guid];
  std::vector<std::string> tags;
  if (!parse_string_array(column_text(s, 1), tags))
    return npc_bad("npc_asset_set", error);
  if (!tags.empty())
    out.asset_set_tags[guid] = std::move(tags);
  return true;
}

bool read_npc_asset_sets(sqlite3 *db, PopulationData &out, std::string &error) {
  {
    Stmt q(db, kNpcAssetSetSql);
    int step = SQLITE_ERROR;
    while ((step = q.step()) == SQLITE_ROW) {
      if (!parse_npc_asset_set_row(q.get(), out, error))
        return false;
    }
    if (!npc_done(db, step, "npc_asset_set", error))
      return false;
  }
  Stmt q(db, kNpcSetMemberAssetSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!check_cols(q.get(), "npc_asset_set_member", "iir", error))
      return false;
    out.asset_sets[col_i64(q.get(), 0)].push_back(
        {col_i64(q.get(), 1), col_real(q.get(), 2)});
  }
  return npc_done(db, step, "npc_asset_set_member", error);
}

bool read_npc_instance_row(sqlite3_stmt *s, PopulationData &out,
                           std::string &error) {
  if (!check_cols(s, "npc_instance", "i.iiirr.i.rrrrrr...ririiiii.riirr..iii",
                  error))
    return false;
  NpcInstanceRow r{col_i64(s, 0),       column_text(s, 1),
                   col_int(s, 2),       col_int(s, 3),
                   col_i64(s, 4) != 0,  col_real(s, 5),
                   col_real(s, 6),      column_text(s, 7),
                   col_i64(s, 8),       col_i64(s, 22),
                   col_i64(s, 23),      col_i64(s, 24),
                   col_i64(s, 25),      0,
                   column_text(s, 9),   col_real(s, 10),
                   col_real(s, 11),     col_real(s, 12),
                   col_real(s, 13),     col_real(s, 14),
                   col_real(s, 15),     column_text(s, 16),
                   column_text(s, 17),  column_text(s, 18),
                   column_text(s, 27),  col_real(s, 19),
                   col_real(s, 21),     col_real(s, 28),
                   col_i64(s, 20) != 0, col_i64(s, 29) != 0,
                   col_i64(s, 26),      col_int(s, 30),
                   col_real(s, 31),     col_real(s, 32),
                   column_text(s, 33),  column_text(s, 34),
                   col_i64(s, 35) != 0, col_i64(s, 36) != 0,
                   col_i64(s, 37),      column_text(s, 38)};
  if (!parse_string_array(column_text(s, 39), r.tags))
    return npc_bad("npc_instance", error);
  out.instances[r.guid] = std::move(r);
  return true;
}

bool read_npc_instances(sqlite3 *db, PopulationData &out, std::string &error) {
  Stmt q(db, kNpcInstanceSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!read_npc_instance_row(q.get(), out, error))
      return false;
  }
  return npc_done(db, step, "npc_instance", error);
}

void append_npc_instance_link(NpcInstanceRow &row, int64_t target,
                              bool is_loot) {
  if (is_loot)
    row.loot_tables.push_back(target);
  else
    row.asset_sets.push_back(target);
}

bool read_npc_instance_link_table(sqlite3 *db, const char *sql, bool is_loot,
                                  PopulationData &out, std::string &error) {
  const char *table = is_loot ? "npc_instance_loot" : "npc_instance_asset_set";
  Stmt q(db, sql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!check_cols(q.get(), table, "ii", error))
      return false;
    if (const auto it = out.instances.find(col_i64(q.get(), 0));
        it != out.instances.end())
      append_npc_instance_link(it->second, col_i64(q.get(), 1), is_loot);
  }
  return npc_done(db, step, table, error);
}

bool read_npc_instance_links(sqlite3 *db, PopulationData &out,
                             std::string &error) {
  return read_npc_instance_link_table(db, kNpcInstanceSetSql, false, out,
                                      error) &&
         read_npc_instance_link_table(db, kNpcInstanceLootSql, true, out,
                                      error);
}

bool read_npc_instance_dialogues(sqlite3 *db, PopulationData &out,
                                 std::string &error) {
  Stmt q(db, "SELECT source_guid, root_guid FROM npc_dialogue WHERE "
             "source_kind = 'instance'");
  if (!q.ok())
    return true;
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!check_cols(q.get(), "npc_dialogue", "ii", error))
      return false;
    if (const auto it = out.instances.find(col_i64(q.get(), 0));
        it != out.instances.end())
      it->second.dialogue_root = col_i64(q.get(), 1);
  }
  return npc_done(db, step, "npc_dialogue", error);
}

bool read_npc_sets(sqlite3 *db, PopulationData &out, std::string &error) {
  Stmt q(db, kNpcSetSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "npc_set", "i.iriii", error))
      return false;
    NpcSetRow r{col_i64(s, 0),  column_text(s, 1),  col_i64(s, 2),
                col_real(s, 3), col_i64(s, 4) != 0, col_i64(s, 5) != 0,
                col_i64(s, 6)};
    if (!parse_string_array(column_text(s, 7), r.tags))
      return npc_bad("npc_set", error);
    out.sets[r.guid] = std::move(r);
  }
  return npc_done(db, step, "npc_set", error);
}

bool read_npc_world_spawns(sqlite3 *db, PopulationData &out,
                           std::string &error) {
  Stmt q(db, kNpcWorldSpawnSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "npc_world_spawn", "i...ii", error))
      return false;
    out.world_spawns[col_i64(s, 0)] = {col_i64(s, 0),      column_text(s, 1),
                                       column_text(s, 2),  column_text(s, 3),
                                       col_i64(s, 4) != 0, col_i64(s, 5)};
  }
  return npc_done(db, step, "npc_world_spawn", error);
}

bool read_npc_filters_and_gates(sqlite3 *db, PopulationData &out,
                                std::string &error) {
  {
    Stmt q(db, kNpcFilterSql);
    int step = SQLITE_ERROR;
    while ((step = q.step()) == SQLITE_ROW) {
      if (!column_is_int(q.get(), 0))
        return npc_bad("npc_filter", error);
      PopFilter f;
      if (!parse_pop_filter(column_text(q.get(), 1), f))
        return npc_bad("npc_filter", error);
      out.filters[col_i64(q.get(), 0)] = std::move(f);
    }
    if (!npc_done(db, step, "npc_filter", error))
      return false;
  }
  Stmt q(db, kNpcGateSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    if (!column_is_int(q.get(), 0))
      return npc_bad("npc_gate", error);
    out.gates[col_i64(q.get(), 0)].push_back(column_text(q.get(), 1));
  }
  return npc_done(db, step, "npc_gate", error);
}

bool read_npc_set_members(sqlite3 *db, PopulationData &out,
                          std::string &error) {
  Stmt q(db, kNpcSetMemberSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "npc_set_member", "iiirrrri", error))
      return false;
    if (const auto it = out.sets.find(col_i64(s, 0)); it != out.sets.end())
      it->second.members.push_back(
          {col_i64(s, 1), col_i64(s, 2), col_real(s, 3), col_real(s, 4),
           col_real(s, 5), col_real(s, 6), col_i64(s, 7) != 0});
  }
  return npc_done(db, step, "npc_set_member", error);
}

bool read_npc_placements(sqlite3 *db, PopulationData &out, std::string &error) {
  Stmt q(db, kNpcPlacementSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "npc_placement", "iiirrrrii", error))
      return false;
    out.placements.push_back({col_i64(s, 0), col_i64(s, 1), col_i64(s, 2),
                              col_real(s, 3), col_real(s, 4), col_real(s, 5),
                              col_real(s, 6), col_int(s, 7), col_int(s, 8)});
  }
  return npc_done(db, step, "npc_placement", error);
}

bool read_npc_routes(sqlite3 *db, PopulationData &out, std::string &error) {
  Stmt q(db, kNpcRouteSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "npc_route", "i.iiiirrrrrrrrrr", error))
      return false;
    NpcRouteRow r{col_i64(s, 0),      column_text(s, 1),  col_i64(s, 2) != 0,
                  col_i64(s, 3) != 0, col_i64(s, 4) != 0, col_i64(s, 5) != 0,
                  col_real(s, 6),     col_real(s, 7),     col_real(s, 8),
                  col_real(s, 9),     col_real(s, 10),    col_real(s, 11),
                  col_real(s, 12),    col_real(s, 13),    col_real(s, 14),
                  col_real(s, 15)};
    if (!parse_vec3_array(column_text(s, 16), r.points) ||
        !parse_vec3_array(column_text(s, 17), r.waypoints) ||
        !parse_number_array(column_text(s, 18), r.pauses))
      return npc_bad("npc_route", error);
    r.pathing_set = column_text(s, 19);
    out.routes[r.guid] = std::move(r);
  }
  return npc_done(db, step, "npc_route", error);
}

bool read_npc_areas(sqlite3 *db, PopulationData &out, std::string &error) {
  {
    Stmt q(db, kNpcAreaSql);
    int step = SQLITE_ERROR;
    while ((step = q.step()) == SQLITE_ROW) {
      sqlite3_stmt *s = q.get();
      if (!check_cols(s, "npc_area", "i.ii", error))
        return false;
      out.areas[col_i64(s, 0)] = {col_i64(s, 0), column_text(s, 1),
                                  col_int(s, 2), col_int(s, 3)};
    }
    if (!npc_done(db, step, "npc_area", error))
      return false;
  }
  Stmt q(db, kNpcAreaCircleSql);
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    if (!check_cols(s, "npc_area_circle", "irrrrri", error))
      return false;
    if (const auto it = out.areas.find(col_i64(s, 0)); it != out.areas.end())
      it->second.circles.push_back({col_real(s, 1), col_real(s, 2),
                                    col_real(s, 3), col_real(s, 4),
                                    col_real(s, 5), col_i64(s, 6)});
  }
  return npc_done(db, step, "npc_area_circle", error);
}

bool combat_done(sqlite3 *db, int step, const char *what, std::string &error) {
  return step == SQLITE_DONE
             ? true
             : fail_msg(std::string(what) + " query failed: " + db_message(db) +
                            " (the combat tables are contentc version 9's: "
                            "rebuild the file with this checkout's contentc)",
                        error);
}

bool read_combat_profile_and_elements(sqlite3 *db, CombatRows &out,
                                      std::string &error) {
  {
    Stmt q(db, "SELECT key, stat_guid FROM combat_profile");
    int step = SQLITE_ERROR;
    while ((step = q.step()) == SQLITE_ROW)
      out.profile[column_text(q.get(), 0)] = col_i64(q.get(), 1);
    if (!combat_done(db, step, "combat_profile", error))
      return false;
  }
  Stmt q(db, "SELECT tag, damage, damage_mult, damage_add_mult, mitigation "
             "FROM combat_element ORDER BY tag");
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW) {
    sqlite3_stmt *s = q.get();
    out.elements.push_back({column_text(s, 0), col_i64(s, 1), col_i64(s, 2),
                            col_i64(s, 3), col_i64(s, 4)});
  }
  return combat_done(db, step, "combat_element", error);
}

bool read_combat_relations(sqlite3 *db, CombatRows &out, std::string &error) {
  {
    Stmt q(db, "SELECT key, value FROM combat_config");
    int step = SQLITE_ERROR;
    while ((step = q.step()) == SQLITE_ROW)
      out.config[column_text(q.get(), 0)] = col_real(q.get(), 1);
    if (!combat_done(db, step, "combat_config", error))
      return false;
  }
  {
    Stmt q(db, "SELECT guid, permissions FROM relationship");
    int step = SQLITE_ERROR;
    while ((step = q.step()) == SQLITE_ROW)
      out.permissions[col_i64(q.get(), 0)] = col_int(q.get(), 1);
    if (!combat_done(db, step, "relationship", error))
      return false;
  }
  Stmt q(db, "SELECT alignment, other, relationship FROM alignment_relation");
  int step = SQLITE_ERROR;
  while ((step = q.step()) == SQLITE_ROW)
    out.relation[col_i64(q.get(), 0)][col_i64(q.get(), 1)] =
        col_i64(q.get(), 2);
  return combat_done(db, step, "alignment_relation", error);
}

} // namespace

std::optional<PopulationData>
ContentDb::population_data(std::string &error) const {
  PopulationData out;
  if (!read_npc_assets(db_, out, error) ||
      !read_npc_asset_loot(db_, out, error) ||
      !read_npc_asset_sets(db_, out, error) ||
      !read_npc_instances(db_, out, error) ||
      !read_npc_instance_links(db_, out, error) ||
      !read_npc_instance_dialogues(db_, out, error) ||
      !read_npc_sets(db_, out, error) ||
      !read_npc_world_spawns(db_, out, error) ||
      !read_npc_filters_and_gates(db_, out, error) ||
      !read_npc_set_members(db_, out, error) ||
      !read_npc_placements(db_, out, error) ||
      !read_npc_routes(db_, out, error) || !read_npc_areas(db_, out, error))
    return std::nullopt;
  return out;
}

std::optional<CombatRows> ContentDb::combat_rows(std::string &error) const {
  CombatRows out;
  if (!read_combat_profile_and_elements(db_, out, error) ||
      !read_combat_relations(db_, out, error))
    return std::nullopt;
  if (out.profile.empty() || out.elements.empty()) {
    fail_msg("has no combat profile (the combat tables are empty)", error);
    return std::nullopt;
  }
  return out;
}

} // namespace aoc::worldd::content
