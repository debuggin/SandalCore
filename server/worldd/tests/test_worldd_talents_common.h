// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/captured_setup.h"
#include "bootstrap/character_setup.h"
#include "content/content_db.h"
#include "content_db_fixture.h"
#include "session/action_bar.h"
#include "session/talents.h"
#include "stats/stat_engine.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline bool contains(const std::string& s, const char* part) {
    return s.find(part) != std::string::npos;
}

namespace syn {
constexpr int64_t kArchetype = 11, kStamina = 12, kWeapon = 13;
constexpr int64_t kAdventuring = 2001, kWeaponType = 2002;
constexpr int64_t kClassTree = 1001, kUniversalTree = 1002, kWeaponTree = 1003, kOtherClassTree = 1004, kProfessionTree = 1005, kStageTree = 1006;
constexpr int64_t kClass = 7001, kOtherClass = 7002, kLine = 8001, kOtherLine = 8002;
constexpr int64_t rA = 3001, rB = 3002, rC = 3003, rD = 3004, rE = 3005, rF = 3006, rG1 = 3007, rG2 = 3008, rH = 3009, rI = 3010, rJ = 3011;
constexpr int64_t rU1 = 3101, rUbuy = 3102, rW = 3201, rOther = 3301, rProf = 3401, rWeaponMod = 3202;
constexpr int64_t kAbilityA = 9001, kAbilityB = 9002, kAbilityC = 9003, kAbilityProf = 9004, kAbilityG = 9005, kEffectE = 9101, kEffectU = 9102, kEffectW = 9103;
constexpr int64_t kStatStamina = 6064631747701440948ll;

inline aoc::worldd::content::SkillRankRow ability_rank(
    int64_t guid, int ord, int64_t ability, std::vector<aoc::worldd::content::SkillCost> costs) {
    aoc::worldd::content::SkillRankRow r;
    r.guid = guid;
    r.ord = ord;
    r.name = "rank" + std::to_string(guid);
    r.ability = ability;
    r.costs = std::move(costs);
    return r;
}

inline aoc::worldd::content::SkillRankRow timed_rank(
    int64_t guid, int64_t effect, double seconds, std::vector<aoc::worldd::content::SkillCost> costs) {
    aoc::worldd::content::SkillRankRow r;
    r.guid = guid;
    r.ord = 0;
    r.name = "rank" + std::to_string(guid);
    r.effect = effect;
    r.effect_duration = seconds;
    r.costs = std::move(costs);
    return r;
}

inline aoc::worldd::content::SkillRankRow mod_rank(
    int64_t guid, int64_t effect, std::vector<aoc::worldd::content::SkillCost> costs,
    std::vector<aoc::worldd::content::RankStatMod> mods) {
    aoc::worldd::content::SkillRankRow r;
    r.guid = guid;
    r.ord = 0;
    r.name = "rank" + std::to_string(guid);
    r.effect = effect;
    r.effect_infinite = true;
    r.costs = std::move(costs);
    r.mods = std::move(mods);
    return r;
}

inline aoc::worldd::content::SkillNodeRow node(
    int64_t guid, const char* name, std::vector<aoc::worldd::content::SkillRankRow> ranks,
    bool hotbar = true, int level = 0) {
    aoc::worldd::content::SkillNodeRow n;
    n.node = guid;
    n.name = name;
    n.skill = guid + 200;
    n.skill_name = name;
    n.level_requirement = level;
    n.auto_hotbar = hotbar;
    n.ranks = std::move(ranks);
    return n;
}

inline aoc::worldd::content::SkillNodeRow auto_node(
    int64_t guid, const char* name, int level, bool hotbar, std::vector<aoc::worldd::content::SkillRankRow> ranks) {
    aoc::worldd::content::SkillNodeRow n = node(guid, name, std::move(ranks), hotbar, level);
    n.auto_granted = true;
    return n;
}

inline aoc::worldd::content::SkillNodeRow gated_node(
    int64_t guid, const char* name, int points, std::vector<aoc::worldd::content::SkillRankRow> ranks, bool hotbar = true) {
    aoc::worldd::content::SkillNodeRow n = node(guid, name, std::move(ranks), hotbar, 0);
    n.point_requirement = points;
    return n;
}

inline aoc::worldd::content::SkillNodeRow req_node(
    int64_t guid, const char* name, std::vector<aoc::worldd::content::SkillRankRow> ranks,
    std::vector<int64_t> requires_skills, bool all = false) {
    aoc::worldd::content::SkillNodeRow n = node(guid, name, std::move(ranks), guid == (rB - 100), 0);
    n.all_required = all;
    n.requires_skills = std::move(requires_skills);
    return n;
}

inline aoc::worldd::content::SkillNodeRow excl_node(
    int64_t guid, const char* name, std::vector<aoc::worldd::content::SkillRankRow> ranks,
    std::vector<int64_t> exclusive) {
    aoc::worldd::content::SkillNodeRow n = node(guid, name, std::move(ranks), true, 0);
    n.exclusive_skills = std::move(exclusive);
    return n;
}

inline aoc::worldd::content::SkillTreeRow tree(
    int64_t guid, const char* name, const char* kind, int64_t type,
    std::vector<aoc::worldd::content::SkillNodeRow> nodes) {
    aoc::worldd::content::SkillTreeRow t;
    t.guid = guid;
    t.name = name;
    t.kind = kind;
    t.leveling_type_guid = type;
    int ord = 0;
    for (aoc::worldd::content::SkillNodeRow& n : nodes) n.ord = ord++;
    t.nodes = std::move(nodes);
    return t;
}

inline aoc::worldd::content::SkillTreeRow class_tree(
    int64_t guid, const char* name, int64_t cls, std::vector<aoc::worldd::content::SkillNodeRow> nodes) {
    aoc::worldd::content::SkillTreeRow t = tree(guid, name, "class", kAdventuring, std::move(nodes));
    t.class_guid = cls;
    return t;
}

inline aoc::worldd::content::SkillTreeRow weapon_tree(
    int64_t guid, const char* name, int64_t line, std::vector<aoc::worldd::content::SkillNodeRow> nodes) {
    aoc::worldd::content::SkillTreeRow t = tree(guid, name, "weapon", kWeaponType, std::move(nodes));
    t.weapon_line_guid = line;
    return t;
}

inline const std::vector<aoc::worldd::content::SkillCost> one = {{kArchetype, 1}};

inline aoc::worldd::content::TalentData data() {
    namespace content = aoc::worldd::content;
    content::TalentData d;
    d.point_types = {{content::kPointArchetype, kArchetype}, {content::kPointStamina, kStamina}, {content::kPointWeapon, kWeapon}};
    for (int level = 2; level <= 25; ++level) {
        d.points[kAdventuring][level] = {{kArchetype, 1}, {kStamina, 1}};
        d.points[kWeaponType][level] = {{kWeapon, 1}};
    }
    std::vector<content::SkillNodeRow> class_nodes;
    class_nodes.push_back(auto_node(rA - 100, "A_auto", 1, true, {ability_rank(rA, 0, kAbilityA, {})}));
    class_nodes.push_back(req_node(rB - 100, "B_needs_A", {ability_rank(rB, 0, kAbilityB, one)}, {rA - 100 + 200}));
    class_nodes.push_back(excl_node(rC - 100, "C_excl_D", {ability_rank(rC, 0, kAbilityC, one)}, {rD - 100 + 200}));
    class_nodes.push_back(excl_node(rD - 100, "D_excl_C", {ability_rank(rD, 0, 0, one)}, {rC - 100 + 200}));
    class_nodes.push_back(gated_node(rE - 100, "E_two_spent", 2, {mod_rank(rE, kEffectE, one, {{kStatStamina, "MaxStamina", 7.5}})}, true));
    class_nodes.push_back(node(rF - 100, "F_level5", {ability_rank(rF, 0, 0, one)}, false, 5));
    class_nodes.push_back(node(rG1 - 100, "G_two_ranks", {ability_rank(rG1, 0, kAbilityG, one), ability_rank(rG2, 1, 0, {{kArchetype, 2}})}, true, 0));
    class_nodes.push_back(req_node(rH - 100, "H_all_of_B_C", {ability_rank(rH, 0, 0, one)}, {rB - 100 + 200, rC - 100 + 200}, true));
    class_nodes.push_back(req_node(rI - 100, "I_any_of_B_E", {ability_rank(rI, 0, 0, one)}, {rB - 100 + 200, rE - 100 + 200}, false));
    class_nodes.push_back(req_node(rJ - 100, "J_empty_requirement", {ability_rank(rJ, 0, 0, one)}, {0}, true));
    class_nodes.push_back(auto_node(1, "A_listed_again", 1, true, {ability_rank(rA, 0, kAbilityA, {})}));
    d.trees.push_back(class_tree(kClassTree, "ClassTree", kClass, std::move(class_nodes)));
    d.trees.back().nodes.back().skill = rA - 100 + 200;
    d.trees.push_back(tree(kUniversalTree, "Universal", "universal", kAdventuring,
                           {auto_node(rU1 - 100, "U_auto_l2", 2, true, {timed_rank(rU1, kEffectU, 1.0, {})}),
                            node(rUbuy - 100, "U_buy", {timed_rank(rUbuy, kEffectU, 1.0, {{kStamina, 2}})}, true, 0)}));
    d.trees.push_back(weapon_tree(kWeaponTree, "Weapon_Test", kLine,
                                  {node(rW - 100, "W_buy", {timed_rank(rW, kEffectW, 0.1, {{kWeapon, 2}})}, true, 0),
                                   node(rWeaponMod - 100, "W_mod", {mod_rank(rWeaponMod, kEffectW, {{kWeapon, 1}}, {{kStatStamina, "MaxStamina", 3}})}, false, 0)}));
    d.trees.push_back(class_tree(kOtherClassTree, "OtherClassTree", kOtherClass,
                                 {node(rOther - 100, "Other_buy", {ability_rank(rOther, 0, 0, one)}, true, 0),
                                  auto_node(rOther - 99, "Other_auto", 0, true, {ability_rank(rOther + 1, 0, 9900, {})})}));
    d.trees.push_back(tree(kProfessionTree, "Mining_Test", "profession", 2003,
                           {auto_node(rProf - 100, "Prof_auto", 0, false, {ability_rank(rProf, 0, kAbilityProf, {})})}));
    d.trees.push_back(tree(kStageTree, "Stage", "other", kAdventuring,
                           {node(rOther + 50, "Stage_buy", {ability_rank(rOther + 51, 0, 0, one)}, true, 0)}));
    return d;
}
}  // namespace syn

