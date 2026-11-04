// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "content/content_types_world.h"

#include <set>

namespace aoc::worldd::content {

struct VendorPriceRow {
    int64_t inventory = 0;
    int     ord = 0;
    int64_t item = 0, variation = 0, offer = 0, currency = 0;
    int64_t price = 0;
};

struct VendorPredicateRow {
    std::string expression, failure;
    bool        hidden = false;
};

struct TagQuery {
    bool                     present = false;
    std::vector<int64_t>     tok;
    std::vector<std::string> tags;
};

struct VendorModifier {
    struct Key {
        double      time = 0, value = 0;
        std::string interp;
    };
    int64_t              guid = 0;
    std::string          name;
    int64_t              contexts = 0;
    std::string          operation, source, factor, expression;
    std::vector<Key>     curve;
    TagQuery             filter;
    std::vector<int64_t> currencies, children;
};

struct WarehouseCostRow {
    int64_t     currency = 0;
    int64_t     type_id = 0;
    std::string value, multiplier;
};

struct WarehouseTabRow {
    int64_t                       guid = 0;
    std::string                   name;
    int                           storage_type = 0;   // 0 General, 1 Crates
    bool                          member = false, specializable = false;
    std::string                   requirement;
    std::vector<WarehouseCostRow> costs;
};

struct WarehouseChunkRow {
    int64_t                       guid = 0;
    std::string                   name;
    int                           size = 0;
    int                           level = 0;
    std::string                   requirement;
    std::vector<WarehouseCostRow> costs;
};

struct WarehouseDefRow {
    int64_t              guid = 0;
    std::string          name;
    int                  columns = 0, rows = 0;
    std::vector<int64_t> tabs;
};

struct WarehouseData {
    struct Node {
        int64_t resource_def = 0, crate_def = 0, chunks_def = 0;
    };
    std::map<int64_t, Node>                 nodes;
    std::map<int64_t, WarehouseDefRow>      defs;
    std::map<int64_t, WarehouseTabRow>      tabs;
    std::map<int64_t, std::vector<int64_t>> chunk_defs;
    std::map<int64_t, WarehouseChunkRow>    chunks;
};

struct VendorSellFilter {
    TagQuery          filter;
    std::set<int64_t> whitelist;
};

struct VendorData {
    struct Def {
        std::string name;
        int64_t     operations = 0;   // 1 buy, 2 sell, 4 repair
        std::string tax_tag;
        bool        blacklisted = false;
        std::vector<int64_t> modifiers;
        std::vector<int64_t> sell_filters;
        std::vector<int64_t> inventories;
    };
    std::map<int64_t, VendorModifier>   modifiers;
    std::map<int64_t, VendorSellFilter> sell_filters;
    std::map<int64_t, Def>     defs;
    std::map<int64_t, int64_t> instance_links;
    std::map<int64_t, int64_t> asset_links;
    std::map<int64_t, std::map<int64_t, std::vector<VendorPriceRow>>> prices;
    std::map<std::pair<int64_t, int>, std::vector<VendorPredicateRow>> predicates;
};

struct EquippedEffect {
    int64_t effect = 0;
    int     min_rarity = 0;
    int     max_rarity = 6;
    int     stacks = 1;
};

struct ItemSaleFacts : items::ItemRarityFacts {
    bool                  known = false;
    bool                  vendorable = false;
    bool                  blacklisted = false;
    int                   level = 0;
    std::set<std::string> tags;
    std::map<std::pair<int, int64_t>, int64_t> values;
};

struct PopulationData {
    std::map<int64_t, NpcAssetRow>                         assets;
    std::map<int64_t, std::vector<std::pair<int64_t, double>>> asset_sets;
    std::map<int64_t, NpcInstanceRow>                      instances;
    std::map<int64_t, NpcSetRow>                           sets;
    std::vector<NpcPlacement>                              placements;
    std::map<int64_t, NpcAreaRow>                          areas;
    std::map<int64_t, NpcRouteRow>                         routes;
    std::map<int64_t, NpcWorldSpawn>                       world_spawns;
    std::map<int64_t, PopFilter>                           filters;
    std::map<int64_t, std::vector<std::string>>            gates;
    std::map<int64_t, std::vector<std::string>>            asset_set_tags;
};

struct VendorListing {
    int64_t              item = 0;
    std::vector<int64_t> offers;
    bool                 hidden = false;
};

struct VendorInventoryRow {
    int64_t                    guid = 0;
    std::string                name;
    std::vector<VendorListing> listings;
};

struct VendorDefRow {
    int64_t                         guid = 0;
    std::string                     name;
    std::vector<VendorInventoryRow> inventories;
};

struct GmItemRow {
    int64_t     guid = 0;
    std::string name;
    int         max_stack = 1;
};

struct MountRow {
    int64_t     item = 0;
    std::string item_name;
    std::string kind;
    std::string blueprint_path;
    std::string blueprint;
    std::string activation;
    int64_t     cast_ability = 0;
    int         level = 0;
    int         ground_pct = 0;
    int         flying_pct = 0;
    int         water_pct = 0;
    int         health_pct = 100;
    bool        mule = false;
    int         crate_capacity = 0;
    bool        tradeable = false;
    std::string description;
    std::string display_name;

    double health_factor() const { return health_pct / 100.0; }
};

struct MountStatRow {
    int         level = 0;
    int64_t     stat = 0;
    std::string stat_name;
    double      value = 0;
};

struct MountAbilityRow {
    int64_t     ability = 0;
    std::string name;
    std::string role;
    bool        usable_while_mounted = false;
    bool        auto_dismount = false;
};

struct MountEffectRow {
    int64_t     effect = 0;
    std::string name;
    std::string kind;
    int         tier = -1;
    int64_t     stat = 0;
    double      value = 0;
    bool        relative = false;
    bool        infinite = false;
    double      duration = 0;
    bool        force_dismount = false;
};

struct MountData {
    std::vector<MountRow>         mounts;
    std::vector<MountStatRow>     stats;
    std::vector<MountAbilityRow>  abilities;
    std::vector<MountEffectRow>   effects;

    const MountRow*        find(int64_t item) const;
    double                 stat_at(int level, int64_t stat, double fallback = 0) const;
    const MountAbilityRow* ability(int64_t guid) const;
    const MountAbilityRow* ability_with_role(const std::string& role) const;
    const MountEffectRow*  road(int tier) const;
    const MountEffectRow*  dash() const;
};

struct StatProgramRow {
    int                  version = 0;
    std::string          mode;
    int                  stats = 0;
    int                  classes = 0;
    uint32_t             crc32 = 0;
    std::vector<uint8_t> data;
};

}  // namespace aoc::worldd::content
