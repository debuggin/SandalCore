// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_weapons.h"

namespace aoc::world::tests::plans {

inline bool send_live_item_used_bunch(bots::LiveSession& s, int64_t item_guid) {
    if (item_guid == 0) return false;
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    bots::repl::RpcParams used;
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed")) {
        bots::repl::set_rpc_param(*rpc, used, "Storage", bots::repl::Value::u64(3));
        bots::repl::set_rpc_param(*rpc, used, "Slot", bots::repl::Value::i64(s.scroll_slot));
        bots::repl::set_rpc_param(*rpc, used, "ExpectedItem.A", bots::repl::Value::i64(static_cast<int32_t>(s.scroll_guid.a)));
        bots::repl::set_rpc_param(*rpc, used, "ExpectedItem.B", bots::repl::Value::i64(static_cast<int32_t>(s.scroll_guid.b)));
        bots::repl::set_rpc_param(*rpc, used, "ExpectedItem.C", bots::repl::Value::i64(static_cast<int32_t>(s.scroll_guid.c)));
        bots::repl::set_rpc_param(*rpc, used, "ExpectedItem.D", bots::repl::Value::i64(static_cast<int32_t>(s.scroll_guid.d)));
        bots::repl::set_rpc_param(*rpc, used, "RequestedEquipSlot", bots::repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    }
    const bool sent = bots::send_live_bunches(s, {s.client->actor(5, bots::build_actor_rpc(pawn, "ServerItemUsed", used), true)});
    bots::drain_live_udp_nonblocking(s);
    return sent;
}

inline bool consume_inventory_stack(bots::LiveSession& s, bots::PlayerBot& bot, int64_t item_guid) {
    if (!send_live_item_used_bunch(s, item_guid)) return false;
    for (auto& item : bot.inventory) {
        if (item.item_guid == item_guid && item.quantity > 0) {
            --item.quantity;
            return true;
        }
    }
    return false;
}

inline bool verify_potions_rations_and_scrolls_usage(bots::LiveSession& s, bots::PlayerBot& bot) {
    if (s.catalog.consumable_item_guids.size() < 5 || s.catalog.ember_springs.empty()) return false;
    const int64_t hp_potion_guid = s.catalog.consumable_item_guids[0];
    const int64_t mana_potion_guid = s.catalog.consumable_item_guids[1];
    const int64_t ration_guid = s.catalog.consumable_item_guids[2];
    const int64_t fury_scroll_guid = s.catalog.consumable_item_guids[3];
    const int64_t recall_scroll_guid = s.catalog.consumable_item_guids[4];

    bots::add_inventory_item(bot, hp_potion_guid, "Elixir of Renewal", 2, bots::ResourceKind::Plant);
    bots::add_inventory_item(bot, mana_potion_guid, "Draught of Clarity", 2, bots::ResourceKind::Plant);
    bots::add_inventory_item(bot, ration_guid, "Roast River Stag Ration", 2, bots::ResourceKind::Huntable);
    bots::add_inventory_item(bot, fury_scroll_guid, "Scroll of Ascendant Fury", 2, bots::ResourceKind::Tree);
    bots::add_inventory_item(bot, recall_scroll_guid, "Scroll of Winstead Recall", 1, bots::ResourceKind::Tree);

    bot.hp = bot.max_hp - 4500;
    bot.mana = bot.max_mana - 2500;
    const int32_t hp_before = bot.hp;
    const int32_t mana_before = bot.mana;
    const int32_t phys_before = bot.phys_power;
    const int32_t mag_before = bot.mag_power;

    if (!consume_inventory_stack(s, bot, hp_potion_guid)) return false;
    bot.hp = std::min(bot.max_hp, bot.hp + 3500);
    if (!consume_inventory_stack(s, bot, mana_potion_guid)) return false;
    bot.mana = std::min(bot.max_mana, bot.mana + 2000);
    if (!consume_inventory_stack(s, bot, ration_guid)) return false;
    bot.max_stamina += 150;
    bot.stamina = bot.max_stamina;
    if (!consume_inventory_stack(s, bot, fury_scroll_guid)) return false;
    bot.phys_power += 180;
    bot.mag_power += 180;
    bot.has_active_buff = true;
    if (!consume_inventory_stack(s, bot, recall_scroll_guid)) return false;
    const auto& target_spring = s.catalog.ember_springs[0];
    bot.pos_cm = target_spring.location_cm;

    return bot.hp > hp_before && bot.mana > mana_before &&
           bot.phys_power == phys_before + 180 && bot.mag_power == mag_before + 180 &&
           bot.has_active_buff && bots::distance_2d_cm(bot.pos_cm, target_spring.location_cm) < 1.0;
}

inline bool run_plan_item_usage_and_scrolls(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_potions_rations_and_scrolls_usage(s, actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
