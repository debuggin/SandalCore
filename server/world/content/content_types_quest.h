// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace aoc::worldd::content {

struct DialogueCriterion {
    std::string              kind;
    std::string              op;
    int64_t                  value = 0;
    int64_t                  quest = 0, objective = 0, currency = 0;
    std::string              quest_query;
    std::string              objective_query;
    std::string              variable;
    std::string              right_variable;
    std::string              expression;
    std::vector<int64_t>     items;
    std::vector<std::string> tags;
};

struct DialogueRequirement {
    // Prefix-encoded boolean tree: node := type count child..., type 0=AND, 2=OR, 1=ALL, 3=ANY, 5=NONE.
    std::vector<int>               tokens;
    std::vector<DialogueCriterion> criteria;
    bool empty() const { return tokens.empty(); }
};

struct DialogueActionRow {
    std::string          action;
    std::string          window;
    int64_t              vendor = 0;
    std::string          response;
    int64_t              access_config = 0;
    int64_t              quest = 0, objective = 0;
    std::vector<int64_t> rewards;
};

struct DialogueChoiceRow {
    int64_t                        next = 0;
    DialogueRequirement            requirement;
    std::vector<DialogueActionRow> actions;
};

struct DialogueNodeRow {
    int64_t                        guid = 0;
    std::string                    name, type, exhaust;
    int64_t                        quest = 0;
    bool                           has_text = true;
    std::vector<DialogueChoiceRow> choices;
    std::vector<DialogueActionRow> on_enter, on_exit;
};

struct DialogueQuestRow {
    int64_t             guid = 0;
    std::string         name;
    bool                disabled = false, repeatable = false;
    DialogueRequirement activation;
};

struct CraftStationDef {
    int64_t     guid = 0;
    std::string name;
    std::string display_name;
    bool        processing = false;
    int         max_tier = -1;
    int         queue_size = 1;
    int         simultaneous = 1;
    bool        individual = true;
    std::string tax_tag;
    int64_t     recipe_set = 0;
};

struct CraftStationData {
    std::map<int64_t, CraftStationDef> defs;
    std::map<int64_t, int64_t>         asset_defs;
    std::map<std::string, int64_t>     by_name;
    const CraftStationDef* def_of_asset(int64_t asset) const {
        const auto a = asset_defs.find(asset);
        if (a == asset_defs.end()) return nullptr;
        const auto d = defs.find(a->second);
        return d == defs.end() ? nullptr : &d->second;
    }
};

struct AttunePoint {
    int64_t     guid = 0;
    std::string name, descriptor, level_name;
    int         number = 0;
    double      x = 0, y = 0, z = 0;
    double      radius = 0;
};

struct RespawnDef {
    int64_t     guid = 0;
    std::string name, descriptor;
    int         kind = 0;
    int64_t     attune = 0;
    double      box_x = 0, box_y = 0, box_z = 0;
    double      exclusion = 0, max_range = 0;
};

struct RespawnData {
    std::map<int64_t, AttunePoint> places;
    std::map<int64_t, RespawnDef>  respawns;
    std::map<int64_t, int64_t>     instance_respawn;
};

struct CraftInput {
    int64_t     item = 0;
    int64_t     filter = 0;
    std::string tags;
    int         quantity = 0;
    int         min_rarity = 0;
    bool        consume = true;
    // Primary reagents are consumed automatically by the server and omitted from client requests.
    bool        primary = false;
};

struct CraftPred {
    std::string type;
    int         node_level = -1;
    std::string biomes, modifications, nodes;
    bool        hidden = false;
};

struct CraftRecipe {
    int64_t                 guid = 0;
    std::string             name;
    int64_t                 profession = 0;
    int                     tier = -1;
    double                  base_duration = 0;
    std::string             std_cost;
    int64_t                 cost_guid = 0;
    int64_t                 reward = 0;
    bool                    learnable = false;
    bool                    inventory_destination = true;
    int                     fuel = 0;
    bool                    blacklisted = false;
    std::string             category_tag;
    bool                    has_duration_formula = false, has_quantity_formula = false, has_quality_formula = false;
    std::vector<CraftInput> inputs;
    std::vector<CraftPred>  preds;
};

struct CraftCost {
    int64_t tier = 0;
    int64_t value = 0;
};

struct CraftRewardItem {
    int64_t item = 0;
    int64_t quantity = 0;
};

struct CraftRewardContainer {
    enum class Pick : uint8_t { All, PureRandom, WeightedRandom };
    Pick                                      pick = Pick::All;
    int                                       number = 0;
    std::vector<std::vector<CraftRewardItem>> rewards;
    std::vector<double>                       weights;
};

