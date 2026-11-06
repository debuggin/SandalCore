// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

#include "content/content_db_internal.h"

namespace aoc::worldd::content {

namespace {

constexpr const char* kMetaSql = "SELECT key, value FROM meta";
constexpr const char* kClassSql = "SELECT guid, name, archetype_id FROM class WHERE archetype_id = ?1";
constexpr const char* kClassByNameSql = "SELECT guid, name, archetype_id FROM class WHERE name = ?1";
constexpr const char* kAttributesSql =
    "SELECT stat_guid, stat_name, value, rounding_ambiguous FROM class_stat_base WHERE class_guid = ?1 AND level = ?2 ORDER BY stat_guid";
constexpr const char* kAbilityRuntimeSql =
    "SELECT ability_guid, name, held_behavior, requires_out_of_combat, min_stamina, cost_stat_guid, cost_amount, "
    "cost_requires_moving, charge_interval, cooldown, triggers_cooldown, disable_during_cooldown, targets_self, "
    "effect_guid, montage_path, cancel_section FROM ability_runtime WHERE ability_guid = ?1";
constexpr const char* kEffectRuntimeSql =
    "SELECT effect_guid, name, infinite, duration, tick_timer, stops_regen_standing, stops_regen_moving, "
    "end_effect_guid, end_effect_duration FROM effect_runtime";
constexpr const char* kEffectStatSql =
    "SELECT effect_guid, stat_guid, source_stat_guid, factor, addend FROM effect_stat_runtime ORDER BY effect_guid, ord";
constexpr const char* kStaminaRuleSql = "SELECT value FROM stamina_rule WHERE key = ?1";
constexpr const char* kItemSql =
    "SELECT guid, name, can_equip, occupies_all_equip_slots, default_quality, max_stack_size, max_durability, inv_w, "
    "inv_h, resource FROM item WHERE guid = ?1";
constexpr const char* kResourceBagSql = "SELECT def_guid, def_name, columns, rows, custom FROM resource_bag WHERE item_guid = ?1";
constexpr const char* kResourceBagCellSql = "SELECT col, row, value FROM resource_bag_cell WHERE def_guid = ?1 ORDER BY col, row";
constexpr const char* kItemNameSql = "SELECT display_name FROM item WHERE guid = ?1";
constexpr const char* kItemSlotsSql = "SELECT slot FROM item_equip_slot WHERE item_guid = ?1 ORDER BY ord";
constexpr const char* kStatProgramSql = "SELECT version, mode, stats, classes, crc32, data FROM stat_program WHERE id = 1";
constexpr const char* kStatBlockSql =
    "SELECT stat_guid, common_min, common_max, uncommon_min, uncommon_max, rare_min, rare_max, heroic_min, heroic_max, "
    "epic_min, epic_max, legendary_min, legendary_max, artifact_min, artifact_max FROM stat_block_stat WHERE block_guid = ?1 "
    "ORDER BY stat_guid";
constexpr const char* kItemSetSql = "SELECT set_guid FROM item_set WHERE item_guid = ?1 ORDER BY ord";
constexpr const char* kItemTagSql = "SELECT tag FROM item_tag WHERE item_guid = ?1 ORDER BY ord";
constexpr const char* kSetBonusSql =
    "SELECT tier_ord, pieces, stat_guid, common, uncommon, rare, heroic, epic, legendary, artifact FROM set_bonus "
    "WHERE set_guid = ?1 ORDER BY tier_ord, bonus_ord";
constexpr const char* kEquipSlotSql = "SELECT slot, name FROM equip_slot";
constexpr const char* kItemUseSql =
    "SELECT item_guid, activation, consume_on_use, cooldown, disabled_in_combat, cast_ability_guid FROM item_use WHERE item_guid = ?1";
constexpr const char* kItemActionSql = "SELECT action_guid FROM item_action WHERE item_guid = ?1 ORDER BY ord";
constexpr const char* kActionSql =
    "SELECT guid, name, response_type, class_guid, class_name, remove_up_to_quantity FROM action_response WHERE guid = ?1";
constexpr const char* kActionRewardSql = "SELECT reward_table_guid FROM action_reward WHERE action_guid = ?1 ORDER BY ord";
constexpr const char* kRewardTableSql = "SELECT guid, name, predicate, level_below, modelled FROM reward_table WHERE guid = ?1";
constexpr const char* kRewardItemSql = "SELECT item_guid, variation_guid, quantity FROM reward_item WHERE table_guid = ?1 ORDER BY ord";
constexpr const char* kRewardXpSql =
    "SELECT experience_type, target_level, margin, curve_guid, mult_stat_guid, expr, expression FROM reward_xp WHERE "
    "table_guid = ?1 ORDER BY ord";
constexpr const char* kLevelingTypeSql =
    "SELECT guid, name, experience_type, level_cap, uses_debt, xp_mult_stat_guid FROM leveling_type WHERE name = ?1";
constexpr const char* kLevelingCurveSql = "SELECT level, xp FROM leveling_curve WHERE type_guid = ?1 ORDER BY level";
constexpr const char* kAutoGrantSql =
    "SELECT tree_guid, tree_name, leveling_type_guid, node_guid, node_name, level_requirement, skill_guid, skill_name, "
    "rank_guid, effect_guid FROM skill_auto_grant WHERE leveling_type_guid = ?1 ORDER BY level_requirement, node_guid";
constexpr const char* kRankModSql = "SELECT stat_guid, stat_name, value FROM skill_rank_stat_mod WHERE rank_guid = ?1 ORDER BY ord";
constexpr const char* kTreeSql = "SELECT guid, name, kind, leveling_type_guid, class_guid, weapon_line_guid FROM skill_tree ORDER BY guid";
constexpr const char* kNodeSql =
    "SELECT tree_guid, node_guid, ord, name, skill_guid, skill_name, level_requirement, point_requirement, "
    "auto_granted, auto_hotbar, all_required, opt, slot_guid FROM skill_node ORDER BY tree_guid, ord, opt";
constexpr const char* kRequireSql =
    "SELECT tree_guid, node_guid, ord, skill_guid, slot_node_guid FROM skill_node_require ORDER BY tree_guid, node_guid, ord";
constexpr const char* kExclusiveSql =
    "SELECT tree_guid, node_guid, ord, skill_guid FROM skill_node_exclusive ORDER BY tree_guid, node_guid, ord";
constexpr const char* kSkillRankSql =
    "SELECT skill_guid, ord, rank_guid, name, ability_guid, ability_name, effect_guid, effect_name, effect_infinite, "
    "effect_duration FROM skill_rank ORDER BY skill_guid, ord";
constexpr const char* kRankCostSql = "SELECT rank_guid, ord, point_type_guid, quantity FROM skill_rank_cost ORDER BY rank_guid, ord";
constexpr const char* kAllRankModSql = "SELECT rank_guid, stat_guid, stat_name, value FROM skill_rank_stat_mod ORDER BY rank_guid, ord";
constexpr const char* kPointTypeSql = "SELECT guid, name FROM skill_point_type";
constexpr const char* kLevelingPointSql = "SELECT type_guid, level, point_type_guid, quantity FROM leveling_point";
constexpr const char* kItemWeaponSql = "SELECT attack_ability_guid, skill_line_guid, skill_line_name FROM item_weapon WHERE item_guid = ?1";
constexpr const char* kRarityBoundsSql = "SELECT rarity_min, rarity_max FROM item_vendor WHERE item_guid = ?1";
constexpr const char* kStatRowSql =
    "SELECT common_min, common_max, uncommon_min, uncommon_max, rare_min, rare_max, heroic_min, heroic_max, epic_min, "
    "epic_max, legendary_min, legendary_max, artifact_min, artifact_max FROM stat_block_stat WHERE block_guid = ?1 AND stat_guid = ?2";
constexpr const char* kItemGearSql = "SELECT stat_block_guid, quality_curve_guid FROM item WHERE guid = ?1";
constexpr const char* kQualityCurveSql = "SELECT ord, x, y FROM quality_curve WHERE guid = ?1 ORDER BY ord";

bool read_meta_rows(sqlite3* db, const std::string& path, std::map<std::string, std::string>& meta, std::string& error) {
    Stmt q(db, kMetaSql);
    if (!q.ok()) {
        error = path + " is not a content.db: " + db_message(db);
        return false;
    }
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) meta[column_text(q.get(), 0)] = column_text(q.get(), 1);
    if (step != SQLITE_DONE) {
        error = path + " cannot be read: " + db_message(db);
        return false;
    }
    return true;
}

const std::string* find_meta_value(const std::map<std::string, std::string>& meta, const char* key) {
    const auto it = meta.find(key);
    return it == meta.end() ? nullptr : &it->second;
}

bool validate_meta_version(const std::string& path, const std::map<std::string, std::string>& meta, ContentMeta& out, std::string& error) {
    const std::string* version = find_meta_value(meta, "schema_version");
    if (!version || *version != std::to_string(kSchemaVersion)) {
        error = path + " has schema_version " + (version ? "'" + *version + "'" : std::string("(none)")) + ", this world reads " +
                std::to_string(kSchemaVersion);
        return false;
    }
    const std::string* contentc = find_meta_value(meta, "contentc.version");
    const std::string* sha = find_meta_value(meta, "source.dbc.sha256");
    if (!contentc || !sha) {
        error = path + " has no contentc.version or source.dbc.sha256 in meta: it was not written by contentc";
        return false;
    }
    char* end = nullptr;
    const long v = std::strtol(contentc->c_str(), &end, 10);
    if (contentc->empty() || end != contentc->c_str() + contentc->size()) {
        error = path + " has contentc.version '" + *contentc + "', which is no number: it was not written by contentc";
        return false;
    }
    if (v < kMinContentcVersion) {
        error = path + " was built by contentc version " + *contentc + ", this world reads version " +
                std::to_string(kMinContentcVersion) +
                " or later (the city nodes, contentc version 12): rebuild it with this checkout's contentc: cd services && go run ./cmd/contentc";
        return false;
    }
    out.schema_version = kSchemaVersion;
    out.contentc_version = *contentc;
    out.source_dbc_sha256 = *sha;
    if (const std::string* built = find_meta_value(meta, "built_at")) out.built_at = *built;
    return true;
}

bool validate_required_tables(sqlite3* db, const std::string& path, std::string& error) {
    const char* const sqls[] = {
        kClassSql,       kClassByNameSql,   kAttributesSql,  kAbilityRuntimeSql, kEffectRuntimeSql,   kEffectStatSql, kStaminaRuleSql,
        kItemSql,        kItemSlotsSql,     kStatProgramSql, kItemGearSql,       kQualityCurveSql,    kStatBlockSql,  kItemSetSql,
        kItemTagSql,     kSetBonusSql,      kEquipSlotSql,   kItemUseSql,        kItemActionSql,      kActionSql,     kActionRewardSql,
        kRewardTableSql, kRewardItemSql,    kRewardXpSql,    kLevelingTypeSql,   kLevelingCurveSql,   kAutoGrantSql,  kRankModSql,
        kTreeSql,        kNodeSql,          kRequireSql,     kExclusiveSql,      kSkillRankSql,       kRankCostSql,   kAllRankModSql,
        kPointTypeSql,   kLevelingPointSql, kItemWeaponSql,  kResourceBagSql,    kResourceBagCellSql,
    };
    for (const char* sql : sqls) {
        Stmt q(db, sql);
        if (!q.ok()) {
            error = path + " does not have the tables this world reads: " + db_message(db) +
                    " (rebuild it with this checkout's contentc: cd services && go run ./cmd/contentc)";
            return false;
        }
    }
    return true;
}

/// Rejects sub-millisecond charge/regeneration intervals so a malformed database cannot trigger millions of timer ticks
/// per frame.
constexpr double kMinInterval = 0.01;

Held held_of(const std::string& s, bool* known) {
    *known = (s == "Smart" || s == "Toggle" || s == "Naive");
    return s == "Smart" ? Held::Smart : (s == "Toggle" ? Held::Toggle : Held::Naive);
}

bool ability_fail(std::string why, std::string& error) {
    error = "content.db's ability runtime rows are not what this world runs: " + std::move(why) +
            " (rebuild it with this checkout's contentc: cd services && go run ./cmd/contentc)";
    return false;
}

bool load_ability_rule(sqlite3* db, int64_t guid, AbilityRule& rule, std::string& error) {
    Stmt q(db, kAbilityRuntimeSql);
    if (!q.ok()) return ability_fail(db_message(db), error);
    sqlite3_bind_int64(q.get(), 1, guid);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) return ability_fail("no ability_runtime row for ability " + std::to_string(guid), error);
    if (step != SQLITE_ROW) return ability_fail(db_message(db), error);
    sqlite3_stmt* s = q.get();
    rule.guid = sqlite3_column_int64(s, 0);
    rule.name = column_text(s, 1);
    bool known = false;
    rule.held = held_of(column_text(s, 2), &known);
    if (!known) return ability_fail(rule.name + " has held_behavior " + column_text(s, 2), error);
    rule.requires_out_of_combat = sqlite3_column_int(s, 3) != 0;
    rule.min_stamina = sqlite3_column_double(s, 4);
    rule.cost_stat_guid = sqlite3_column_int64(s, 5);
    rule.cost_amount = sqlite3_column_double(s, 6);
    rule.cost_requires_moving = sqlite3_column_int(s, 7) != 0;
    rule.charge_interval = sqlite3_column_double(s, 8);
    rule.cooldown = sqlite3_column_double(s, 9);
    rule.triggers_cooldown = sqlite3_column_int(s, 10) != 0;
    rule.disable_during_cooldown = sqlite3_column_int(s, 11) != 0;
    rule.targets_self = sqlite3_column_int(s, 12) != 0;
    rule.effect_guid = sqlite3_column_int64(s, 13);
    rule.montage_path = column_text(s, 14);
    rule.cancel_section = column_text(s, 15);
    return true;
}

