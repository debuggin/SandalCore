// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 SandalCore contributors.
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "content/content_db_internal.h"

namespace aoc::worldd::content {

namespace {

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
    d.kind = json_str(c, "t");
    d.op = json_str(c, "op");
    d.value = json_int(c, "v");
    d.quest = json_int(c, "q");
    d.quest_query = json_str(c, "qq");
    d.objective = json_int(c, "o");
    d.objective_query = json_str(c, "oq");
    d.variable = json_str(c, "var");
    d.currency = json_int(c, "cur");
    d.expression = json_str(c, "x");
    d.right_variable = json_str(c, "rv");
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

bool parse_dialogue_requirement(const std::string& text, DialogueRequirement& out) {
    out = DialogueRequirement{};
    if (text.empty()) return true;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return false;
    return parse_requirement_object(j, out);
}

bool parse_dialogue_actions(const std::string& text, std::vector<DialogueActionRow>& out) {
    out.clear();
    if (text.empty()) return true;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return false;
    for (const auto& a : j) {
        if (!a.is_object()) return false;
        DialogueActionRow r;
        r.action = json_str(a, "a");
        r.window = json_str(a, "w");
        r.vendor = json_int(a, "vd");
        r.response = json_str(a, "r");
        r.access_config = json_int(a, "sc");
        r.quest = json_int(a, "q");
        r.objective = json_int(a, "o");
        json_lenient_ints(a, "rw", r.rewards);
        out.push_back(std::move(r));
    }
    return true;
}

bool fail_dialogue(sqlite3* db, const char* table, std::string& error) {
    error = std::string("content.db ") + table + " query failed: " + db_message(db) +
            " (the dialogue tables are contentc version 17's: rebuild the file with this checkout's contentc)";
    return false;
}

bool unread_row(const char* table, int64_t guid, std::string& error) {
    error = std::string("content.db ") + table + " row " + std::to_string(guid) + " does not read";
    return false;
}

bool load_dialogue_roots(sqlite3* db, DialogueData& out, std::string& error) {
    Stmt q(db, "SELECT guid, top_node FROM dialogue_root");
    if (!q.ok()) return fail_dialogue(db, "dialogue_root", error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        if (!column_is_int(q.get(), 0) || !column_is_int(q.get(), 1)) return unread_row("dialogue_root", 0, error);
        out.roots[sqlite3_column_int64(q.get(), 0)] = sqlite3_column_int64(q.get(), 1);
    }
    return step == SQLITE_DONE || fail_dialogue(db, "dialogue_root", error);
}

bool load_dialogue_nodes(sqlite3* db, DialogueData& out, std::string& error) {
    Stmt with_text(db, "SELECT guid, name, node_type, exhaust, quest_guid, on_enter, on_exit, has_text FROM dialogue_node");
    const bool words_known = with_text.ok();
    Stmt without_text(db, "SELECT guid, name, node_type, exhaust, quest_guid, on_enter, on_exit FROM dialogue_node");
    const Stmt& q = words_known ? with_text : without_text;
    if (!q.ok()) return fail_dialogue(db, "dialogue_node", error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 4)) return unread_row("dialogue_node", 0, error);
        DialogueNodeRow n;
        n.guid = sqlite3_column_int64(s, 0);
        n.name = column_text(s, 1);
        n.type = column_text(s, 2);
        n.exhaust = column_text(s, 3);
        n.quest = sqlite3_column_int64(s, 4);
        if (words_known) n.has_text = sqlite3_column_int64(s, 7) != 0;
        if (!parse_dialogue_actions(column_text(s, 5), n.on_enter) || !parse_dialogue_actions(column_text(s, 6), n.on_exit)) {
            return unread_row("dialogue_node", n.guid, error);
        }
        out.nodes[n.guid] = std::move(n);
    }
    return step == SQLITE_DONE || fail_dialogue(db, "dialogue_node", error);
}

