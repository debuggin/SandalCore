// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "content/content_types_ability.h"

#include <array>

namespace aoc::worldd::content {

struct CertificationRow {
    int64_t     guid = 0;
    std::string name;
    int         tier = 0;
    int         min_level = 1;
    int         max_level = 10;
    bool        limited = false;
    int         max_allowable = 0;
};

struct ProfessionRow {
    int64_t       guid = 0;
    std::string   name;
    std::string   category;
    int64_t       leveling_type_guid = 0;
    int64_t       skill_tree_guid = 0;
    LevelingType  leveling;
};

struct CertificationActionRow {
    int64_t     action_guid = 0;
    std::string name;
    int64_t     profession_guid = 0;
    bool        promote = true;
};

struct ProfessionData {
    std::vector<ProfessionRow>                professions;
    std::vector<CertificationRow>             certifications;
    std::map<int64_t, CertificationActionRow> actions;
};

struct CurrencyTierRow {
    int64_t     guid = 0;
    std::string name;
    int64_t     max_base_quantity = 0;
};

struct CurrencyDefRow {
    int64_t     guid = 0;
    std::string name;
    int64_t     tier = 0;
    int         ord = 0;
    int64_t     item = 0;
    int64_t     rate = 0;
};

struct CurrencyData {
    std::vector<CurrencyTierRow> tiers;
    std::vector<CurrencyDefRow>  defs;
};

struct GatherDefRow {
    int64_t     guid = 0;
    std::string name;
    double      empty_duration = 0;
    double      override_respawn = 0;
    int64_t     interaction_guid = 0;
    double      interact_radius = 0;
    double      interact_tolerance = 0;
    bool        allow_in_combat = true;
    int64_t     profession_guid = 0;
    int         tool_slot = 0;
    int64_t     cert_guid = 0;
    int         cert_tier = 0;
    double      base_duration = 0;
    int64_t     duration_expr = 0;
    int64_t     durability_expr = 0;
    int64_t     reward_guid = 0;
};

struct GatherToolRow {
    int64_t     item_guid = 0;
    std::string item_name;
    int64_t     profession_guid = 0;
    int64_t     cert_guid = 0;
    int         cert_tier = 0;
    int         tool_slot = 0;
    int64_t     required_profession = 0;
    int64_t     required_cert = 0;
};

struct GatherData {
    std::map<int64_t, GatherDefRow>  defs;
    std::map<int64_t, GatherToolRow> tools;
    std::map<int64_t, int64_t>       quality_expr;
    std::map<int64_t, int64_t>       durability_expr;
};

inline constexpr int kFoliageInfancy = 0, kFoliageJuvenile = 1, kFoliageAdult = 2, kFoliageEmpty = 3;

struct FoliageDefRow {
    int64_t     guid = 0;
    std::string name;
    double      empty_duration = 0;
    int64_t     dynamic_guid = 0;
    int         paint_state = kFoliageAdult;
    int         gatherable_state = kFoliageAdult, min_state = kFoliageInfancy, max_state = kFoliageAdult;
    int64_t     state_guids[3] = {0, 0, 0};
};

struct FoliageStateRow {
    int64_t     guid = 0;
    std::string name;
    double      lifecycle_duration = 0;
    int64_t     interaction_guid = 0;
    // Negative sentinel (-1) falls back to the interaction definition's default range/angle.
    double      override_distance = -1;
    double      override_angle = -1;
    double      collision_radius = 0;
};

struct GatherInteractionRow {
    int64_t     guid = 0;
    std::string name;
    int         target_state = kFoliageEmpty;
    int64_t     profession_guid = 0;
    int64_t     cert_guid = 0;
    int         cert_tier = 0;
    int         tool_slot = -1;
    double      base_duration = 0;
    int64_t     duration_expr = 0;
    int64_t     durability_expr = 0;
    int64_t     reward_guid = 0;
    int64_t     interaction_guid = 0;
    double      interact_radius = 0, interact_tolerance = 0;
    bool        allow_in_combat = true;
};

struct GatherInteractionDefRow {
    int64_t     guid = 0;
    std::string name;
    int64_t     gather_guid = 0;
    double      interact_radius = 0, interact_tolerance = 0, discovery_distance = 0;
    bool        allow_in_combat = true;
};

struct FoliageData {
    std::map<int64_t, FoliageDefRow>           defs;
    // Multiple foliage records can share a name (e.g. Stone_Limestone_Large_2).
    std::multimap<std::string, int64_t>        defs_by_name;
    std::map<int64_t, FoliageStateRow>         states;
    std::map<int64_t, GatherInteractionRow>    interactions;
    std::map<int64_t, GatherInteractionDefRow> interaction_defs;
};

inline constexpr const char* kPointArchetype = "Combat_Archetype";
inline constexpr const char* kPointStamina = "Combat_Stamina";
inline constexpr const char* kPointWeapon = "Combat_Weapon";

struct SkillCost {
    int64_t point_type = 0;
    int64_t quantity = 0;
};

struct SkillRankRow {
    int64_t                  guid = 0;
    int                      ord = 0;
    std::string              name;
    int64_t                  ability = 0;
    std::string              ability_name;
    int64_t                  effect = 0;
    std::string              effect_name;
    bool                     effect_infinite = false;
    double                   effect_duration = 0;
    std::vector<SkillCost>   costs;
    std::vector<RankStatMod> mods;
};

struct SkillNodeRow {
    int64_t                   node = 0;
    int                       ord = 0;
    std::string               name;
    int64_t                   skill = 0;
    std::string               skill_name;
    int                       level_requirement = 0;
    int                       point_requirement = 0;
    bool                      auto_granted = false;
    bool                      auto_hotbar = false;
    bool                      all_required = false;
    // Slot nodes repeat with the same node/ord per option; opt is the option index in the slot.
    int                       opt = 0;
    int64_t                   slot = 0;
    std::vector<int64_t>      requires_skills;
    std::vector<int64_t>      requires_nodes;
    std::vector<int64_t>      exclusive_skills;
    std::vector<SkillRankRow> ranks;
};

struct SkillTreeRow {
    int64_t                   guid = 0;
    std::string               name;
    std::string               kind;
    int64_t                   leveling_type_guid = 0;
    int64_t                   class_guid = 0;
    int64_t                   weapon_line_guid = 0;
    std::vector<SkillNodeRow> nodes;
};

struct TalentData {
    std::vector<SkillTreeRow>                                    trees;
    std::map<std::string, int64_t>                               point_types;
    std::map<int64_t, std::map<int, std::map<int64_t, int64_t>>> points;
};

struct ItemWeaponRow {
    int64_t     attack_ability = 0;
    int64_t     skill_line = 0;
    std::string skill_line_name;
};

struct PopFilter {
    std::vector<std::string> biomes, cultures, node_types, required_tags, seasons, weather;
    std::vector<int64_t>     node_ids, zones;
    std::string              node_level_min = "Wilderness", node_level_max = "Metropolis";
    double                   time_of_day_start = -1, time_of_day_end = -1;
};

struct NpcAssetRow {
    int64_t                  guid = 0;
    std::string              name;
    std::string              asset_ref;
    std::string              archetype_path;
    int64_t                  faction = 0;
    double                   aggro_radius = 0, leash_radius = 0;
    bool                     override_aggro = false, override_leash = false;
    double                   scale_min = 1, scale_max = 1;
    int64_t                  dialogue_root = 0, interact_component = 0, interaction_def = 0, appearance = 0;
    std::vector<int64_t>     loot_tables;
    bool                     dev_only = false;
    PopFilter                filter;
    std::vector<std::string> tags;
};

struct NpcInstanceRow {
    int64_t                  guid = 0;
    std::string              name;
    int                      level_min = 0, level_max = 0;
    bool                     dynamic_leveling = false;
    double                   respawn_time = 0, respawn_variance = 0;
    std::string              respawn_rule;
    int64_t                  patrol_route = 0, vendor = 0, interact_component = 0, interact_response = 0, dialogue = 0;
    int64_t                  dialogue_root = 0;
    std::string              ambient_behavior;
    double                   min_radius = 0, max_radius = 0, arrival_distance = 0, wander_speed_mult = 0, wander_delay = 0, wander_delay_deviation = 0;
    std::string              speed_expr, snapping_rule, orientation_type, spawn_location_type;
    double                   aggro_radius = 0, area_weight = 0, activation_range = 0;
    bool                     override_aggro = false, blacklisted = false;
    int64_t                  group_id = 0;
    int                      num_waypoints = 0;
    double                   max_angle = 0, follow_distance = 0;
    std::string              formation, idle_anim;
    bool                     align_to_slope = false;
    bool                     dev_only = false;
    int64_t                  filter_override = 0;
    std::string              spawner_priority;
    std::vector<std::string> tags;
    std::vector<int64_t>     asset_sets;
    std::vector<int64_t>     loot_tables;
};

struct NpcRouteRow {
    int64_t                            guid = 0;
    std::string                        name;
    // Routes anchored to a city node or encounter cannot be walked in open-world coordinates.
    bool                               looping = false, start_closest = false, relative_city_node = false, relative_encounter = false;
    double                             tx = 0, ty = 0, tz = 0, qx = 0, qy = 0, qz = 0, qw = 1, sx = 1, sy = 1, sz = 1;
    std::vector<std::array<double, 3>> points;
    std::vector<std::array<double, 3>> waypoints;
    std::vector<double>                pauses;
    std::string                        pathing_set;
};

struct NpcSetMember {
    int64_t key = 0;
    int64_t instance = 0;
    double  x = 0, y = 0, z = 0, yaw = 0;
    bool    random_rotation = false;
};

struct NpcSetRow {
    int64_t                   guid = 0;
    std::string               name;
    int64_t                   area = 0;
    double                    activation_range = 0;
    bool                      blacklisted = false;
    bool                      dev_only = false;
    int64_t                   filter_override = 0;
    std::vector<std::string>  tags;
    std::vector<NpcSetMember> members;
};

struct NpcWorldSpawn {
    int64_t     guid = 0;
    std::string name;
    std::string culture;
    std::string map_name;
    bool        dev_only = false;
    int64_t     filter_override = 0;
};

struct NpcPlacement {
    int64_t spawn = 0;
    int64_t key = 0;
    int64_t set = 0;
    double  x = 0, y = 0, z = 0, yaw = 0;
    int     min_level = 0, max_level = 0;
};

struct NpcAreaCircle {
    double  x = 0, y = 0, z = 0, extent = 0, weight = 1;
    int64_t max_spawns = 0;
};

struct NpcAreaRow {
    int64_t                    guid = 0;
    std::string                name;
    int                        min_spawns = 0, max_spawns = 0;
    std::vector<NpcAreaCircle> circles;
};

inline constexpr const char* kCombatInCombatTimer = "in_combat_timer";
inline constexpr const char* kCombatGlobalDamageMult = "global_damage_multiplier";
inline constexpr const char* kCombatGlobalHealingMult = "global_healing_multiplier";
inline constexpr const char* kCombatOutOfRange = "out_of_range_distance";
inline constexpr const char* kCombatActivationLeeway = "server_activation_leeway";
inline constexpr const char* kCombatThirdPartyHit = "third_party_hit_distance";
inline constexpr const char* kCombatMaxHitsPerRpc = "max_hits_per_rpc";
inline constexpr const char* kCombatStatRegenTime = "stat_regen_time";
inline constexpr int kPermissionHostile = 1, kPermissionFriendly = 2, kPermissionNeutral = 3;
inline constexpr int64_t kAlignmentPlayer = 6064629444695359488ll;
inline constexpr int64_t kAlignmentNpcPlayerEnemy = 6064629454762803230ll;

struct CombatRows {
    struct Element {
        std::string tag;
        int64_t     damage = 0, damage_mult = 0, damage_add_mult = 0, mitigation = 0;
    };
    std::map<std::string, int64_t>                profile;
    std::vector<Element>                          elements;
    std::map<std::string, double>                 config;
    std::map<int64_t, int>                        permissions;
    std::map<int64_t, std::map<int64_t, int64_t>> relation;
};

struct LootXp {
    std::string type;
    int64_t     expr = 0;
};

struct LootItem {
    int64_t item = 0, variation = 0, qty = 0;
};

struct LootCurrency {
    int64_t currency = 0, amount = 0;
};

struct LootReward {
    std::vector<LootXp>       xp;
    std::vector<LootItem>     items;
    std::vector<LootCurrency> currency;
};

struct LootContainer {
    std::string             grant;
    std::string             algo;
    int64_t                 pred = 0;
    int64_t                 n = 0;
    std::vector<double>     w;
    std::vector<int64_t>    pct, xw;
    std::vector<LootReward> rewards;
};

struct LootTable {
    int64_t                    guid = 0;
    std::string                name;
    int64_t                    pred = 0;
    std::string                algo;
    int64_t                    n = 0;
    std::vector<int64_t>       subs;
    std::vector<double>        w;
    std::vector<int64_t>       pct, xw;
    std::vector<LootContainer> containers;
};

}  // namespace aoc::worldd::content
