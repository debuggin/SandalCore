// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "content/content_db_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

namespace aoc::worldd::content {

namespace {

int64_t int_of(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<int64_t>() : 0;
}

std::string str_of(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

void json_lenient_ints(const nlohmann::json& o, const char* key, std::vector<int64_t>& out) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return;
    for (const auto& v : *it) {
        if (v.is_number_integer()) out.push_back(v.get<int64_t>());
    }
}

void json_lenient_strings(const nlohmann::json& o, const char* key, std::vector<std::string>& out) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return;
    for (const auto& v : *it) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
}

bool parse_criterion_object(const nlohmann::json& c, DialogueCriterion& d) {
    if (!c.is_object()) return false;
    d.kind = str_of(c, "t");
    d.op = str_of(c, "op");
    d.value = int_of(c, "v");
    d.quest = int_of(c, "q");
    d.quest_query = str_of(c, "qq");
    d.objective = int_of(c, "o");
    d.objective_query = str_of(c, "oq");
    d.variable = str_of(c, "var");
    d.currency = int_of(c, "cur");
    d.expression = str_of(c, "x");
    d.right_variable = str_of(c, "rv");
    json_lenient_ints(c, "items", d.items);
    json_lenient_strings(c, "tags", d.tags);
    return true;
}

bool parse_requirement_object(const nlohmann::json& j, DialogueRequirement& out) {
    out = DialogueRequirement{};
    if (!j.is_object()) return false;
    const auto tok = j.find("tok");
    const auto q = j.find("q");
    if (tok == j.end() || !tok->is_array() || q == j.end() || !q->is_array()) return false;
    for (const auto& t : *tok) {
        if (!t.is_number_integer()) return false;
        out.tokens.push_back(t.get<int>());
    }
    for (const auto& c : *q) {
        DialogueCriterion d;
        if (!parse_criterion_object(c, d)) return false;
        out.criteria.push_back(std::move(d));
    }
    return true;
}

std::vector<int64_t> json_ids(const std::string& text) {
    std::vector<int64_t> out;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (e.is_number_integer()) out.push_back(e.get<int64_t>());
    }
    return out;
}

std::vector<int64_t> int_list(const nlohmann::json& j, const char* key) {
    std::vector<int64_t> out;
    const auto it = j.find(key);
    if (it != j.end() && it->is_array()) {
        for (const auto& v : *it) out.push_back(v.is_number() ? v.get<int64_t>() : 0);
    }
    return out;
}

std::vector<double> number_list(const nlohmann::json& j, const char* key) {
    std::vector<double> out;
    const auto it = j.find(key);
    if (it != j.end() && it->is_array()) {
        for (const auto& v : *it) out.push_back(v.is_number() ? v.get<double>() : 0.0);
    }
    return out;
}

bool object_failed(sqlite3* db, const char* table, std::string& error) {
    error = std::string("content.db ") + table + " query failed: " + db_message(db) +
            " (the quest objects are contentc version 36's: rebuild the file with this checkout's contentc)";
    return false;
}

void parse_interactable_grants(const std::string& text, std::vector<std::pair<int64_t, int64_t>>& out) {
    const auto grants = nlohmann::json::parse(text, nullptr, false);
    if (!grants.is_array()) return;
    for (const auto& g : grants) {
        if (g.is_array() && g.size() == 2 && g[0].is_number_integer() && g[1].is_number_integer()) {
            out.emplace_back(g[0].get<int64_t>(), g[1].get<int64_t>());
        }
    }
}

bool read_interactable_row(sqlite3_stmt* s, ObjectData& out, std::string& error) {
    InteractableRow r;
    const int64_t guid = sqlite3_column_int64(s, 0);
    r.name = column_text(s, 1);
    r.radius = sqlite3_column_double(s, 2);
    r.tolerance = sqlite3_column_double(s, 3);
    r.discovery = sqlite3_column_double(s, 4);
    r.require_los = sqlite3_column_int(s, 5) != 0;
    const std::string requirement = column_text(s, 6);
    if (!requirement.empty()) {
        const auto j = nlohmann::json::parse(requirement, nullptr, false);
        if (j.is_discarded() || !parse_requirement_object(j, r.requirement)) {
            error = "content.db interactable " + std::to_string(guid) + ": the rule of its prompt does not read";
            return false;
        }
    }
    r.pid = sqlite3_column_int64(s, 7);
    r.channel = sqlite3_column_double(s, 8);
    r.busy = sqlite3_column_double(s, 9);
    r.allow_combat = sqlite3_column_int(s, 10) != 0;
    r.single_use = sqlite3_column_int(s, 11) != 0;
    r.delete_done = sqlite3_column_int(s, 12) != 0;
    r.delete_delay = sqlite3_column_double(s, 13);
    r.anim = column_text(s, 14);
    parse_interactable_grants(column_text(s, 15), r.grants);
    r.reward_table = sqlite3_column_int64(s, 16);
    r.complete_objs = json_ids(column_text(s, 17));
    r.reveal_objs = json_ids(column_text(s, 18));
    out.interactables.emplace(guid, std::move(r));
    return true;
}