bool load_effect_rules(sqlite3* db, AbilityRules& out, std::string& error) {
    Stmt q(db, kEffectRuntimeSql);
    if (!q.ok()) return ability_fail(db_message(db), error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        EffectRule e;
        e.guid = sqlite3_column_int64(s, 0);
        e.name = column_text(s, 1);
        e.infinite = sqlite3_column_int(s, 2) != 0;
        e.duration = sqlite3_column_double(s, 3);
        e.tick_timer = sqlite3_column_double(s, 4);
        e.stops_regen_standing = sqlite3_column_int(s, 5) != 0;
        e.stops_regen_moving = sqlite3_column_int(s, 6) != 0;
        e.end_effect_guid = sqlite3_column_int64(s, 7);
        e.end_effect_duration = sqlite3_column_double(s, 8);
        out.effects[e.guid] = std::move(e);
    }
    return step == SQLITE_DONE ? true : ability_fail(db_message(db), error);
}

bool load_effect_stat_mods(sqlite3* db, AbilityRules& out, std::string& error) {
    Stmt q(db, kEffectStatSql);
    if (!q.ok()) return ability_fail(db_message(db), error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        const auto it = out.effects.find(sqlite3_column_int64(s, 0));
        if (it == out.effects.end()) continue;
        StatMod m{sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2), sqlite3_column_double(s, 3), sqlite3_column_double(s, 4)};
        it->second.stat_mods.push_back(m);
    }
    return step == SQLITE_DONE ? true : ability_fail(db_message(db), error);
}

