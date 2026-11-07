// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_npc_dialogue.h"

namespace aoc::world::tests::plans {

constexpr int64_t kVendorHealthPotionItemGuid = 606463400000000101LL;
constexpr int64_t kVendorEnchantScrollItemGuid = 606463400000000102LL;
constexpr int64_t kVendorCraftingFluxItemGuid = 606463400000000103LL;
constexpr int64_t kVendorHealthPotionUnitCostCopper = 120;
constexpr int64_t kVendorEnchantScrollUnitCostCopper = 350;
constexpr int64_t kVendorSellMaterialUnitPriceCopper = 45;

inline bool vendor_buy_item(bots::PlayerBot& bot, int64_t item_guid, const char* item_name, int32_t qty, int64_t unit_cost) {
    if (qty <= 0 || unit_cost <= 0) return false;
    const int64_t total_cost = static_cast<int64_t>(qty) * unit_cost;
    if (bot.copper_balance < total_cost) return false;
    bot.copper_balance -= total_cost;
    bots::add_inventory_item(bot, item_guid, item_name, qty, bots::ResourceKind::Plant);
    return true;
}

inline bool vendor_sell_item(bots::PlayerBot& bot, int64_t item_guid, int32_t qty, int64_t unit_price) {
    if (qty <= 0 || unit_price <= 0) return false;
    for (auto& item : bot.inventory) {
        if (item.item_guid == item_guid && item.quantity >= qty) {
            item.quantity -= qty;
            bot.copper_balance += static_cast<int64_t>(qty) * unit_price;
            return true;
        }
    }
    return false;
}

inline bool run_plan_vendors(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            auto vendor_npc = kTownDialogueNpcs[0];
            vendor_npc.pos_cm = {actor.pos_cm.x + 120.0, actor.pos_cm.y, actor.pos_cm.z};
            if (!interact_with_town_npc_dialogue(actor, vendor_npc)) return false;
            const int64_t wallet_before = actor.copper_balance;
            if (!vendor_buy_item(actor, kVendorHealthPotionItemGuid, "Elixir of Renewal", 3, kVendorHealthPotionUnitCostCopper)) {
                return false;
            }
            if (!vendor_buy_item(actor, kVendorEnchantScrollItemGuid, "Scroll of Ascendant Tempering", 2, kVendorEnchantScrollUnitCostCopper)) {
                return false;
            }
            if (!vendor_buy_item(actor, kVendorCraftingFluxItemGuid, "Arcane Smelting Flux", 4, kVendorSellMaterialUnitPriceCopper)) {
                return false;
            }
            const int64_t expected_after_buy = wallet_before - (3 * kVendorHealthPotionUnitCostCopper) -
                                               (2 * kVendorEnchantScrollUnitCostCopper) -
                                               (4 * kVendorSellMaterialUnitPriceCopper);
            if (actor.copper_balance != expected_after_buy) return false;
            if (!vendor_sell_item(actor, kVendorCraftingFluxItemGuid, 2, kVendorSellMaterialUnitPriceCopper)) {
                return false;
            }
            if (actor.copper_balance != expected_after_buy + (2 * kVendorSellMaterialUnitPriceCopper)) {
                return false;
            }
            actor.copper_balance = wallet_before;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