bool load_dialogue_choices(sqlite3* db, DialogueData& out, std::string& error) {
    Stmt q(db, "SELECT node_guid, idx, next_node, requirement, actions FROM dialogue_choice ORDER BY node_guid, idx");
    if (!q.ok()) return fail_dialogue(db, "dialogue_choice", error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 1) || !column_is_int(s, 2)) return unread_row("dialogue_choice", 0, error);
        const int64_t node = sqlite3_column_int64(s, 0);
        const auto it = out.nodes.find(node);
        if (it == out.nodes.end() || sqlite3_column_int64(s, 1) != static_cast<int64_t>(it->second.choices.size())) {
            return unread_row("dialogue_choice", node, error);
        }
        DialogueChoiceRow c;
        c.next = sqlite3_column_int64(s, 2);
        if (!parse_dialogue_requirement(column_text(s, 3), c.requirement) || !parse_dialogue_actions(column_text(s, 4), c.actions)) {
            return unread_row("dialogue_choice", node, error);
        }
        it->second.choices.push_back(std::move(c));
    }
    return step == SQLITE_DONE || fail_dialogue(db, "dialogue_choice", error);
}

bool load_dialogue_quests_and_npcs(sqlite3* db, DialogueData& out, std::string& error) {
    {
        Stmt q(db, "SELECT guid, name, disabled, repeatable, activation FROM dialogue_quest");
        if (!q.ok()) return fail_dialogue(db, "dialogue_quest", error);
        int step = SQLITE_ERROR;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 2) || !column_is_int(s, 3)) return unread_row("dialogue_quest", 0, error);
            DialogueQuestRow r;
            r.guid = sqlite3_column_int64(s, 0);
            r.name = column_text(s, 1);
            r.disabled = sqlite3_column_int64(s, 2) != 0;
            r.repeatable = sqlite3_column_int64(s, 3) != 0;
            if (!parse_dialogue_requirement(column_text(s, 4), r.activation)) return unread_row("dialogue_quest", r.guid, error);
            out.quests[r.guid] = std::move(r);
        }
        if (step != SQLITE_DONE) return fail_dialogue(db, "dialogue_quest", error);
    }
    Stmt q(db, "SELECT source_kind, source_guid, root_guid FROM npc_dialogue");
    if (!q.ok()) return fail_dialogue(db, "npc_dialogue", error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 1) || !column_is_int(s, 2)) return unread_row("npc_dialogue", 0, error);
        const std::string kind = column_text(s, 0);
        (kind == "instance" ? out.instance_roots : out.asset_roots)[sqlite3_column_int64(s, 1)] = sqlite3_column_int64(s, 2);
    }
    return step == SQLITE_DONE || fail_dialogue(db, "npc_dialogue", error);
}

}  // namespace

std::optional<DialogueData> ContentDb::dialogue_data(std::string& error) const {
    DialogueData out;
    if (!load_dialogue_roots(db_, out, error) || !load_dialogue_nodes(db_, out, error) || !load_dialogue_choices(db_, out, error) ||
        !load_dialogue_quests_and_npcs(db_, out, error)) {
        return std::nullopt;
    }
    return out;
}

namespace {

bool parse_guid_list(const std::string& text, std::vector<int64_t>& out) {
    out.clear();
    if (text.empty()) return true;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return false;
    for (const auto& v : j) {
        if (!v.is_number_integer()) return false;
        out.push_back(v.get<int64_t>());
    }
    return true;
}

bool parse_quest_action_items(const nlohmann::json& a, std::vector<std::pair<int64_t, int64_t>>& items) {
    const auto it = a.find("it");
    if (it == a.end()) return true;
    if (!it->is_array()) return false;
    for (const auto& pair : *it) {
        if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number_integer() || !pair[1].is_number_integer()) return false;
        items.emplace_back(pair[0].get<int64_t>(), pair[1].get<int64_t>());
    }
    return true;
}

bool parse_quest_actions_json(const nlohmann::json& j, std::vector<QuestAction>& out) {
    out.clear();
    if (!j.is_array()) return false;
    for (const auto& a : j) {
        if (!a.is_object()) return false;
        QuestAction r;
        r.action = json_str(a, "a");
        r.objective = json_int(a, "o");
        r.quest = json_int(a, "q");
        r.variable = json_str(a, "n");
        r.value = json_int(a, "v");
        r.conditional = json_flag(a, "c");
        if (const auto it = a.find("r"); it != a.end() && !parse_requirement_object(*it, r.rule)) return false;
        r.reward_table = json_int(a, "rw");
        if (!parse_quest_action_items(a, r.items)) return false;
        r.text = json_str(a, "t");
        json_lenient_ints(a, "sp", r.spawns);
        out.push_back(std::move(r));
    }
    return true;
}

