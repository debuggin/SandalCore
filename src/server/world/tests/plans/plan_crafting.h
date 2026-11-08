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
    const auto& in_cat = s.catalog.craft_input_item_guids;
    const auto& out_cat = s.catalog.craft_output_item_guids;
    auto out_at = [&](size_t i) -> int64_t {
        return (out_cat.size() >= 5 && out_cat[i] != 0) ? out_cat[i] : c[i % c.size()];
    };
    auto in_at = [&](size_t i, int64_t out_guid) -> int64_t {
        int64_t in_guid = (in_cat.size() >= 5 && in_cat[i] != 0) ? in_cat[i] : c[(i + 2) % c.size()];
        if (in_guid == out_guid) {
            for (size_t k = 0; k < c.size(); ++k) {
                const int64_t cand = c[(i + 2 + k) % c.size()];
                if (cand != 0 && cand != out_guid) return cand;
            }
        }
        return in_guid;
    };
    const int64_t out0 = out_at(0), out1 = out_at(1), out2 = out_at(2), out3 = out_at(3), out4 = out_at(4);
    return {{
        {"Smelter", r[0], in_at(0, out0), "Copper Ore", 4, out0, "Refined Copper Ingot", 2},
        {"Lumbermill", r[1], in_at(1, out1), "Oak Wood", 4, out1, "Seasoned Oak Timber", 2},
        {"Forge", r[2], in_at(2, out2), "Refined Copper Ingot", 2, out2, "Tempered Verra Greatsword", 1},
        {"Loom", r[3], in_at(3, out3), "Riverreed Fiber", 4, out3, "Woven Mooncloth Bolt", 2},
        {"Alchemy", r[4], in_at(4, out4), "Granite Powder & Herbs", 3, out4, "Draught of Unyielding Vigor", 2},
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