bool read_stamina_rule(sqlite3* db, const char* key, double* into, std::string& error) {
    Stmt q(db, kStaminaRuleSql);
    if (!q.ok()) return ability_fail(std::string("no number for the stamina rule ") + key, error);
    sqlite3_bind_text(q.get(), 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(q.get()) != SQLITE_ROW) return ability_fail(std::string("no number for the stamina rule ") + key, error);
    const std::string v = column_text(q.get(), 0);
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (v.empty() || end != v.c_str() + v.size()) return ability_fail(std::string("no number for the stamina rule ") + key, error);
    *into = d;
    return true;
}

bool validate_sprint_and_rest(const AbilityRules& out, std::string& error) {
    const AbilityRule& s = out.sprint;
    const AbilityRule& r = out.rest;
    if (s.held != Held::Smart || s.cost_stat_guid != kGuidStamina || !(s.cost_amount > 0) || !s.cost_requires_moving ||
        !(s.charge_interval >= kMinInterval) || !(s.min_stamina > 0) || s.effect_guid == 0) {
        return ability_fail("the Sprint is not a held ability that charges Stamina while moving, on an interval (at least " +
                                std::to_string(kMinInterval) + " s), from a minimum",
                            error);
    }
    if (r.held != Held::Toggle || r.cost_stat_guid != 0 || r.effect_guid == 0 || !r.targets_self) {
        return ability_fail("the Rest is not a free toggle with an effect, on the caster", error);
    }
    return true;
}