bool load_interactable_rows(sqlite3* db, ObjectData& out, std::string& error) {
    Stmt q(db, "SELECT icd_guid, name, radius, tolerance, discovery, require_los, requirement, pid_guid, channel, busy, allow_combat, "
               "single_use, delete_done, delete_delay, anim, grants, reward_table, complete_objs, reveal_objs FROM interactable");
    if (!q.ok()) return object_failed(db, "interactable", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        if (!read_interactable_row(q.get(), out, error)) return false;
    }
    return step == SQLITE_DONE ? true : object_failed(db, "interactable", error);
}

bool load_actor_defs(sqlite3* db, ObjectData& out, std::string& error) {
    Stmt q(db, "SELECT iad_guid, despawn_delay, despawn_final FROM interactable_actor_def");
    if (!q.ok()) return object_failed(db, "interactable_actor_def", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        ActorDefRow r{sqlite3_column_double(s, 1), sqlite3_column_int(s, 2) != 0};
        out.actor_defs.emplace(sqlite3_column_int64(s, 0), r);
    }
    return step == SQLITE_DONE ? true : object_failed(db, "interactable_actor_def", error);
}

bool load_object_aux_maps(sqlite3* db, ObjectData& out, std::string& error) {
    {
        Stmt q(db, "SELECT instance_guid, iad_guid FROM instance_iad");
        if (!q.ok()) return object_failed(db, "instance_iad", error);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            out.instance_iad[sqlite3_column_int64(q.get(), 0)] = sqlite3_column_int64(q.get(), 1);
        }
        if (step != SQLITE_DONE) return object_failed(db, "instance_iad", error);
    }
    {
        Stmt q(db, "SELECT item_guid, major FROM quest_item");
        if (!q.ok()) return object_failed(db, "quest_item", error);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            out.quest_items[sqlite3_column_int64(q.get(), 0)] = sqlite3_column_int(q.get(), 1) != 0;
        }
        if (step != SQLITE_DONE) return object_failed(db, "quest_item", error);
    }
    Stmt q(db, "SELECT item_guid, table_guid FROM item_box");
    if (!q.ok()) return object_failed(db, "item_box", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        out.item_boxes[sqlite3_column_int64(q.get(), 0)] = sqlite3_column_int64(q.get(), 1);
    }
    return step == SQLITE_DONE ? true : object_failed(db, "item_box", error);
}

bool station_failed(sqlite3* db, const char* table, std::string& error) {
    error = std::string("content.db ") + table + " query failed: " + db_message(db) +
            " (the crafting tables are contentc version 20's: rebuild the file with this checkout's contentc)";
    return false;
}

bool load_craft_station_defs(sqlite3* db, CraftStationData& out, std::string& error) {
    Stmt q(db, "SELECT guid, name, display_name, kind, max_tier, queue_size, simultaneous, individual, tax_tag, recipe_set_guid FROM craft_station_def");
    if (!q.ok()) return station_failed(db, "craft_station_def", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 4) || !column_is_int(s, 5) || !column_is_int(s, 6) || !column_is_int(s, 9)) {
            error = "content.db craft_station_def has a column of the wrong storage class";
            return false;
        }
        CraftStationDef d;
        d.guid = sqlite3_column_int64(s, 0);
        d.name = column_text(s, 1);
        d.display_name = column_text(s, 2);
        d.processing = column_text(s, 3) == "Processing";
        d.max_tier = static_cast<int>(sqlite3_column_int64(s, 4));
        d.queue_size = static_cast<int>(sqlite3_column_int64(s, 5));
        d.simultaneous = static_cast<int>(sqlite3_column_int64(s, 6));
        d.individual = sqlite3_column_int64(s, 7) != 0;
        d.tax_tag = column_text(s, 8);
        d.recipe_set = sqlite3_column_int64(s, 9);
        out.by_name[d.name] = d.guid;
        out.defs[d.guid] = std::move(d);
    }
    return step == SQLITE_DONE ? true : station_failed(db, "craft_station_def", error);
}

