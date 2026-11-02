// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "items/rarity.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::content {

struct ContentMeta {
    int         schema_version = 0;
    std::string contentc_version;
    std::string source_dbc_sha256;
    std::string built_at;
};

inline constexpr int kSchemaVersion = 1;
inline constexpr int kMinContentcVersion = 15;

struct ClassRecord {
    int64_t     guid = 0;
    std::string name;
    int64_t     archetype_id = 0;
};

struct PrimaryAttribute {
    int64_t     stat_guid = 0;
    std::string stat_name;
    double      value = 0;
    // Flagged when float32 evaluation rounds to a neighboring integer compared to float64.
    bool        rounding_ambiguous = false;
};

inline constexpr int kMinLevel = 1;
inline constexpr int kMaxLevel = 50;

inline constexpr int64_t kGuidSprint = 6064631641648332884ll;
inline constexpr int64_t kGuidDodge = 107258680465016ll;
inline constexpr int64_t kGuidRest = 109393746155128ll;
inline constexpr int64_t kGuidHealth = 108986352686712ll;
inline constexpr int64_t kGuidMaxHealth = 108986356618873ll;
inline constexpr int64_t kGuidHealthRegeneration = 109343399171806ll;
inline constexpr int64_t kGuidMana = 109183576135288ll;
inline constexpr int64_t kGuidMaxMana = 109183578756729ll;
inline constexpr int64_t kGuidManaRegeneration = 109343399171796ll;
inline constexpr int64_t kGuidGuard = 107887054050936ll;
inline constexpr int64_t kTypeAbilityRecord = 1841048597116038327ll;
inline constexpr int64_t kGuidStamina = 6064629575339934476ll;
inline constexpr int64_t kGuidMaxStamina = 6064631747701440948ll;
inline constexpr int64_t kGuidStaminaRegeneration = 6064631663405498419ll;
inline constexpr int64_t kGuidTempHealth = 6064629069966082048ll;
inline constexpr int64_t kResourceStats[] = {6064629429438907308ll, 6064629623747772676ll, 6064629623761404733ll, 6064629959589298751ll,
                                             6064633175093543338ll, 6064633668368219158ll};
inline constexpr int64_t kSourceTypeAbility = 1841048597116038327ll;
inline constexpr int64_t kSourceTypeProjectile = 979581256175327151ll;
inline constexpr int64_t kSourceTypeEffect = 3377240744094973029ll;
inline constexpr int64_t kSourceTypeZone = 7793593157135267051ll;
inline constexpr int64_t kSourceTypeReaction = 6091248426050982904ll;
inline constexpr int64_t kTargetingDefaultSelf = 6064628968549646338ll;

enum class Held : uint8_t { Naive, Smart, Toggle };

struct AbilityRule {
    int64_t     guid = 0;
    std::string name;
    Held        held = Held::Naive;
    bool        requires_out_of_combat = false;
    double      min_stamina = 0;
    int64_t     cost_stat_guid = 0;
    double      cost_amount = 0;
    bool        cost_requires_moving = false;
    double      charge_interval = 0;
    double      cooldown = 0;
    bool        triggers_cooldown = false;
    bool        disable_during_cooldown = false;
    bool        targets_self = false;
    int64_t     effect_guid = 0;
    std::string montage_path;
    std::string cancel_section;
};

struct StatMod {
    int64_t stat_guid = 0;
    int64_t source_stat_guid = 0;
    double  factor = 0;
    double  addend = 0;
};

struct EffectRule {
    int64_t              guid = 0;
    std::string          name;
    bool                 infinite = false;
    double               duration = 0;
    double               tick_timer = 0;
    bool                 stops_regen_standing = false;
    bool                 stops_regen_moving = false;
    int64_t              end_effect_guid = 0;
    double               end_effect_duration = 0;
    std::vector<StatMod> stat_mods;
};

struct AbilityRules {
    AbilityRule                   sprint, dodge, rest;
    std::map<int64_t, EffectRule> effects;
    double                        stat_regen_time = 1;
    double                        regen_rate = 5;

    const AbilityRule* ability(int64_t guid) const {
        return guid == sprint.guid ? &sprint : guid == dodge.guid ? &dodge : guid == rest.guid ? &rest : nullptr;
    }
    const EffectRule* effect(int64_t guid) const {
        const auto it = effects.find(guid);
        return it == effects.end() ? nullptr : &it->second;
    }
};