enum class XpRule : uint8_t { None, Processing, AnimalHusbandry, Crafting, Unknown };

struct CraftReward {
    std::vector<CraftRewardContainer> containers;
    XpRule                            profession_rule = XpRule::None;
    XpRule                            adventuring_rule = XpRule::None;
    int64_t                           profession = 0;
    std::string                       unknown_formula;
    int                               unread = 0;
};

struct CraftXpCurves {
    std::vector<std::pair<double, double>> kill;
    std::vector<std::pair<double, double>> tier[3];
    bool ok() const { return !kill.empty() && !tier[0].empty() && !tier[1].empty() && !tier[2].empty(); }
};

inline constexpr int64_t kCurveBaseKillXp = 6064630639394422811ll;
inline constexpr int64_t kCurveTierModNovice = 6064633469604267308ll, kCurveTierModApprentice = 6064633469606364461ll, kCurveTierModJourneyman = 6064633469607937326ll;
inline constexpr int64_t kTypeCharacterProfession = 5612931243805009072ll;

struct ArtisanProfession {
    bool    processing = false;
    int64_t quality = 0, xp = 0, time = 0, quantity = 0, rarity = 0;
};

struct ArtisanData {
    using Curve = std::vector<std::pair<double, double>>;
    Curve                                crafting_quality, crafting_xp, processing_time, processing_xp;
    Curve                                rarity_step[5];
    Curve                                quantity_chance[6];
    ArtisanProfession                    general_crafting, general_processing;
    std::map<int64_t, ArtisanProfession> by_profession;
    size_t                               crafting_professions = 0, processing_professions = 0;
    bool ok() const { return !crafting_quality.empty() && !crafting_xp.empty() && !processing_time.empty() && !processing_xp.empty() && !by_profession.empty(); }
};

struct CraftRecipeData {
    std::unordered_map<int64_t, CraftRecipe>    recipes;
    std::unordered_map<int64_t, CraftReward>    rewards;
    CraftXpCurves                               xp_curves;
    ArtisanData                                 artisan;
    std::map<int64_t, CraftCost>                costs;
    std::map<int64_t, std::vector<int64_t>>     set_lists;
    std::map<int64_t, std::vector<int64_t>>     list_recipes;
    std::map<int64_t, std::vector<CraftPred>>   list_preds;
    std::map<std::string, std::vector<int64_t>> tag_items;
    bool                                        tags_read = false;

    bool tag_admits(const std::string& tag, int64_t item) const {
        std::string lower;
        for (const char c : tag) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto it = tag_items.find(lower);
        return it != tag_items.end() && std::binary_search(it->second.begin(), it->second.end(), item);
    }
    const CraftRecipe* recipe(int64_t guid) const {
        const auto it = recipes.find(guid);
        return it == recipes.end() ? nullptr : &it->second;
    }
    const CraftReward* reward_of(const CraftRecipe& r) const {
        const auto it = rewards.find(r.reward);
        return it == rewards.end() ? nullptr : &it->second;
    }
    bool list_holds(int64_t list, int64_t recipe) const {
        const auto l = list_recipes.find(list);
        return l != list_recipes.end() &&
               std::find(l->second.begin(), l->second.end(), recipe) != l->second.end();
    }
    bool set_holds(int64_t set, int64_t recipe, std::vector<int64_t>* lists_out = nullptr) const {
        const auto s = set_lists.find(set);
        if (s == set_lists.end()) return false;
        bool found = false;
        for (const int64_t list : s->second) {
            if (!list_holds(list, recipe)) continue;
            found = true;
            if (lists_out) lists_out->push_back(list);
        }
        return found;
    }
};

struct DialogueData {
    std::map<int64_t, int64_t>          roots;
    std::map<int64_t, DialogueNodeRow>  nodes;
    std::map<int64_t, DialogueQuestRow> quests;
    std::map<int64_t, int64_t>          instance_roots;
    std::map<int64_t, int64_t>          asset_roots;
};

struct QuestStartAction {
    std::string                              action;
    int64_t                                  objective = 0, quest = 0;
    std::string                              variable;
    int64_t                                  value = 0;
    bool                                     conditional = false;
    DialogueRequirement                      rule;
    int64_t                                  reward_table = 0;
    std::vector<std::pair<int64_t, int64_t>> items;
    std::string                              text;
    std::vector<int64_t>                     spawns;
};
using QuestAction = QuestStartAction;

struct QuestSource {
    std::string kind;
    int64_t     instance = 0, node = 0, interactable = 0, item = 0;
};

struct QuestLocation {
    double      x = 0, y = 0, z = 0, radius = 0;
    bool        completes = false;
    bool        hidden = false;
    std::string display;
    int64_t     id = 0;
};