bool load_craft_station_assets(sqlite3* db, CraftStationData& out, std::string& error) {
    Stmt q(db, "SELECT asset_guid, def_guid FROM craft_station_asset");
    if (!q.ok()) return station_failed(db, "craft_station_asset", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 1)) {
            error = "content.db craft_station_asset has a column of the wrong storage class";
            return false;
        }
        out.asset_defs[sqlite3_column_int64(s, 0)] = sqlite3_column_int64(s, 1);
    }
    return step == SQLITE_DONE ? true : station_failed(db, "craft_station_asset", error);
}

bool respawn_failed(sqlite3* db, const char* table, std::string& error) {
    error = std::string("content.db ") + table + " query failed: " + db_message(db) +
            " (the respawn tables are contentc version 23's: rebuild the file with this checkout's contentc)";
    return false;
}

bool load_attune_points(sqlite3* db, RespawnData& out, std::string& error) {
    Stmt q(db, "SELECT guid, name, idx, x, y, z, radius, descriptor, level_name FROM attune_point");
    if (!q.ok()) return respawn_failed(db, "attune_point", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 2)) {
            error = "content.db attune_point has a column of the wrong storage class";
            return false;
        }
        AttunePoint p;
        p.guid = sqlite3_column_int64(s, 0);
        p.name = column_text(s, 1);
        p.number = static_cast<int>(sqlite3_column_int64(s, 2));
        p.x = sqlite3_column_double(s, 3);
        p.y = sqlite3_column_double(s, 4);
        p.z = sqlite3_column_double(s, 5);
        p.radius = sqlite3_column_double(s, 6);
        p.descriptor = column_text(s, 7);
        p.level_name = column_text(s, 8);
        out.places[p.guid] = std::move(p);
    }
    return step == SQLITE_DONE ? true : respawn_failed(db, "attune_point", error);
}

bool load_respawn_points(sqlite3* db, RespawnData& out, std::string& error) {
    Stmt q(db, "SELECT guid, name, kind, attune_guid, box_x, box_y, box_z, exclusion, max_range, descriptor FROM respawn_point");
    if (!q.ok()) return respawn_failed(db, "respawn_point", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 2) || !column_is_int(s, 3)) {
            error = "content.db respawn_point has a column of the wrong storage class";
            return false;
        }
        RespawnDef d;
        d.guid = sqlite3_column_int64(s, 0);
        d.name = column_text(s, 1);
        d.kind = static_cast<int>(sqlite3_column_int64(s, 2));
        d.attune = sqlite3_column_int64(s, 3);
        d.box_x = sqlite3_column_double(s, 4);
        d.box_y = sqlite3_column_double(s, 5);
        d.box_z = sqlite3_column_double(s, 6);
        d.exclusion = sqlite3_column_double(s, 7);
        d.max_range = sqlite3_column_double(s, 8);
        d.descriptor = column_text(s, 9);
        out.respawns[d.guid] = std::move(d);
    }
    return step == SQLITE_DONE ? true : respawn_failed(db, "respawn_point", error);
}

bool load_npc_respawns(sqlite3* db, RespawnData& out, std::string& error) {
    Stmt q(db, "SELECT instance_guid, respawn_guid FROM npc_instance_respawn");
    if (!q.ok()) return respawn_failed(db, "npc_instance_respawn", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 1)) {
            error = "content.db npc_instance_respawn has a column of the wrong storage class";
            return false;
        }
        out.instance_respawn[sqlite3_column_int64(s, 0)] = sqlite3_column_int64(s, 1);
    }
    return step == SQLITE_DONE ? true : respawn_failed(db, "npc_instance_respawn", error);
}