inline std::optional<aoc::worldd::talents::Catalog> synthetic_catalog() {
    std::string error;
    auto c = aoc::worldd::talents::Catalog::from(syn::data(), error);
    if (!c) std::printf("  [FAIL] the synthetic catalog does not build: %s\n", error.c_str());
    return c;
}

inline aoc::protocol::repl::DesignDataId id_of(int64_t guid, int64_t type) {
    aoc::protocol::repl::DesignDataId d;
    d.guid = guid;
    d.type_id = type;
    return d;
}

struct Entry {
    int64_t rank, tree;
    bool    purchase;
};

inline aoc::protocol::repl::SkillBuild build_of(std::initializer_list<Entry> entries) {
    aoc::protocol::repl::SkillBuild b;
    for (const Entry& e : entries) {
        aoc::protocol::repl::SkillRankChange c;
        c.rank = id_of(e.rank, aoc::worldd::content::kTypeSkillRank);
        c.tree = id_of(e.tree, aoc::worldd::content::kTypeSkillTree);
        c.leveling_type = id_of(syn::kAdventuring, aoc::worldd::content::kTypeLevelingTypeDef);
        c.purchase = e.purchase;
        b.changes.push_back(c);
    }
    return b;
}

inline bool refused_with(const aoc::worldd::talents::Plan& p, const char* part) {
    return !p.ok && contains(p.refusal, part);
}

