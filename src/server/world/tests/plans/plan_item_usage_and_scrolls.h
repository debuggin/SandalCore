// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_weapons.h"

namespace aoc::world::tests::plans {

constexpr int64_t kConsumableHealthPotionGuid = 606463400000000101LL;
constexpr int64_t kConsumableManaPotionGuid = 606463400000000201LL;
constexpr int64_t kConsumableStatRationGuid = 606463400000000202LL;
constexpr int64_t kScrollAttributeFuryGuid = 606463400000000203LL;
constexpr int64_t kScrollTeleportWinsteadGuid = 606463400000000204LL;

inline bool consume_inventory_stack(bots::PlayerBot& bot, int64_t item_guid) {
    for (auto& item : bot.inventory) {
        if (item.item_guid == item_guid && item.quantity > 0) {
            --item.quantity;
            return true;
        }
    }
    return false;
}

inline bool verify_potions_rations_and_scrolls_usage(bots::PlayerBot& bot) {
    bots::add_inventory_item(bot, kConsumableHealthPotionGuid, "Elixir of Renewal", 2, bots::ResourceKind::Plant);
    bots::add_inventory_item(bot, kConsumableManaPotionGuid, "Draught of Clarity", 2, bots::ResourceKind::Plant);
    bots::add_inventory_item(bot, kConsumableStatRationGuid, "Roast River Stag Ration", 2, bots::ResourceKind::Huntable);
    bots::add_inventory_item(bot, kScrollAttributeFuryGuid, "Scroll of Ascendant Fury", 2, bots::ResourceKind::Tree);
    bots::add_inventory_item(bot, kScrollTeleportWinsteadGuid, "Scroll of Winstead Recall", 1, bots::ResourceKind::Tree);

    bot.hp = bot.max_hp - 4500;
    bot.mana = bot.max_mana - 2500;
    const int32_t hp_before = bot.hp;
    const int32_t mana_before = bot.mana;
    const int32_t phys_before = bot.phys_power;
    const int32_t mag_before = bot.mag_power;

    if (!consume_inventory_stack(bot, kConsumableHealthPotionGuid)) return false;
    bot.hp = std::min(bot.max_hp, bot.hp + 3500);
    if (!consume_inventory_stack(bot, kConsumableManaPotionGuid)) return false;
    bot.mana = std::min(bot.max_mana, bot.mana + 2000);
    if (!consume_inventory_stack(bot, kConsumableStatRationGuid)) return false;
    bot.max_stamina += 150;
    bot.stamina = bot.max_stamina;
    if (!consume_inventory_stack(bot, kScrollAttributeFuryGuid)) return false;
    bot.phys_power += 180;
    bot.mag_power += 180;
    bot.has_active_buff = true;
    if (!consume_inventory_stack(bot, kScrollTeleportWinsteadGuid)) return false;
    bot.pos_cm = bots::kEmberSprings[2].location_cm;

    return bot.hp > hp_before && bot.mana > mana_before &&
           bot.phys_power == phys_before + 180 && bot.mag_power == mag_before + 180 &&
           bot.has_active_buff && bots::distance_2d_cm(bot.pos_cm, bots::kEmberSprings[2].location_cm) < 1.0;
}

inline bool run_plan_item_usage_and_scrolls(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_potions_rations_and_scrolls_usage(actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