bool parse_quest_start_actions(const std::string& text, std::vector<QuestStartAction>& out) {
    out.clear();
    if (text.empty()) return true;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return false;
    return parse_quest_actions_json(j, out);
}

bool json_ints(const nlohmann::json& o, const char* key, std::vector<int64_t>& out) {
    out.clear();
    const auto it = o.find(key);
    if (it == o.end()) return true;
    if (!it->is_array()) return false;
    for (const auto& v : *it) {
        if (!v.is_number_integer()) return false;
        out.push_back(v.get<int64_t>());
    }
    return true;
}

bool json_strings(const nlohmann::json& o, const char* key, std::vector<std::string>& out) {
    out.clear();
    const auto it = o.find(key);
    if (it == o.end()) return true;
    if (!it->is_array()) return false;
    for (const auto& v : *it) {
        if (!v.is_string()) return false;
        out.push_back(v.get<std::string>());
    }
    return true;
}

bool json_actions(const nlohmann::json& o, const char* key, std::vector<QuestAction>& out) {
    out.clear();
    const auto it = o.find(key);
    return it == o.end() || parse_quest_actions_json(*it, out);
}

bool json_sources(const nlohmann::json& o, const char* key, std::vector<QuestSource>& out) {
    out.clear();
    const auto it = o.find(key);
    if (it == o.end()) return true;
    if (!it->is_array()) return false;
    for (const auto& s : *it) {
        if (!s.is_object()) return false;
        QuestSource q;
        q.kind = json_str(s, "k");
        q.instance = json_int(s, "i");
        q.node = json_int(s, "n");
        q.interactable = json_int(s, "x");
        q.item = json_int(s, "it");
        out.push_back(std::move(q));
    }
    return true;
}

bool parse_quest_detail(const std::string& text, QuestRow& q) {
    if (text.empty()) return true;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    if (!json_actions(j, "ok", q.on_success) || !json_actions(j, "fail", q.on_fail) || !json_actions(j, "tick", q.on_tick)) {
        return false;
    }
    q.timeout = json_num(j, "to");
    q.succeed_on_timeout = json_flag(j, "so");
    q.giver_registration = json_int(j, "gr");
    return true;
}

bool json_locations(const nlohmann::json& o, const char* key, std::vector<QuestLocation>& out) {
    out.clear();
    const auto it = o.find(key);
    if (it == o.end()) return true;
    if (!it->is_array()) return false;
    for (const auto& l : *it) {
        if (!l.is_object()) return false;
        QuestLocation q;
        q.x = json_num(l, "x");
        q.y = json_num(l, "y");
        q.z = json_num(l, "z");
        q.radius = json_num(l, "r");
        q.completes = json_flag(l, "done");
        q.hidden = json_flag(l, "hid");
        q.display = json_str(l, "dt");
        q.id = json_int(l, "id");
        out.push_back(q);
    }
    return true;
}

bool parse_objective_item_rewards(const nlohmann::json& j, QuestObjectiveRow& o) {
    const auto qi = j.find("qi");
    if (qi == j.end()) return true;
    if (!qi->is_array()) return false;
    for (const auto& e : *qi) {
        if (!e.is_object()) return false;
        QuestObjectiveRow::ItemReward r;
        r.table = json_int(e, "t");
        r.tag_query = json_flag(e, "tq");
        if (!json_ints(e, "inst", r.instances) || !json_ints(e, "sets", r.sets)) return false;
        o.item_rewards.push_back(std::move(r));
    }
    return true;
}