struct ItemRecord : items::ItemRarityFacts {
    int64_t              guid = 0;
    std::string          name;
    // Starter armor pieces have bCanEquip=0 in design data; only equip_slots governs wearability.
    bool                 can_equip = false;
    bool                 occupies_all_equip_slots = false;
    std::vector<uint8_t> equip_slots;
    int32_t              default_quality = 1;
    int32_t              max_stack_size = 1;
    double               max_durability = 0;
    // Six reagent records carry negative sentinel DefaultQuality values and cannot hold rarity.
    bool                 has_quality = true;
    std::optional<items::RarityRange> durability_stat;
    int32_t              inv_w = 1;
    int32_t              inv_h = 1;
    bool                 resource = false;
    std::string          display_name;

    float max_durability_at(int quality) const {
        return static_cast<float>(items::max_durability_at(*this, durability_stat ? &*durability_stat : nullptr, max_durability, static_cast<double>(quality)));
    }
};

struct ResourceBagRecord {
    int64_t                           item = 0;
    int64_t                           def = 0;
    std::string                       def_name;
    int32_t                           columns = 0;
    int32_t                           rows = 0;
    bool                              custom = false;
    std::vector<std::vector<int32_t>> matrix;
};

inline constexpr int64_t kGuidAdventuring = 108651085518458ll;
inline constexpr int64_t kGuidUniversalTree = 6064631673604472832ll;
inline constexpr int64_t kGuidAdventuringXPAddMult = 6064630809541214212ll;
inline constexpr int64_t kGuidDropCrate = 6064633774740275540ll;
inline constexpr int64_t kProfessionHunting = 108752770061965ll;
inline constexpr int64_t kTypeLevelingTypeDef = 5949882024684912047ll;
inline constexpr int64_t kTypeSkillTree = 297887639323616934ll;
inline constexpr int64_t kTypeSkillRank = 4847957796014348780ll;
inline constexpr int64_t kTypeItemRecord = 2616523457551378613ll;

struct ItemUse {
    int64_t              item = 0;
    std::string          activation;
    bool                 consume_on_use = false;
    double               cooldown = 0;
    bool                 disabled_in_combat = false;
    std::vector<int64_t> actions;
};

struct ActionResponse {
    int64_t              guid = 0;
    std::string          name;
    std::string          response_type;
    int64_t              class_guid = 0;
    std::string          class_name;
    bool                 remove_up_to_quantity = false;
    std::vector<int64_t> reward_tables;
};

struct RewardItem {
    int64_t item = 0;
    int64_t variation = 0;
    int32_t quantity = 1;
};

struct RewardXp {
    std::string experience_type;
    int         target_level = 0;
    double      margin = 0;
    int64_t     curve_guid = 0;
    int64_t     mult_stat_guid = 0;
    int64_t     expr = 0;
    std::string expression;
};

struct RewardTable {
    int64_t                 guid = 0;
    std::string             name;
    std::string             predicate;
    int                     level_below = 0;
    bool                    modelled = false;
    std::vector<RewardItem> items;
    std::vector<RewardXp>   experience;
};

struct CurveKeyRow {
    double      level = 0, value = 0;
    std::string interp;
};

struct CurveRow {
    int64_t                  guid = 0;
    std::string              name;
    std::vector<CurveKeyRow> keys;
};

struct LevelingType {
    int64_t                              guid = 0;
    std::string                          name;
    std::string                          experience_type;
    int                                  level_cap = 0;
    bool                                 uses_debt = false;
    int64_t                              xp_mult_stat_guid = 0;
    std::vector<std::pair<int, int64_t>> curve;
};

struct RankStatMod {
    int64_t     stat_guid = 0;
    std::string stat_name;
    double      value = 0;
};

struct AutoGrant {
    int64_t                  tree_guid = 0;
    std::string              tree_name;
    int64_t                  leveling_type_guid = 0;
    int64_t                  node_guid = 0;
    std::string              node_name;
    int                      level_requirement = 0;
    int64_t                  skill_guid = 0;
    std::string              skill_name;
    int64_t                  rank_guid = 0;
    int64_t                  effect_guid = 0;
    std::vector<RankStatMod> mods;
};

}  // namespace aoc::worldd::content
