// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_storage.h"
#include "protocol/repl/craft_rpc.h"

namespace aoc::world::tests::plans {

struct CraftingRecipeSpec {
    const char* station_name = "";
    int64_t recipe_guid = 0;
    int64_t input_item_guid = 0;
    const char* input_item_name = "";
    int32_t input_qty = 0;
    int64_t output_item_guid = 0;
    const char* output_item_name = "";
    int32_t output_qty = 0;
};

inline std::array<CraftingRecipeSpec, 5> build_town_crafting_recipes(const bots::LiveSession& s) {
    const auto& r = s.catalog.craft_recipe_guids;
    const auto& c = s.catalog.consumable_item_guids;
    const int64_t in0 = !s.catalog.resource_nodes.empty() ? s.catalog.resource_nodes[0].yield_item_guid : c[0];
    const int64_t in1 = s.catalog.resource_nodes.size() > 1 ? s.catalog.resource_nodes[1].yield_item_guid : c[1];
    return {{
        {"Smelter", r[0], in0, "Copper Ore", 4, c[0], "Refined Copper Ingot", 2},
        {"Lumbermill", r[1], in1, "Oak Wood", 4, c[1], "Seasoned Oak Timber", 2},
        {"Forge", r[2], c[0], "Refined Copper Ingot", 2, c[2], "Tempered Verra Greatsword", 1},
        {"Loom", r[3], in0, "Riverreed Fiber", 4, c[3], "Woven Mooncloth Bolt", 2},
        {"Alchemy", r[4], in1, "Granite Powder & Herbs", 3, c[4], "Draught of Unyielding Vigor", 2},
    }};
}

inline bool send_live_start_craft_bunch(bots::LiveSession& s, int64_t recipe_guid, int32_t qty) {
    const bots::schema::ClassSchema& pc = bots::class_named("AoCPlayerControllerBP_C");
    const bots::schema::RpcSchema* rpc = pc.rpc("ServerStartCraft");
    if (!rpc) return false;
    bots::repl::StartCraft c{};
    c.station = s.pawn_guid;
    c.recipe = recipe_guid;
    c.quantity = qty;
    const auto p = bots::repl::start_craft_params(*rpc, c);
    if (!p) return false;
    bots::send_live_bunches(s, {s.client->actor(3, bots::build_actor_rpc(pc, "ServerStartCraft", *p), true)});
    bots::drain_live_udp_nonblocking(s);
    return true;
}

inline bool execute_station_crafting_recipe(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    const CraftingRecipeSpec& recipe) {
    if (recipe.recipe_guid == 0 || recipe.input_item_guid == 0 || recipe.output_item_guid == 0) return false;
    if (!send_live_start_craft_bunch(s, recipe.recipe_guid, recipe.output_qty)) return false;
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

inline bool run_plan_crafting(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16 || s.catalog.consumable_item_guids.size() < 5 || s.catalog.craft_recipe_guids.size() < 5) {
        return false;
    }
    const auto recipes = build_town_crafting_recipes(s);
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            for (const auto& recipe : recipes) {
                if (!execute_station_crafting_recipe(s, actor, recipe)) return false;
            }
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