std::vector<int64_t> formula_guids_of(const std::string& text) {
    std::vector<int64_t> out;
    for (size_t at = text.find("$#"); at != std::string::npos; at = text.find("$#", at + 2)) {
        const size_t colon = text.find(':', at);
        const size_t end = text.find('$', at + 2);
        if (colon == std::string::npos || end == std::string::npos || colon > end) continue;
        out.push_back(std::atoll(text.substr(colon + 1, end - colon - 1).c_str()));
    }
    return out;
}

XpRule profession_rule_named(const std::string& name) {
    if (name == "Artisanship_Processing_BaseXP") return XpRule::Processing;
    if (name == "Artisanship_Processing_BaseXP_AnimalHusbandry") return XpRule::AnimalHusbandry;
    if (name == "Artisanship_Crafting_BaseXP") return XpRule::Crafting;
    return XpRule::Unknown;
}

XpRule adventuring_rule_named(const std::string& name) {
    if (name == "Adventuring_Processing_BaseXP") return XpRule::Processing;
    if (name == "Adventuring_Crafting_BaseXP") return XpRule::Crafting;
    return XpRule::Unknown;
}

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

void parse_craft_reward_items(const nlohmann::json& r, std::vector<CraftRewardItem>& items, int& unread) {
    const auto item_rewards = r.find("ItemRewards");
    if (item_rewards == r.end() || !item_rewards->is_array()) return;
    for (const nlohmann::json& it : *item_rewards) {
        const auto item = it.find("Item");
        if (item == it.end() || !item->is_object()) continue;
        const std::string quantity = str_of(it, "Quantity");
        if (quantity.empty()) continue;
        if (int_of(*item, "VariationId") != 0 || !all_digits(quantity)) {
            ++unread;
            continue;
        }
        CraftRewardItem x{int_of(*item, "ItemId"), std::atoll(quantity.c_str())};
        if (x.item != 0 && x.quantity > 0) items.push_back(x);
    }
}

void parse_craft_reward_xp_entry(const nlohmann::json& x, CraftReward& out, const std::function<std::string(int64_t)>& formula_name) {
    const std::string type = str_of(x, "ExperienceType");
    const std::vector<int64_t> formulas = formula_guids_of(str_of(x, "ExperienceValue"));
    const std::string base = formulas.empty() ? std::string() : formula_name(formulas.front());
    const std::string fallback = base.empty() ? std::string("(none)") : base;
    if (type == "EExperienceContextType::Profession") {
        out.profession = int_of(x, "ProfessionsId");
        out.profession_rule = profession_rule_named(base);
        if (out.profession_rule == XpRule::Unknown && out.unknown_formula.empty()) out.unknown_formula = fallback;
    } else if (type == "EExperienceContextType::Adventuring") {
        out.adventuring_rule = adventuring_rule_named(base);
        if (out.adventuring_rule == XpRule::Unknown && out.unknown_formula.empty()) out.unknown_formula = fallback;
    } else {
        ++out.unread;
    }
}

void parse_craft_reward_entry(const nlohmann::json& r, CraftRewardContainer& box, CraftReward& out,
                              const std::function<std::string(int64_t)>& formula_name) {
    if (!r.is_object()) return;
    const auto currency = r.find("Currency");
    if (currency != r.end() && currency->is_array() && !currency->empty()) ++out.unread;
    std::vector<CraftRewardItem> items;
    parse_craft_reward_items(r, items, out.unread);
    const auto experience = r.find("ExperienceRewards");
    if (experience != r.end() && experience->is_array()) {
        for (const nlohmann::json& x : *experience) parse_craft_reward_xp_entry(x, out, formula_name);
    }
    if (!items.empty()) box.rewards.push_back(std::move(items));
}

void parse_craft_reward_container(const nlohmann::json& c, CraftReward& out, const std::function<std::string(int64_t)>& formula_name) {
    if (!c.is_object()) return;
    if (str_of(c, "GrantMethod") != "ERewardGrantMethod::Always") {
        ++out.unread;
        return;
    }
    CraftRewardContainer box;
    const std::string algorithm = str_of(c, "SelectionAlgorithm");
    if (algorithm == "ERewardSelectionAlgorithm::PureRandom") {
        box.pick = CraftRewardContainer::Pick::PureRandom;
    } else if (algorithm == "ERewardSelectionAlgorithm::PerElementWeightedRandom") {
        box.pick = CraftRewardContainer::Pick::WeightedRandom;
    } else if (algorithm != "ERewardSelectionAlgorithm::All") {
        ++out.unread;
        return;
    }
    box.number = static_cast<int>(int_of(c, "NumberToSelect"));
    box.weights = number_list(c, "WeightsPerReward");
    const auto rewards = c.find("Rewards");
    if (rewards == c.end() || !rewards->is_array()) return;
    for (const nlohmann::json& r : *rewards) parse_craft_reward_entry(r, box, out, formula_name);
    if (!box.rewards.empty()) out.containers.push_back(std::move(box));
}