bool validate_ability_invariants(const AbilityRules& out, std::string& error) {
    if (!validate_sprint_and_rest(out, error)) return false;
    const AbilityRule& d = out.dodge;
    if (d.held != Held::Naive || d.cost_stat_guid != kGuidStamina || !(d.cost_amount > 0) || d.cost_requires_moving ||
        d.charge_interval != 0 || !(d.cooldown > 0) || !d.triggers_cooldown || d.effect_guid == 0) {
        return ability_fail("the Dodge is not a plain ability that charges Stamina once and starts a cooldown", error);
    }
    if (!(out.stat_regen_time >= kMinInterval) || !(out.regen_rate > 0)) {
        return ability_fail(
            "the Stamina regeneration is not a positive rate on an interval of at least " + std::to_string(kMinInterval) + " s", error);
    }
    return true;
}

bool validate_ability_effects(const AbilityRules& out, std::string& error) {
    for (const AbilityRule* a : {&out.sprint, &out.dodge, &out.rest}) {
        const EffectRule* e = out.effect(a->effect_guid);
        if (!e)
            return ability_fail(a->name + " applies effect " + std::to_string(a->effect_guid) + ", which has no effect_runtime row", error);
        if (e->end_effect_guid != 0 && (!out.effect(e->end_effect_guid) || !(e->end_effect_duration > 0))) {
            return ability_fail(e->name + "'s End hit applies effect " + std::to_string(e->end_effect_guid) + " for no time or with no row",
                                error);
        }
        if (!e->infinite && !(e->duration > 0)) return ability_fail(e->name + " is finite with no duration", error);
    }
    return true;
}

int read_item_row_dimensions(sqlite3_stmt* s, int64_t guid, ItemRecord& rec, std::string& error) {
    if (!column_is_int(s, 7) || !column_is_int(s, 8) || !column_is_int(s, 9)) {
        error = "content.db item " + std::to_string(guid) + " has an inv_w, inv_h or resource of the wrong storage class";
        return -1;
    }
    const int64_t w = sqlite3_column_int64(s, 7);
    const int64_t h = sqlite3_column_int64(s, 8);
    const int64_t resource = sqlite3_column_int64(s, 9);
    if (w < 1 || w > 255 || h < 1 || h > 255 || (resource != 0 && resource != 1)) {
        error = "content.db item " + std::to_string(guid) + " has the dimension " + std::to_string(w) + " x " + std::to_string(h) +
                " or the resource flag " + std::to_string(resource) + ", which contentc does not write";
        return -1;
    }
    rec.inv_w = static_cast<int32_t>(w);
    rec.inv_h = static_cast<int32_t>(h);
    rec.resource = resource == 1;
    return 1;
}

int read_item_row(sqlite3* db, int64_t guid, ItemRecord& rec, std::string& error) {
    Stmt q(db, kItemSql, guid);
    if (!q.ok()) {
        fail_db(db, "item query failed", error);
        return -1;
    }
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) return 0;
    if (step != SQLITE_ROW) {
        fail_db(db, "item query failed", error);
        return -1;
    }
    rec.guid = sqlite3_column_int64(q.get(), 0);
    rec.name = column_text(q.get(), 1);
    rec.can_equip = sqlite3_column_int(q.get(), 2) != 0;
    rec.occupies_all_equip_slots = sqlite3_column_int(q.get(), 3) != 0;
    if (!column_is_int(q.get(), 4) || !column_is_int(q.get(), 5) || !column_is_real(q.get(), 6)) {
        error = "content.db item " + std::to_string(guid) +
                " has a default_quality, max_stack_size or max_durability of the wrong storage class";
        return -1;
    }
    /// Six design-data reagent items carry negative sentinel DefaultQuality (-3/-2); clamp fresh instances to 1 so they
    /// do not fall back to the Backpack.
    const int64_t quality = std::max<int64_t>(1, sqlite3_column_int64(q.get(), 4));
    const int64_t stack = sqlite3_column_int64(q.get(), 5);
    rec.has_quality = sqlite3_column_int64(q.get(), 4) >= 1;
    if (quality > 200 || stack < 1 || stack > 1000000) {
        error = "content.db item " + std::to_string(guid) + " has default_quality " + std::to_string(quality) + " or max_stack_size " +
                std::to_string(stack) + " outside the character service's ranges";
        return -1;
    }
    rec.default_quality = static_cast<int32_t>(quality);
    rec.max_stack_size = static_cast<int32_t>(stack);
    rec.max_durability = sqlite3_column_double(q.get(), 6);
    return read_item_row_dimensions(q.get(), guid, rec, error);
}

