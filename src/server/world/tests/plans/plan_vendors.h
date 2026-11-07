// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_npc_dialogue.h"
#include "protocol/repl/vendor_rpc.h"

namespace aoc::world::tests::plans {

constexpr int64_t kVendorHealthPotionUnitCostCopper = 120;
constexpr int64_t kVendorEnchantScrollUnitCostCopper = 350;
constexpr int64_t kVendorSellMaterialUnitPriceCopper = 45;

inline bool send_live_vendor_and_tax_bunches(bots::LiveSession& s) {
    const bots::schema::ClassSchema& pc = bots::class_named("AoCPlayerControllerBP_C");
    std::vector<fake::Bunch> batch;
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerOpenVendor")) {
        bots::repl::OpenVendorRequest req{{1, 2, 3, 4}, s.catalog.vendor_def_guid};
        if (const auto p = bots::repl::server_open_vendor_params(*rpc, req)) {
            batch.push_back(s.client->actor(3, bots::build_actor_rpc(pc, "ServerOpenVendor", *p), true));
        }
    }
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerGetNodeTaxTable")) {
        if (const auto p = bots::repl::node_tax_request_params(*rpc, s.catalog.vendor_def_guid)) {
            batch.push_back(s.client->actor(3, bots::build_actor_rpc(pc, "ServerGetNodeTaxTable", *p), true));
        }
    }
    if (batch.empty()) return false;
    bots::send_live_bunches(s, std::move(batch));
    bots::drain_live_udp_nonblocking(s);
    return true;
}

inline bool vendor_buy_item(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    int64_t item_guid,
    const char* item_name,
    int32_t qty,
    int64_t unit_cost) {
    if (qty <= 0 || unit_cost <= 0 || item_guid == 0) return false;
    if (!send_live_vendor_and_tax_bunches(s)) return false;
    const int64_t total_cost = static_cast<int64_t>(qty) * unit_cost;
    if (bot.copper_balance < total_cost) return false;
    bot.copper_balance -= total_cost;
    bots::add_inventory_item(bot, item_guid, item_name, qty, bots::ResourceKind::Plant);
    return true;
}

inline bool vendor_sell_item(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    int64_t item_guid,
    int32_t qty,
    int64_t unit_price) {
    if (qty <= 0 || unit_price <= 0 || item_guid == 0) return false;
    if (!send_live_vendor_and_tax_bunches(s)) return false;
    for (auto& item : bot.inventory) {
        if (item.item_guid == item_guid && item.quantity >= qty) {
            item.quantity -= qty;
            bot.copper_balance += static_cast<int64_t>(qty) * unit_price;
            return true;
        }
    }
    return false;
}

inline bool run_plan_vendors(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16 || s.catalog.consumable_item_guids.size() < 3) return false;
    const auto npcs = build_town_dialogue_npcs(s);
    const int64_t potion_guid = s.catalog.consumable_item_guids[0];
    const int64_t scroll_guid = s.catalog.consumable_item_guids[1];
    const int64_t flux_guid = s.catalog.consumable_item_guids[2];
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            auto vendor_npc = npcs[0];
            vendor_npc.pos_cm = {actor.pos_cm.x + 120.0, actor.pos_cm.y, actor.pos_cm.z};
            if (!interact_with_town_npc_dialogue(s, actor, vendor_npc)) return false;
            const int64_t wallet_before = actor.copper_balance;
            if (!vendor_buy_item(s, actor, potion_guid, "Elixir of Renewal", 3, kVendorHealthPotionUnitCostCopper)) {
                return false;
            }
            if (!vendor_buy_item(s, actor, scroll_guid, "Scroll of Ascendant Tempering", 2, kVendorEnchantScrollUnitCostCopper)) {
                return false;
            }
            if (!vendor_buy_item(s, actor, flux_guid, "Arcane Smelting Flux", 4, kVendorSellMaterialUnitPriceCopper)) {
                return false;
            }
            const int64_t expected_after_buy = wallet_before - (3 * kVendorHealthPotionUnitCostCopper) -
                                               (2 * kVendorEnchantScrollUnitCostCopper) -
                                               (4 * kVendorSellMaterialUnitPriceCopper);
            if (actor.copper_balance != expected_after_buy) return false;
            if (!vendor_sell_item(s, actor, flux_guid, 2, kVendorSellMaterialUnitPriceCopper)) {
                return false;
            }
            if (actor.copper_balance != expected_after_buy + (2 * kVendorSellMaterialUnitPriceCopper)) {
                return false;
            }
            actor.copper_balance = wallet_before;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    return ok;
}

}  // namespace aoc::world::tests::plans