void read_craft_reward(const std::map<int64_t, std::string>& bodies, int64_t guid, CraftReward& out, std::vector<int64_t>& seen,
                       const std::function<std::string(int64_t)>& formula_name) {
    if (seen.size() >= 16 || std::find(seen.begin(), seen.end(), guid) != seen.end()) return;
    seen.push_back(guid);
    const auto body = bodies.find(guid);
    if (body == bodies.end()) return;
    const nlohmann::json j = nlohmann::json::parse(body->second, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        ++out.unread;
        return;
    }
    const auto containers = j.find("RewardDefContainers");
    if (containers != j.end() && containers->is_array()) {
        for (const nlohmann::json& c : *containers) parse_craft_reward_container(c, out, formula_name);
    }
    for (const int64_t sub : int_list(j, "SubTablesIds")) read_craft_reward(bodies, sub, out, seen, formula_name);
}

std::vector<std::pair<double, double>> stat_curve_points(sqlite3* db, int64_t guid) {
    std::vector<std::pair<double, double>> out;
    Stmt q(db, "SELECT level, value FROM stat_curve WHERE guid = ?1 ORDER BY ord");
    if (!q.ok()) return out;
    sqlite3_bind_int64(q.get(), 1, guid);
    while (sqlite3_step(q.get()) == SQLITE_ROW) {
        out.emplace_back(sqlite3_column_double(q.get(), 0), sqlite3_column_double(q.get(), 1));
    }
    return out;
}

bool craft_failed(sqlite3* db, const char* table, std::string& error) {
    return station_failed(db, table, error);
}

bool craft_unread(const char* table, std::string& error) {
    error = std::string("content.db ") + table + " has a column of the wrong storage class";
    return false;
}

bool valid_craft_recipe_cols(sqlite3_stmt* s) {
    for (const int col : {0, 2, 3, 6, 7, 8, 10, 11}) {
        if (!column_is_int(s, col)) return false;
    }
    return column_is_real(s, 4);
}

bool load_craft_recipes(sqlite3* db, CraftRecipeData& out, std::string& error) {
    Stmt q(db, "SELECT guid, name, profession_guid, tier, base_duration, std_cost, cost_guid, reward_guid, learnable, destination, "
               "fuel, blacklisted, category_tag, duration_formula, quantity_formula, quality_formula FROM craft_recipe");
    if (!q.ok()) return craft_failed(db, "craft_recipe", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!valid_craft_recipe_cols(s)) return craft_unread("craft_recipe", error);
        CraftRecipe r;
        r.guid = sqlite3_column_int64(s, 0);
        r.name = column_text(s, 1);
        r.profession = sqlite3_column_int64(s, 2);
        r.tier = static_cast<int>(sqlite3_column_int64(s, 3));
        r.base_duration = sqlite3_column_double(s, 4);
        r.std_cost = column_text(s, 5);
        r.cost_guid = sqlite3_column_int64(s, 6);
        r.reward = sqlite3_column_int64(s, 7);
        r.learnable = sqlite3_column_int64(s, 8) != 0;
        r.inventory_destination = column_text(s, 9) == "Inventory";
        r.fuel = static_cast<int>(sqlite3_column_int64(s, 10));
        r.blacklisted = sqlite3_column_int64(s, 11) != 0;
        r.category_tag = column_text(s, 12);
        r.has_duration_formula = !column_text(s, 13).empty();
        r.has_quantity_formula = !column_text(s, 14).empty();
        r.has_quality_formula = !column_text(s, 15).empty();
        out.recipes[r.guid] = std::move(r);
    }
    return step == SQLITE_DONE ? true : craft_failed(db, "craft_recipe", error);
}