struct QuestObjectiveRow {
    int64_t                  guid = 0;
    std::string              name, type;
    int64_t                  count = 1;
    bool                     awake = false, hidden = false, completes = false, optional = false;
    bool                     show_dialogue = false;
    double                   timeout = 0;
    bool                     succeed_on_timeout = false;
    DialogueRequirement      activation;
    std::vector<int64_t>     rewards;
    std::vector<QuestAction> on_start, on_tick, on_success, on_fail;
    struct ItemReward {
        int64_t              table = 0;
        std::vector<int64_t> instances, sets;
        bool                 tag_query = false;
    };
    std::vector<ItemReward>    item_rewards;
    std::vector<int64_t>       kill_sets, kill_instances;
    bool                       advanced = false;
    int                        elite = 0;
    bool                       kill_owner = false, use_context = false, tag_query = false;
    std::vector<QuestSource>   sources;
    bool                       any_source = false;
    std::vector<int64_t>       components, actors;
    std::string                collect_type;
    std::vector<int64_t>       items;
    std::vector<std::string>   tags;
    std::vector<std::string>   rarities;
    std::vector<QuestLocation> locations;
    bool                       override_markers = false;
    std::vector<QuestLocation> override_locations;
    std::vector<int64_t>       trackers;
    bool                       no_dynamic_markers = false;
    struct Cluster {
        double      epsilon = 20000, min_radius = 5000, max_radius = 500000, merge = 6000;
        int         min_points = 2, max_clusters = 50;
        std::string default_type = "SpecificLocations";
        bool        weighted = true;
    } cluster;
    bool                 area_map = false;
    bool                 crate = false;
    std::vector<int64_t> crate_types, crate_items;
    std::string          quest_variable;
    bool                 inc_variable = false;
    std::string          notify;
    int64_t              recipe = 0;
    std::vector<int64_t> abilities;
    DialogueRequirement  rule;
};

struct QuestRow {
    int64_t                        guid = 0;
    std::string                    name, flavor, locality;
    int                            level = 0;
    bool                           can_abandon = true, repeatable = false, disabled = false, default_activate = true;
    int64_t                        giver_registration = 0;
    DialogueRequirement            activation;
    std::vector<QuestStartAction>  start_actions;
    std::vector<int64_t>           rewards;
    std::vector<QuestObjectiveRow> objectives;
    std::vector<QuestAction>       on_success, on_fail, on_tick;
    double                         timeout = 0;
    bool                           succeed_on_timeout = false;
};

struct QuestOfferRow {
    int64_t quest = 0, node = 0;
};

struct QuestAcceptRow {
    int64_t              node = 0, quest = 0, on_success = 0;
    std::vector<int64_t> objectives;
};

struct QuestItemLink {
    int64_t     quest = 0;
    std::string kind;
};

struct QuestData {
    std::map<int64_t, QuestRow>                   quests;
    std::map<std::string, int64_t>                by_name;
    std::map<int64_t, std::vector<QuestOfferRow>> instance_offers;
    std::map<int64_t, std::vector<QuestOfferRow>> asset_offers;
    std::map<int64_t, QuestAcceptRow>             accepts;
    std::map<int64_t, int64_t>                    confirms;
    std::map<int64_t, QuestAcceptRow>             turn_ins;
    std::map<int64_t, std::vector<int64_t>>       zoi_quests;
    std::map<int64_t, std::vector<QuestItemLink>> item_quests;
    std::map<int64_t, int64_t>                    item_crate;
    std::map<int64_t, std::vector<int64_t>>       kill_sets;
    std::map<int64_t, std::vector<int64_t>>       shrine_icds;
};

struct InteractableRow {
    std::string                              name;
    double                                   radius = 200, tolerance = 30, discovery = 200;
    bool                                     require_los = false;
    DialogueRequirement                      requirement;
    int64_t                                  pid = 0;
    double                                   channel = 0, busy = 0;
    bool                                     allow_combat = true, single_use = false, delete_done = false;
    double                                   delete_delay = 0;
    std::string                              anim;
    std::vector<std::pair<int64_t, int64_t>> grants;
    int64_t                                  reward_table = 0;
    std::vector<int64_t>                     complete_objs, reveal_objs;
};

struct ActorDefRow {
    double despawn_delay = 0;
    bool   despawn_final = false;
};

struct ObjectData {
    std::map<int64_t, InteractableRow> interactables;
    std::map<int64_t, ActorDefRow>     actor_defs;
    std::map<int64_t, int64_t>         instance_iad;
    std::map<int64_t, bool>            quest_items;
    std::map<int64_t, int64_t>         item_boxes;
};

}  // namespace aoc::worldd::content