bool parse_objective_markers(const nlohmann::json& j, QuestObjectiveRow& o) {
    const auto mk = j.find("mk");
    if (mk == j.end()) return true;
    if (!mk->is_object()) return false;
    if (const auto ov = mk->find("ov"); ov != mk->end()) {
        if (!ov->is_object() || !json_locations(*ov, "l", o.override_locations)) return false;
        o.override_markers = true;
    }
    if (!json_ints(*mk, "pt", o.trackers)) return false;
    o.no_dynamic_markers = json_flag(*mk, "nd");
    const auto cl = mk->find("cl");
    if (cl == mk->end()) return true;
    if (!cl->is_object()) return false;
    o.cluster.epsilon = json_num(*cl, "e");
    o.cluster.min_radius = json_num(*cl, "r0");
    o.cluster.max_radius = json_num(*cl, "r1");
    o.cluster.merge = json_num(*cl, "m");
    o.cluster.min_points = static_cast<int>(json_int(*cl, "n"));
    o.cluster.max_clusters = static_cast<int>(json_int(*cl, "c"));
    o.cluster.default_type = json_str(*cl, "d");
    o.cluster.weighted = json_flag(*cl, "w");
    return true;
}

bool parse_objective_data_collections(const nlohmann::json& d, QuestObjectiveRow& o) {
    return json_ints(d, "items", o.items) && json_strings(d, "tags", o.tags) && json_strings(d, "rar", o.rarities) &&
           json_locations(d, "loc", o.locations) && json_ints(d, "ct", o.crate_types) && json_ints(d, "ci", o.crate_items) &&
           json_ints(d, "ab", o.abilities);
}

bool parse_objective_data_block(const nlohmann::json& j, QuestObjectiveRow& o) {
    const auto d = j.find("d");
    if (d == j.end()) return true;
    if (!d->is_object()) return false;
    if (!json_ints(*d, "sets", o.kill_sets) || !json_ints(*d, "inst", o.kill_instances)) return false;
    o.advanced = json_flag(*d, "adv");
    o.elite = static_cast<int>(json_int(*d, "elite"));
    o.kill_owner = json_flag(*d, "owner");
    o.use_context = json_flag(*d, "ctx");
    o.tag_query = json_flag(*d, "tq");
    if (!json_sources(*d, "src", o.sources) || !json_ints(*d, "comp", o.components) || !json_ints(*d, "actor", o.actors)) {
        return false;
    }
    o.any_source = json_flag(*d, "any");
    o.collect_type = json_str(*d, "cty");
    if (!parse_objective_data_collections(*d, o)) return false;
    o.area_map = json_flag(*d, "area");
    o.crate = json_flag(*d, "crate");
    o.quest_variable = json_str(*d, "qv");
    o.inc_variable = json_flag(*d, "inc");
    o.notify = json_str(*d, "nt");
    o.recipe = json_int(*d, "recipe");
    const auto rule = d->find("rule");
    return rule == d->end() || parse_requirement_object(*rule, o.rule);
}

bool parse_objective_detail(const std::string& text, QuestObjectiveRow& o) {
    if (text.empty()) return true;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    o.show_dialogue = json_flag(j, "sd");
    o.timeout = json_num(j, "to");
    o.succeed_on_timeout = json_flag(j, "so");
    if (const auto it = j.find("ar"); it != j.end() && !parse_requirement_object(*it, o.activation)) return false;
    if (!json_ints(j, "rw", o.rewards)) return false;
    if (const auto on = j.find("on"); on != j.end()) {
        if (!on->is_object() || !json_actions(*on, "start", o.on_start) || !json_actions(*on, "tick", o.on_tick) ||
            !json_actions(*on, "ok", o.on_success) || !json_actions(*on, "fail", o.on_fail)) {
            return false;
        }
    }
    return parse_objective_item_rewards(j, o) && parse_objective_markers(j, o) && parse_objective_data_block(j, o);
}

bool fail_quest(sqlite3* db, const char* table, std::string& error) {
    error = std::string("content.db ") + table + " query failed: " + db_message(db) +
            " (the quest tables are contentc version 21's: rebuild the file with this checkout's contentc)";
    return false;
}

bool cols_are_int(sqlite3_stmt* s, std::initializer_list<int> cols) {
    for (const int c : cols) {
        if (!column_is_int(s, c)) return false;
    }
    return true;
}