bool load_craft_recipe_inputs(sqlite3* db, CraftRecipeData& out, std::string& error) {
    Stmt q(db, "SELECT recipe_guid, kind, item_guid, filter_guid, tags, quantity, min_rarity, consume FROM craft_recipe_input ORDER BY recipe_guid, kind DESC, ord");
    if (!q.ok()) return craft_failed(db, "craft_recipe_input", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 2) || !column_is_int(s, 3) || !column_is_int(s, 5) || !column_is_int(s, 6) || !column_is_int(s, 7)) {
            return craft_unread("craft_recipe_input", error);
        }
        const auto r = out.recipes.find(sqlite3_column_int64(s, 0));
        if (r == out.recipes.end()) continue;
        CraftInput in;
        in.primary = column_text(s, 1) == "primary";
        in.item = sqlite3_column_int64(s, 2);
        in.filter = sqlite3_column_int64(s, 3);
        in.tags = column_text(s, 4);
        in.quantity = static_cast<int>(sqlite3_column_int64(s, 5));
        in.min_rarity = static_cast<int>(sqlite3_column_int64(s, 6));
        in.consume = sqlite3_column_int64(s, 7) != 0;
        r->second.inputs.push_back(std::move(in));
    }
    return step == SQLITE_DONE ? true : craft_failed(db, "craft_recipe_input", error);
}

bool load_craft_preds(sqlite3* db, CraftRecipeData& out, std::string& error) {
    Stmt q(db, "SELECT owner_kind, owner_guid, type, node_level_n, biomes, modifications, nodes, hidden FROM craft_pred ORDER BY owner_kind, owner_guid, ord");
    if (!q.ok()) return craft_failed(db, "craft_pred", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 1) || !column_is_int(s, 3) || !column_is_int(s, 7)) return craft_unread("craft_pred", error);
        CraftPred p{column_text(s, 2), static_cast<int>(sqlite3_column_int64(s, 3)), column_text(s, 4),
                    column_text(s, 5), column_text(s, 6), sqlite3_column_int64(s, 7) != 0};
        const int64_t owner = sqlite3_column_int64(s, 1);
        if (column_text(s, 0) == "list") {
            out.list_preds[owner].push_back(std::move(p));
        } else if (const auto r = out.recipes.find(owner); r != out.recipes.end()) {
            r->second.preds.push_back(std::move(p));
        }
    }
    return step == SQLITE_DONE ? true : craft_failed(db, "craft_pred", error);
}

bool load_craft_costs_and_sets(sqlite3* db, CraftRecipeData& out, std::string& error) {
    {
        Stmt q(db, "SELECT guid, tier_guid, value FROM craft_cost");
        if (!q.ok()) return craft_failed(db, "craft_cost", error);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 1)) return craft_unread("craft_cost", error);
            out.costs[sqlite3_column_int64(s, 0)] = {sqlite3_column_int64(s, 1), static_cast<int64_t>(std::atoll(column_text(s, 2).c_str()))};
        }
        if (step != SQLITE_DONE) return craft_failed(db, "craft_cost", error);
    }
    Stmt q(db, "SELECT set_guid, list_guid FROM craft_recipe_set_list ORDER BY set_guid, ord");
    if (!q.ok()) return craft_failed(db, "craft_recipe_set_list", error);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 1)) return craft_unread("craft_recipe_set_list", error);
        out.set_lists[sqlite3_column_int64(s, 0)].push_back(sqlite3_column_int64(s, 1));
    }
    return step == SQLITE_DONE ? true : craft_failed(db, "craft_recipe_set_list", error);
}

bool load_craft_list_and_tags(sqlite3* db, CraftRecipeData& out, std::string& error) {
    {
        Stmt q(db, "SELECT list_guid, recipe_guid FROM craft_recipe_list_member ORDER BY list_guid, ord");
        if (!q.ok()) return craft_failed(db, "craft_recipe_list_member", error);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 1)) return craft_unread("craft_recipe_list_member", error);
            out.list_recipes[sqlite3_column_int64(s, 0)].push_back(sqlite3_column_int64(s, 1));
        }
        if (step != SQLITE_DONE) return craft_failed(db, "craft_recipe_list_member", error);
    }
    Stmt q(db, "SELECT tag, item_guid FROM craft_tag_item ORDER BY tag, item_guid");
    if (!q.ok()) return true;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 1)) return craft_unread("craft_tag_item", error);
        out.tag_items[column_text(s, 0)].push_back(sqlite3_column_int64(s, 1));
    }
    if (step != SQLITE_DONE) return craft_failed(db, "craft_tag_item", error);
    out.tags_read = true;
    return true;
}