bool read_item_equip_slots(sqlite3* db, int64_t guid, ItemRecord& rec, std::string& error) {
    Stmt q(db, kItemSlotsSql, guid);
    if (!q.ok()) return fail_db(db, "item slot query failed", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        const int slot = sqlite3_column_int(q.get(), 0);
        if (slot < 0 || slot > 255)
            return fail_msg("item " + std::to_string(guid) + " has the equip slot " + std::to_string(slot) + ", which is no EEquipSlot", error);
        rec.equip_slots.push_back(static_cast<uint8_t>(slot));
    }
    return check_done(db, step, "item slot query failed", error);
}

int read_resource_bag_header(sqlite3* db, int64_t item_guid, ResourceBagRecord& bag, std::string& error) {
    Stmt q(db, kResourceBagSql, item_guid);
    if (!q.ok()) {
        fail_db(db, "resource bag query failed", error);
        return -1;
    }
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) return 0;
    if (step != SQLITE_ROW) {
        fail_db(db, "resource bag query failed", error);
        return -1;
    }
    if (!column_is_int(q.get(), 0) || !column_is_text(q.get(), 1) || !column_is_int(q.get(), 2) || !column_is_int(q.get(), 3) ||
        !column_is_int(q.get(), 4)) {
        fail_msg("resource bag " + std::to_string(item_guid) + " has a column of the wrong storage class", error);
        return -1;
    }
    const int64_t columns = sqlite3_column_int64(q.get(), 2), rows = sqlite3_column_int64(q.get(), 3);
    if (columns < 1 || rows < 1 || columns * rows > 255) {
        fail_msg("resource bag " + std::to_string(item_guid) + " has the grid " + std::to_string(columns) + " x " + std::to_string(rows) +
                     ", outside 1 to 255 cells",
                 error);
        return -1;
    }
    bag.def = sqlite3_column_int64(q.get(), 0);
    bag.def_name = column_text(q.get(), 1);
    bag.columns = static_cast<int32_t>(columns);
    bag.rows = static_cast<int32_t>(rows);
    bag.custom = sqlite3_column_int64(q.get(), 4) != 0;
    return 1;
}

bool read_resource_bag_cells(sqlite3* db, int64_t item_guid, ResourceBagRecord& bag, std::string& error) {
    Stmt q(db, kResourceBagCellSql, bag.def);
    if (!q.ok()) return fail_db(db, "resource bag cell query failed", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        const int64_t col = sqlite3_column_int64(q.get(), 0), row = sqlite3_column_int64(q.get(), 1), value = sqlite3_column_int64(q.get(), 2);
        if (col < 0 || col > 255 || row < 0 || row > 255 || (value != -1 && value != -2))
            return fail_msg("resource bag " + std::to_string(item_guid) + " has the cell (" + std::to_string(col) + ", " +
                                std::to_string(row) + ") = " + std::to_string(value),
                            error);
        if (bag.matrix.size() <= static_cast<size_t>(col)) bag.matrix.resize(static_cast<size_t>(col) + 1);
        std::vector<int32_t>& column = bag.matrix[static_cast<size_t>(col)];
        if (column.size() <= static_cast<size_t>(row)) column.resize(static_cast<size_t>(row) + 1, -1);
        column[static_cast<size_t>(row)] = static_cast<int32_t>(value);
    }
    return check_done(db, step, "resource bag cell query failed", error);
}

bool gear_fail(sqlite3* db, int64_t guid, const char* what, std::string& error) {
    error = std::string("content.db ") + what + " query failed for item " + std::to_string(guid) + ": " + db_message(db);
    return false;
}

bool read_item_gear_curve(sqlite3* db, int64_t curve, stats::ItemGear& g, std::string& error) {
    if (curve == 0) return true;
    Stmt q(db, kQualityCurveSql, curve);
    if (!q.ok()) return gear_fail(db, g.record, "quality curve", error);
    int n = 0, step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        if (!column_is_int(q.get(), 0) || sqlite3_column_int64(q.get(), 0) != n || n >= stats::kRarities) {
            error = "content.db's quality curve " + std::to_string(curve) + " has its rarities out of order";
            return false;
        }
        if (!column_is_real(q.get(), 1) || !column_is_real(q.get(), 2)) {
            error = "content.db's quality curve " + std::to_string(curve) + " has a band that is not a number";
            return false;
        }
        g.bands[static_cast<size_t>(n++)] = {sqlite3_column_double(q.get(), 1), sqlite3_column_double(q.get(), 2)};
    }
    if (step != SQLITE_DONE) return gear_fail(db, g.record, "quality curve", error);
    if (n != stats::kRarities) {
        error = "content.db's quality curve " + std::to_string(curve) + " has " + std::to_string(n) + " rarities, not " +
                std::to_string(stats::kRarities);
        return false;
    }
    g.has_curve = true;
    return true;
}