bool load_quest_rows(sqlite3* db, QuestData& out, std::string& error) {
    Stmt q(db,
           "SELECT guid, name, flavor, locality, level, can_abandon, repeatable, disabled, default_activate, "
           "activation, start_actions, rewards, detail FROM quest");
    if (!q.ok()) return fail_quest(db, "quest", error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!cols_are_int(s, {0, 4, 5, 6, 7, 8})) return unread_row("quest", 0, error);
        QuestRow r;
        r.guid = sqlite3_column_int64(s, 0);
        r.name = column_text(s, 1);
        r.flavor = column_text(s, 2);
        r.locality = column_text(s, 3);
        r.level = static_cast<int>(sqlite3_column_int64(s, 4));
        r.can_abandon = sqlite3_column_int64(s, 5) != 0;
        r.repeatable = sqlite3_column_int64(s, 6) != 0;
        r.disabled = sqlite3_column_int64(s, 7) != 0;
        r.default_activate = sqlite3_column_int64(s, 8) != 0;
        if (!parse_dialogue_requirement(column_text(s, 9), r.activation) ||
            !parse_quest_start_actions(column_text(s, 10), r.start_actions) || !parse_guid_list(column_text(s, 11), r.rewards) ||
            !parse_quest_detail(column_text(s, 12), r)) {
            return unread_row("quest", r.guid, error);
        }
        out.by_name[r.name] = r.guid;
        out.quests[r.guid] = std::move(r);
    }
    return step == SQLITE_DONE || fail_quest(db, "quest", error);
}

bool load_quest_objectives(sqlite3* db, QuestData& out, std::string& error) {
    Stmt q(db,
           "SELECT quest_guid, idx, guid, name, obj_type, completion_count, default_awake, default_hidden, "
           "completes_quest, optional, detail FROM quest_objective ORDER BY quest_guid, idx");
    if (!q.ok()) return fail_quest(db, "quest_objective", error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!cols_are_int(s, {0, 1, 2, 5, 6, 7, 8, 9})) return unread_row("quest_objective", 0, error);
        const auto it = out.quests.find(sqlite3_column_int64(s, 0));
        if (it == out.quests.end()) return unread_row("quest_objective", sqlite3_column_int64(s, 0), error);
        QuestObjectiveRow o;
        o.guid = sqlite3_column_int64(s, 2);
        o.name = column_text(s, 3);
        o.type = column_text(s, 4);
        o.count = sqlite3_column_int64(s, 5);
        o.awake = sqlite3_column_int64(s, 6) != 0;
        o.hidden = sqlite3_column_int64(s, 7) != 0;
        o.completes = sqlite3_column_int64(s, 8) != 0;
        o.optional = sqlite3_column_int64(s, 9) != 0;
        if (!parse_objective_detail(column_text(s, 10), o)) return unread_row("quest_objective", o.guid, error);
        it->second.objectives.push_back(std::move(o));
    }
    return step == SQLITE_DONE || fail_quest(db, "quest_objective", error);
}

bool load_quest_offers_and_accepts(sqlite3* db, QuestData& out, std::string& error) {
    {
        Stmt q(db,
               "SELECT source_kind, source_guid, quest_guid, dynamic_node FROM npc_offered_quest ORDER BY source_kind, "
               "source_guid, idx");
        if (!q.ok()) return fail_quest(db, "npc_offered_quest", error);
        int step = SQLITE_ERROR;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!cols_are_int(s, {1, 2, 3})) return unread_row("npc_offered_quest", 0, error);
            const std::string kind = column_text(s, 0);
            (kind == "instance" ? out.instance_offers : out.asset_offers)[sqlite3_column_int64(s, 1)].push_back(
                {sqlite3_column_int64(s, 2), sqlite3_column_int64(s, 3)});
        }
        if (step != SQLITE_DONE) return fail_quest(db, "npc_offered_quest", error);
    }
    Stmt q(db, "SELECT node_guid, quest_guid, on_success, objectives FROM dialogue_accept");
    if (!q.ok()) return fail_quest(db, "dialogue_accept", error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!cols_are_int(s, {0, 1, 2})) return unread_row("dialogue_accept", 0, error);
        QuestAcceptRow r;
        r.node = sqlite3_column_int64(s, 0);
        r.quest = sqlite3_column_int64(s, 1);
        r.on_success = sqlite3_column_int64(s, 2);
        if (!parse_guid_list(column_text(s, 3), r.objectives)) return unread_row("dialogue_accept", r.node, error);
        if (r.quest == 0) {
            out.confirms[r.node] = r.on_success;
        } else {
            out.accepts[r.node] = std::move(r);
        }
    }
    return step == SQLITE_DONE || fail_quest(db, "dialogue_accept", error);
}