inline aoc::worldd::talents::Plan run(aoc::worldd::talents::Book& book, std::initializer_list<Entry> entries) {
    aoc::worldd::talents::Plan p = book.plan(build_of(entries));
    if (p.ok) book.apply(p);
    return p;
}

struct RealWorld {
    std::filesystem::path                        dir;
    std::optional<aoc::worldd::content::ContentDb> db;
    std::optional<aoc::worldd::talents::Catalog>   catalog;
    RealWorld() {
        dir = content_fixture::make_temp_dir("worldd_talents_");
        const std::filesystem::path path = dir / "content.db";
        if (!content_fixture::make_db(path)) std::exit(2);
        std::string error;
        db = aoc::worldd::content::ContentDb::open(path.string(), error);
        if (!db) {
            std::printf("  [FAIL] the fixture does not open: %s\n", error.c_str());
            std::exit(2);
        }
        auto data = db->talent_data(error);
        if (!data) {
            std::printf("  [FAIL] talent_data: %s\n", error.c_str());
            std::exit(2);
        }
        catalog = aoc::worldd::talents::Catalog::from(std::move(*data), error);
        if (!catalog) {
            std::printf("  [FAIL] the catalog: %s\n", error.c_str());
            std::exit(2);
        }
    }
    ~RealWorld() {
        db.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

inline constexpr int64_t kBardClass = 108640158570105ll;
inline constexpr int64_t kTankClass = 108640201692798ll;
inline constexpr int64_t kScepterCombo = 6064631845843960261ll;
inline constexpr int64_t kBookCombo = 6064631012536680449ll;

void run_talents_part2();
void run_talents_part3();
void run_talents_part4();
