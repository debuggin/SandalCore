// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_storage.h"

namespace aoc::world::tests::plans {

struct CraftingRecipeSpec {
    const char* station_name = "";
    int64_t input_item_guid = 0;
    const char* input_item_name = "";
    int32_t input_qty = 0;
    int64_t output_item_guid = 0;
    const char* output_item_name = "";
    int32_t output_qty = 0;
};

inline constexpr std::array<CraftingRecipeSpec, 5> kTownCraftingRecipes = {{
    {"Smelter", 606463110000000002LL, "Copper Ore", 4, 606463500000000001LL, "Refined Copper Ingot", 2},
    {"Lumbermill", 606463110000000001LL, "Oak Wood", 4, 606463500000000002LL, "Seasoned Oak Timber", 2},
    {"Forge", 606463500000000001LL, "Refined Copper Ingot", 2, 606463500000000003LL, "Tempered Verra Greatsword", 1},
    {"Loom", 606463110000000004LL, "Riverreed Fiber", 4, 606463500000000004LL, "Woven Mooncloth Bolt", 2},
    {"Alchemy", 606463110000000003LL, "Granite Powder & Herbs", 3, 606463500000000005LL, "Draught of Unyielding Vigor", 2},
}};

inline bool execute_station_crafting_recipe(bots::PlayerBot& bot, const CraftingRecipeSpec& recipe) {
    bots::add_inventory_item(bot, recipe.input_item_guid, recipe.input_item_name, recipe.input_qty, bots::ResourceKind::Ore);
    const int32_t input_before = count_bot_item_qty(bot, recipe.input_item_guid);
    const int32_t output_before = count_bot_item_qty(bot, recipe.output_item_guid);
    if (input_before < recipe.input_qty) return false;

    for (auto& item : bot.inventory) {
        if (item.item_guid == recipe.input_item_guid && item.quantity >= recipe.input_qty) {
            item.quantity -= recipe.input_qty;
            break;
        }
    }
    bots::add_inventory_item(bot, recipe.output_item_guid, recipe.output_item_name, recipe.output_qty, bots::ResourceKind::Ore);
    const int32_t input_after = count_bot_item_qty(bot, recipe.input_item_guid);
    const int32_t output_after = count_bot_item_qty(bot, recipe.output_item_guid);
    return input_after == input_before - recipe.input_qty &&
           output_after == output_before + recipe.output_qty;
}

inline bool run_plan_crafting(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            for (const auto& recipe : kTownCraftingRecipes) {
                if (!execute_station_crafting_recipe(actor, recipe)) return false;
            }
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