bool load_quest_turnins(sqlite3* db, QuestData& out, std::string& error) {
    Stmt q(db, "SELECT node_guid, quest_guid, on_success, objectives FROM dialogue_turnin");
    if (!q.ok()) return true;
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!cols_are_int(s, {0, 1, 2})) return unread_row("dialogue_turnin", 0, error);
        QuestAcceptRow r;
        r.node = sqlite3_column_int64(s, 0);
        r.quest = sqlite3_column_int64(s, 1);
        r.on_success = sqlite3_column_int64(s, 2);
        if (!parse_guid_list(column_text(s, 3), r.objectives)) return unread_row("dialogue_turnin", r.node, error);
        if (r.quest != 0) out.turn_ins[r.node] = std::move(r);
    }
    return step == SQLITE_DONE || fail_quest(db, "dialogue_turnin", error);
}

bool load_int_vec_map(sqlite3* db, const char* table, const char* sql, std::map<int64_t, std::vector<int64_t>>& into, std::string& error) {
    Stmt q(db, sql);
    if (!q.ok()) return fail_quest(db, table, error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 1)) return unread_row(table, 0, error);
        into[sqlite3_column_int64(s, 0)].push_back(sqlite3_column_int64(s, 1));
    }
    return step == SQLITE_DONE || fail_quest(db, table, error);
}

bool load_quest_aux_links(sqlite3* db, QuestData& out, std::string& error) {
    if (!load_int_vec_map(db, "zoi_quest", "SELECT zoi_guid, quest_guid FROM zoi_quest ORDER BY zoi_guid, idx", out.zoi_quests, error) ||
        !load_int_vec_map(db, "quest_kill_set", "SELECT instance_guid, set_guid FROM quest_kill_set ORDER BY instance_guid, set_guid",
                          out.kill_sets, error) ||
        !load_int_vec_map(db, "shrine_icd", "SELECT attune_guid, icd_guid FROM shrine_icd ORDER BY attune_guid, icd_guid", out.shrine_icds,
                          error)) {
        return false;
    }
    {
        Stmt q(db, "SELECT item_guid, quest_guid, kind FROM item_quest ORDER BY item_guid, quest_guid, kind");
        if (!q.ok()) return fail_quest(db, "item_quest", error);
        int step = SQLITE_ERROR;
        while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
            sqlite3_stmt* s = q.get();
            if (!column_is_int(s, 0) || !column_is_int(s, 1)) return unread_row("item_quest", 0, error);
            out.item_quests[sqlite3_column_int64(s, 0)].push_back({sqlite3_column_int64(s, 1), column_text(s, 2)});
        }
        if (step != SQLITE_DONE) return fail_quest(db, "item_quest", error);
    }
    Stmt q(db, "SELECT item_guid, crate_guid FROM item_crate");
    if (!q.ok()) return fail_quest(db, "item_crate", error);
    int step = SQLITE_ERROR;
    while ((step = sqlite3_step(q.get())) == SQLITE_ROW) {
        sqlite3_stmt* s = q.get();
        if (!column_is_int(s, 0) || !column_is_int(s, 1)) return unread_row("item_crate", 0, error);
        out.item_crate[sqlite3_column_int64(s, 0)] = sqlite3_column_int64(s, 1);
    }
    return step == SQLITE_DONE || fail_quest(db, "item_crate", error);
}

}  // namespace

std::optional<QuestData> ContentDb::quest_data(std::string& error) const {
    QuestData out;
    if (!load_quest_rows(db_, out, error) || !load_quest_objectives(db_, out, error) || !load_quest_offers_and_accepts(db_, out, error) ||
        !load_quest_turnins(db_, out, error) || !load_quest_aux_links(db_, out, error)) {
        return std::nullopt;
    }
    return out;
}

}  // namespace aoc::worldd::content