bool read_item_gear_stats(sqlite3* db, int64_t block, const std::set<int64_t>& percent_stats, stats::ItemGear& g, std::string& error) {
    if (block == 0) return true;
    Stmt q(db, kStatBlockSql, block);
    if (!q.ok()) return gear_fail(db, g.record, "stat block", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        stats::BlockStat s;
        if (!column_is_int(q.get(), 0)) {
            error = "content.db's stat block " + std::to_string(block) + " names a stat that is not an integer";
            return false;
        }
        s.stat = sqlite3_column_int64(q.get(), 0);
        s.whole = percent_stats.count(s.stat) == 0;
        for (int r = 0; r < stats::kRarities; ++r) {
            if (!column_is_real(q.get(), 1 + 2 * r) || !column_is_real(q.get(), 2 + 2 * r)) {
                error = "content.db's stat block " + std::to_string(block) + " has a value of stat " + std::to_string(s.stat) +
                        " that is not a number";
                return false;
            }
            s.min[static_cast<size_t>(r)] = sqlite3_column_double(q.get(), 1 + 2 * r);
            s.max[static_cast<size_t>(r)] = sqlite3_column_double(q.get(), 2 + 2 * r);
        }
        g.stats.push_back(s);
    }
    return step == SQLITE_DONE ? true : gear_fail(db, g.record, "stat block", error);
}

bool read_item_gear_sets_and_tags(sqlite3* db, stats::ItemGear& g, std::string& error) {
    {
        Stmt q(db, kItemSetSql, g.record);
        if (!q.ok()) return gear_fail(db, g.record, "item set", error);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) g.sets.push_back(sqlite3_column_int64(q.get(), 0));
        if (step != SQLITE_DONE) return gear_fail(db, g.record, "item set", error);
    }
    Stmt q(db, kItemTagSql, g.record);
    if (!q.ok()) return gear_fail(db, g.record, "item tag", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) g.tags.push_back(column_text(q.get(), 0));
    return step == SQLITE_DONE ? true : gear_fail(db, g.record, "item tag", error);
}

bool validate_stat_program_columns(sqlite3_stmt* s, std::string& error) {
    if (!column_is_int(s, 0) || !column_is_text(s, 1) || !column_is_int(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 4)) {
        error =
            "content.db's stat program row has a column of the wrong storage class (version, stats, classes and crc32 "
            "are integers, mode text)";
        return false;
    }
    /// Read SQLite 64-bit integer columns before narrowing so overflowed CRC32 or count values cannot bypass provenance
    /// bounds.
    const int64_t version = sqlite3_column_int64(s, 0);
    const int64_t stats = sqlite3_column_int64(s, 2);
    const int64_t classes = sqlite3_column_int64(s, 3);
    const int64_t crc = sqlite3_column_int64(s, 4);
    if (version < 0 || version > INT32_MAX || stats < 0 || stats > INT32_MAX || classes < 0 || classes > INT32_MAX || crc < 0 ||
        crc > 0xFFFFFFFFll) {
        error = "content.db's stat program row has a version, stat count, class count or crc32 outside its range (crc32 " +
                std::to_string(crc) + ")";
        return false;
    }
    return true;
}

}  // namespace

void read_rarity_bounds(sqlite3* db, int64_t guid, items::ItemRarityFacts& out) {
    Stmt q(db, kRarityBoundsSql, guid);
    if (q.step() == SQLITE_ROW && column_is_int(q.get(), 0) && column_is_int(q.get(), 1)) {
        const int hi = items::kRarityCount - 1;
        out.rarity_min = std::clamp(col_int(q.get(), 0), 0, hi);
        out.rarity_max = std::clamp(col_int(q.get(), 1), 0, hi);
    }
}
void read_quality_curve(sqlite3* db, int64_t curve, items::ItemRarityFacts& out) {
    out.has_curve = false;
    Stmt q(db, kQualityCurveSql, curve);
    std::array<items::QualityBand, items::kRarityCount> bands{};
    int n = 0, step = SQLITE_ERROR;
    while (curve != 0 && (step = q.step()) == SQLITE_ROW) {
        if (n >= items::kRarityCount || col_i64(q.get(), 0, -1) != n || !column_is_real(q.get(), 1) || !column_is_real(q.get(), 2)) return;
        bands[static_cast<size_t>(n++)] = {col_real(q.get(), 1), col_real(q.get(), 2)};
    }
    if (step == SQLITE_DONE && n == items::kRarityCount) {
        out.bands = bands;
        out.has_curve = true;
    }
}
void read_item_rarity(sqlite3* db, int64_t guid, ItemRecord& rec) {
    Stmt g(db, kItemGearSql, guid);
    if (g.step() != SQLITE_ROW) return;
    const int64_t block = col_i64(g.get(), 0), curve = col_i64(g.get(), 1);
    read_rarity_bounds(db, guid, rec);
    read_quality_curve(db, curve, rec);
    Stmt q(db, kStatRowSql, block);
    if (block == 0 || !q.ok()) return;
    sqlite3_bind_int64(q.get(), 2, items::kMaxDurabilityStat);
    if (q.step() != SQLITE_ROW) return;
    items::RarityRange row;
    for (int r = 0; r < items::kRarityCount; ++r) {
        if (!column_is_real(q.get(), 2 * r) || !column_is_real(q.get(), 2 * r + 1)) return;
        row.min[static_cast<size_t>(r)] = col_real(q.get(), 2 * r);
        row.max[static_cast<size_t>(r)] = col_real(q.get(), 2 * r + 1);
    }
    rec.durability_stat = row;
}

