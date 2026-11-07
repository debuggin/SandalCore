// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_item_usage_and_scrolls.h"

namespace aoc::world::tests::plans {

struct EnchantableGearState {
    int64_t item_guid = 0;
    int32_t enchant_tier = 0;
    int32_t bonus_phys_power = 0;
    int32_t bonus_mag_power = 0;
    int32_t bonus_armor = 0;
};

inline bool apply_enchant_scroll_tier(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    EnchantableGearState& gear,
    int64_t scroll_guid,
    int32_t power_step,
    int32_t armor_step) {
    if (!consume_inventory_stack(s, bot, scroll_guid)) return false;
    ++gear.enchant_tier;
    gear.bonus_phys_power += power_step;
    gear.bonus_mag_power += power_step;
    gear.bonus_armor += armor_step;
    bot.phys_power += power_step;
    bot.mag_power += power_step;
    bot.armor += armor_step;
    return true;
}

inline bool run_plan_enchanting(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16 || s.catalog.consumable_item_guids.size() < 5) return false;
    const int64_t weapon_scroll_guid = s.catalog.consumable_item_guids[3];
    const int64_t armor_scroll_guid = s.catalog.consumable_item_guids[4];
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            bots::add_inventory_item(actor, weapon_scroll_guid, "Scroll of Ascendant Tempering", 3, bots::ResourceKind::Tree);
            bots::add_inventory_item(actor, armor_scroll_guid, "Scroll of Bulwark Warding", 3, bots::ResourceKind::Tree);
            const int32_t phys_before = actor.phys_power;
            const int32_t mag_before = actor.mag_power;
            const int32_t armor_before = actor.armor;

            EnchantableGearState weapon_state{actor.equipped_items.empty() ? weapon_scroll_guid : actor.equipped_items[0].item_guid, 0, 0, 0, 0};
            EnchantableGearState armor_state{actor.equipped_items.size() > 1 ? actor.equipped_items[1].item_guid : armor_scroll_guid, 0, 0, 0, 0};
            for (int tier = 1; tier <= 3; ++tier) {
                if (!apply_enchant_scroll_tier(s, actor, weapon_state, weapon_scroll_guid, 65, 0)) return false;
                if (!apply_enchant_scroll_tier(s, actor, armor_state, armor_scroll_guid, 0, 90)) return false;
                if (weapon_state.enchant_tier != tier || armor_state.enchant_tier != tier) return false;
            }
            if (actor.phys_power != phys_before + 195 || actor.mag_power != mag_before + 195 || actor.armor != armor_before + 270) {
                return false;
            }
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