std::string lookup_formula_name(sqlite3* db, std::map<int64_t, std::string>& names, int64_t guid) {
    const auto known = names.find(guid);
    if (known != names.end()) return known->second;
    std::string name;
    Stmt q(db, "SELECT name FROM record WHERE guid = ?1 AND schema = 'StatFormulaTypeRecordBase'");
    if (q.ok()) {
        sqlite3_bind_int64(q.get(), 1, guid);
        if (sqlite3_step(q.get()) == SQLITE_ROW) name = column_text(q.get(), 0);
    }
    names[guid] = name;
    return name;
}

bool load_craft_rewards_and_xp(sqlite3* db, CraftRecipeData& out, std::string& error) {
    std::map<int64_t, std::string> bodies;
    {
        Stmt q(db, "SELECT guid, body FROM craft_reward");
        if (!q.ok()) return craft_failed(db, "craft_reward", error);
        int step = SQLITE_OK;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0)) return craft_unread("craft_reward", error);
            bodies[sqlite3_column_int64(s, 0)] = column_text(s, 1);
        }
        if (step != SQLITE_DONE) return craft_failed(db, "craft_reward", error);
    }
    std::map<int64_t, std::string> names;
    const auto formula_name = [&](int64_t guid) { return lookup_formula_name(db, names, guid); };
    for (const auto& [guid, recipe] : out.recipes) {
        if (recipe.reward == 0 || out.rewards.count(recipe.reward) != 0) continue;
        CraftReward reward;
        std::vector<int64_t> seen;
        read_craft_reward(bodies, recipe.reward, reward, seen, formula_name);
        out.rewards.emplace(recipe.reward, std::move(reward));
    }
    out.xp_curves.kill = stat_curve_points(db, kCurveBaseKillXp);
    out.xp_curves.tier[0] = stat_curve_points(db, kCurveTierModNovice);
    out.xp_curves.tier[1] = stat_curve_points(db, kCurveTierModApprentice);
    out.xp_curves.tier[2] = stat_curve_points(db, kCurveTierModJourneyman);
    return true;
}