ContentDb::ContentDb(ContentDb&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)),
      path_(std::move(other.path_)),
      meta_(std::move(other.meta_)),
      percent_stats_(std::move(other.percent_stats_)),
      item_cache_mu_(std::move(other.item_cache_mu_)),
      item_cache_(std::move(other.item_cache_)),
      item_gear_cache_(std::move(other.item_gear_cache_)) {}

ContentDb& ContentDb::operator=(ContentDb&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = std::exchange(other.db_, nullptr);
        path_ = std::move(other.path_);
        meta_ = std::move(other.meta_);
        percent_stats_ = std::move(other.percent_stats_);
        item_cache_mu_ = std::move(other.item_cache_mu_);
        item_cache_ = std::move(other.item_cache_);
        item_gear_cache_ = std::move(other.item_gear_cache_);
    }
    return *this;
}

ContentDb::~ContentDb() { if (db_) sqlite3_close(db_); }

std::optional<ContentDb> ContentDb::open(const std::string& path, std::string& error) {
    std::error_code ec;
    const std::filesystem::path fs_path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    if (!std::filesystem::is_regular_file(fs_path, ec)) {
        error = "content.db not found at " + path + " (build it with: cd services && go run ./cmd/contentc)";
        return std::nullopt;
    }
    ContentDb c;
    c.path_ = path;
    bool in_mem = false;
    std::string open_err;
    c.db_ = open_content_sqlite_db(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, true, &in_mem, &open_err);
    if (!c.db_) {
        error = "cannot open " + path + ": " + open_err;
        return std::nullopt;
    }
    if (in_mem) spdlog::info("[content] {}: preloaded into in-memory SQLite database", path);
    std::map<std::string, std::string> meta;
    if (!read_meta_rows(c.db_, path, meta, error) || !validate_meta_version(path, meta, c.meta_, error) ||
        !validate_required_tables(c.db_, path, error))
        return std::nullopt;
    c.load_percent_stats();
    spdlog::info("[content] {}: contentc version {}, built from the design database sha256 {}{}", path, c.meta_.contentc_version,
                 c.meta_.source_dbc_sha256, c.meta_.built_at.empty() ? "" : (", built " + c.meta_.built_at));
    return std::optional<ContentDb>(std::move(c));
}