ArtisanData::Curve curve_named(sqlite3* db, const std::string& name) {
    ArtisanData::Curve points;
    Stmt q(db, "SELECT level, value FROM stat_curve WHERE name = ?1 ORDER BY ord");
    if (!q.ok()) return points;
    sqlite3_bind_text(q.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    while (sqlite3_step(q.get()) == SQLITE_ROW) {
        points.emplace_back(sqlite3_column_double(q.get(), 0), sqlite3_column_double(q.get(), 1));
    }
    return points;
}

std::string lower_of(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int64_t rating_stat_of(const std::map<std::string, int64_t>& rating, const std::string& name) {
    const auto it = rating.find(lower_of(name));
    return it == rating.end() ? 0 : it->second;
}

void load_artisan_ratings(sqlite3* db, std::map<std::string, int64_t>& rating) {
    Stmt q(db, "SELECT guid, name FROM stat_def WHERE name LIKE 'Artisan%Rating'");
    if (!q.ok()) return;
    while (sqlite3_step(q.get()) == SQLITE_ROW) {
        if (column_is_int(q.get(), 0)) rating[lower_of(column_text(q.get(), 1))] = sqlite3_column_int64(q.get(), 0);
    }
}

void load_artisan_professions(sqlite3* db, const std::map<std::string, int64_t>& rating, ArtisanData& a) {
    Stmt q(db, "SELECT guid, name, category FROM profession");
    if (!q.ok()) return;
    while (sqlite3_step(q.get()) == SQLITE_ROW) {
        if (!column_is_int(q.get(), 0)) continue;
        const int64_t guid = sqlite3_column_int64(q.get(), 0);
        const std::string name = column_text(q.get(), 1);
        const std::string category = column_text(q.get(), 2);
        ArtisanProfession p;
        if (category == "Crafting") {
            p.quality = rating_stat_of(rating, "Artisan_Crafting_" + name + "_Quality_Rating");
            p.xp = rating_stat_of(rating, "Artisan_Crafting_" + name + "_XP_Rating");
            if (p.quality != 0 || p.xp != 0) {
                a.by_profession[guid] = p;
                ++a.crafting_professions;
            }
        } else if (category == "Processing") {
            p.processing = true;
            p.time = rating_stat_of(rating, "Artisan_Processing_" + name + "_Time_Rating");
            p.quantity = rating_stat_of(rating, "Artisan_Processing_" + name + "_Quantity_Rating");
            p.rarity = rating_stat_of(rating, "Artisan_Processing_" + name + "_Rarity_Rating");
            p.xp = rating_stat_of(rating, "Artisan_Processing_" + name + "_XP_Rating");
            if (p.time != 0 || p.quantity != 0 || p.rarity != 0 || p.xp != 0) {
                a.by_profession[guid] = p;
                ++a.processing_professions;
            }
        }
    }
}

void load_artisan_data(sqlite3* db, ArtisanData& a) {
    a.crafting_quality = curve_named(db, "CraftingQuality");
    a.crafting_xp = curve_named(db, "CraftingXP");
    a.processing_time = curve_named(db, "ProcessingTime");
    a.processing_xp = curve_named(db, "ProcessingXP");
    static const char* const kRarities[] = {"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary"};
    for (int i = 0; i < 5; ++i) a.rarity_step[i] = curve_named(db, std::string("ProcessingRarityIncrease_") + kRarities[i]);
    for (int i = 0; i < 6; ++i) a.quantity_chance[i] = curve_named(db, std::string("ProcessingQuantityChance_") + kRarities[i]);
    std::map<std::string, int64_t> rating;
    load_artisan_ratings(db, rating);
    a.general_crafting.quality = rating_stat_of(rating, "Artisan_Crafting_Quality_Rating");
    a.general_crafting.xp = rating_stat_of(rating, "Artisan_Crafting_XP_Rating");
    a.general_processing.processing = true;
    a.general_processing.time = rating_stat_of(rating, "Artisan_Processing_Time_Rating");
    a.general_processing.quantity = rating_stat_of(rating, "Artisan_Processing_Quantity_Rating");
    a.general_processing.rarity = rating_stat_of(rating, "Artisan_Processing_Rarity_Rating");
    a.general_processing.xp = rating_stat_of(rating, "Artisan_Processing_XP_Rating");
    load_artisan_professions(db, rating, a);
}

}  // namespace

std::optional<ObjectData> ContentDb::object_data(std::string& error) const {
    ObjectData out;
    if (!load_interactable_rows(db_, out, error) || !load_actor_defs(db_, out, error) || !load_object_aux_maps(db_, out, error)) {
        return std::nullopt;
    }
    return out;
}

std::vector<int64_t> ContentDb::items_named(const std::string& name, std::string& error) const {
    std::vector<int64_t> out;
    Stmt q(db_, "SELECT guid FROM item WHERE name = ?1 COLLATE NOCASE ORDER BY guid LIMIT 8");
    if (!q.ok()) {
        error = "content.db item query failed: " + db_message(db_);
        return out;
    }
    sqlite3_bind_text(q.get(), 1, name.c_str(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    int step = SQLITE_OK;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) out.push_back(sqlite3_column_int64(q.get(), 0));
    if (step != SQLITE_DONE) error = "content.db item query failed: " + db_message(db_);
    return out;
}

std::optional<CraftStationData> ContentDb::craft_station_data(std::string& error) const {
    CraftStationData out;
    if (!load_craft_station_defs(db_, out, error) || !load_craft_station_assets(db_, out, error)) return std::nullopt;
    return out;
}

std::optional<RespawnData> ContentDb::respawn_data(std::string& error) const {
    RespawnData out;
    if (!load_attune_points(db_, out, error) || !load_respawn_points(db_, out, error) || !load_npc_respawns(db_, out, error)) return std::nullopt;
    return out;
}

std::optional<CraftRecipeData> ContentDb::craft_recipe_data(std::string& error) const {
    CraftRecipeData out;
    if (!load_craft_recipes(db_, out, error) || !load_craft_recipe_inputs(db_, out, error) || !load_craft_preds(db_, out, error) ||
        !load_craft_costs_and_sets(db_, out, error) || !load_craft_list_and_tags(db_, out, error) || !load_craft_rewards_and_xp(db_, out, error)) {
        return std::nullopt;
    }
    load_artisan_data(db_, out.artisan);
    return out;
}

}  // namespace aoc::worldd::content