std::optional<ClassRecord> ContentDb::class_for_archetype(int64_t archetype_id, std::string& error) const {
    Stmt q(db_, kClassSql);
    if (!q.ok()) {
        error = "content.db class query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, archetype_id);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_ROW) return ClassRecord{sqlite3_column_int64(q.get(), 0), column_text(q.get(), 1), sqlite3_column_int64(q.get(), 2)};
    error = step == SQLITE_DONE ? "no player class has archetype id " + std::to_string(archetype_id) + " in content.db"
                                : "content.db class query failed: " + db_message(db_);
    return std::nullopt;
}

std::optional<ClassRecord> ContentDb::class_for_name(const std::string& name, std::string& error) const {
    Stmt q(db_, kClassByNameSql);
    if (!q.ok()) {
        error = "content.db class query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_text(q.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_ROW) return ClassRecord{sqlite3_column_int64(q.get(), 0), column_text(q.get(), 1), sqlite3_column_int64(q.get(), 2)};
    error =
        step == SQLITE_DONE ? "no player class named '" + name + "' in content.db" : "content.db class query failed: " + db_message(db_);
    return std::nullopt;
}

std::optional<std::vector<PrimaryAttribute>> ContentDb::primary_attributes(int64_t class_guid, int level, std::string& error) const {
    Stmt q(db_, kAttributesSql);
    if (!q.ok()) {
        error = "content.db attribute query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_bind_int64(q.get(), 1, class_guid);
    sqlite3_bind_int(q.get(), 2, level);
    std::vector<PrimaryAttribute> out;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        PrimaryAttribute a{sqlite3_column_int64(q.get(), 0), column_text(q.get(), 1), sqlite3_column_double(q.get(), 2),
                           sqlite3_column_int(q.get(), 3) != 0};
        out.push_back(std::move(a));
    }
    if (step != SQLITE_DONE) {
        error = "content.db attribute query failed: " + db_message(db_);
        return std::nullopt;
    }
    if (out.empty()) {
        error = "content.db has no primary attributes for class " + std::to_string(class_guid) + " at level " + std::to_string(level) +
                " (it covers levels " + std::to_string(kMinLevel) + " to " + std::to_string(kMaxLevel) + ")";
        return std::nullopt;
    }
    return out;
}

std::optional<AbilityRules> ContentDb::ability_rules(std::string& error) const {
    AbilityRules out;
    if (!load_ability_rule(db_, kGuidSprint, out.sprint, error) || !load_ability_rule(db_, kGuidDodge, out.dodge, error) ||
        !load_ability_rule(db_, kGuidRest, out.rest, error) || !load_effect_rules(db_, out, error) ||
        !load_effect_stat_mods(db_, out, error) || !read_stamina_rule(db_, "stat_regen_time", &out.stat_regen_time, error) ||
        !read_stamina_rule(db_, "regen.rate_enabled", &out.regen_rate, error) || !validate_ability_invariants(out, error) ||
        !validate_ability_effects(out, error))
        return std::nullopt;
    return out;
}

std::optional<ItemRecord> ContentDb::item(int64_t guid, std::string& error) const {
    error.clear();
    if (item_cache_mu_) {
        std::lock_guard<std::mutex> lk(*item_cache_mu_);
        if (const auto it = item_cache_.find(guid); it != item_cache_.end()) return it->second;
    }
    ItemRecord rec;
    if (read_item_row(db_, guid, rec, error) <= 0) {
        if (error.empty() && item_cache_mu_) { std::lock_guard<std::mutex> lk(*item_cache_mu_); item_cache_.try_emplace(guid, std::nullopt); }
        return std::nullopt;
    }
    { Stmt q(db_, kItemNameSql, guid); if (q.step() == SQLITE_ROW) rec.display_name = column_text(q.get(), 0); }
    read_item_rarity(db_, guid, rec);
    if (!read_item_equip_slots(db_, guid, rec, error)) return std::nullopt;
    std::optional<ItemRecord> out(std::move(rec));
    if (item_cache_mu_) { std::lock_guard<std::mutex> lk(*item_cache_mu_); item_cache_.try_emplace(guid, out); }
    return out;
}

std::optional<ResourceBagRecord> ContentDb::resource_bag(int64_t item_guid, std::string& error) const {
    error.clear();
    ResourceBagRecord bag;
    bag.item = item_guid;
    if (read_resource_bag_header(db_, item_guid, bag, error) <= 0 || !read_resource_bag_cells(db_, item_guid, bag, error))
        return std::nullopt;
    return bag;
}

std::optional<StatProgramRow> ContentDb::stat_program(std::string& error) const {
    Stmt q(db_, kStatProgramSql);
    if (!q.ok()) {
        error = "content.db stat program query failed: " + db_message(db_);
        return std::nullopt;
    }
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) {
        error = "content.db has no stat program";
        return std::nullopt;
    }
    if (step != SQLITE_ROW) {
        error = "content.db stat program query failed: " + db_message(db_);
        return std::nullopt;
    }
    sqlite3_stmt* s = q.get();
    if (!validate_stat_program_columns(s, error)) return std::nullopt;
    const void* blob = sqlite3_column_blob(s, 5);
    const int n = sqlite3_column_bytes(s, 5);
    if (!blob || n <= 0) {
        error = "content.db's stat program is empty";
        return std::nullopt;
    }
    StatProgramRow r;
    r.version = static_cast<int>(sqlite3_column_int64(s, 0));
    r.mode = column_text(s, 1);
    r.stats = static_cast<int>(sqlite3_column_int64(s, 2));
    r.classes = static_cast<int>(sqlite3_column_int64(s, 3));
    r.crc32 = static_cast<uint32_t>(sqlite3_column_int64(s, 4));
    r.data.assign(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + n);
    return r;
}

std::optional<stats::ItemGear> ContentDb::item_gear(int64_t guid, std::string& error) const {
    error.clear();
    if (item_cache_mu_) {
        std::lock_guard<std::mutex> lk(*item_cache_mu_);
        if (const auto it = item_gear_cache_.find(guid); it != item_gear_cache_.end()) return it->second;
    }
    stats::ItemGear g;
    g.record = guid;
    int64_t block = 0, curve = 0;
    {
        Stmt q(db_, kItemGearSql);
        if (!q.ok()) { error = "content.db item gear query failed: " + db_message(db_); return std::nullopt; }
        sqlite3_bind_int64(q.get(), 1, guid);
        const int step = sqlite3_step(q.get());
        if (step == SQLITE_DONE) {
            if (item_cache_mu_) { std::lock_guard<std::mutex> lk(*item_cache_mu_); item_gear_cache_.try_emplace(guid, std::nullopt); }
            return std::nullopt;
        }
        if (step != SQLITE_ROW) { error = "content.db item gear query failed: " + db_message(db_); return std::nullopt; }
        block = sqlite3_column_int64(q.get(), 0);
        curve = sqlite3_column_int64(q.get(), 1);
    }
    read_rarity_bounds(db_, guid, g);
    if (!read_item_gear_curve(db_, curve, g, error) || !read_item_gear_stats(db_, block, percent_stats_, g, error) ||
        !read_item_gear_sets_and_tags(db_, g, error))
        return std::nullopt;
    if (item_cache_mu_) { std::lock_guard<std::mutex> lk(*item_cache_mu_); item_gear_cache_.try_emplace(guid, g); }
    return g;
}

}  // namespace aoc::worldd::content
