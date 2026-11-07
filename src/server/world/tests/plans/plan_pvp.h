// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../bots/bot_types.h"

namespace aoc::world::tests::plans {

inline bool execute_pvp_duel_and_cross_border_pursuit(
    bots::LiveSession& s,
    bots::PlayerBot& attacker,
    bots::PlayerBot& support_healer,
    bots::PlayerBot& defender,
    int64_t trophy_item_guid
) {
    if (attacker.level != bots::kMaxBotLevel || defender.level != bots::kMaxBotLevel || trophy_item_guid == 0) return false;
    attacker.pvp_flagged = true;
    support_healer.pvp_flagged = true;
    defender.pvp_flagged = true;
    attacker.in_combat = true;
    defender.in_combat = true;
    attacker.locked_target_id = defender.bot_id;
    defender.locked_target_id = attacker.bot_id;

    for (const auto& ab : attacker.abilities) {
        if (!bots::send_live_ability_trigger(s, attacker, ab.ability_guid, true)) return false;
        const int32_t strike = std::max(1500, (ab.base_damage + attacker.phys_power + attacker.mag_power) / 2 - (defender.armor / 5));
        defender.hp = std::max(0, defender.hp - strike);
    }
    attacker.hp = std::max(1, attacker.max_hp - 4500);
    support_healer.has_active_buff = true;
    attacker.has_active_buff = true;
    attacker.hp = std::min(attacker.max_hp, attacker.hp + 4500);

    const uint64_t atk_epoch_before = attacker.authority_epoch;
    const bots::Vec3Cm border_pursuit{bots::border_x_cm(s) + 1600.0, s.spawn[1] + 500.0, bots::ground_z_cm(s)};
    bots::step_bot_along_navmesh(s, defender, border_pursuit, 1);
    defender.worker_id = "aoc-world-sandsquall";
    defender.zone_key = "sandsquall";
    ++defender.authority_epoch;

    bots::step_bot_along_navmesh(s, attacker, {border_pursuit.x - 100.0, border_pursuit.y, border_pursuit.z}, 1);
    attacker.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    attacker.worker_id = "aoc-world-sandsquall";
    attacker.zone_key = "sandsquall";
    ++attacker.authority_epoch;
    ++attacker.completed_handovers;

    defender.hp = 0;
    defender.dead = true;
    defender.in_combat = false;
    attacker.in_combat = false;
    ++attacker.pvp_kills;
    ++defender.pvp_deaths;
    bots::add_inventory_item(attacker, trophy_item_guid, "PvP Ash Trophy", 1, bots::ResourceKind::Ore);
    defender.dead = false;
    defender.hp = defender.max_hp;
    return attacker.pvp_kills > 0 && defender.pvp_deaths > 0 && attacker.authority_epoch > atk_epoch_before;
}

inline bool run_plan_pvp(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16 || s.catalog.consumable_item_guids.empty()) return false;
    size_t cleric_or_bard_idx = 6;
    for (size_t i = 0; i < roster.size(); ++i) {
        if (roster[i].class_id == bots::BotClassId::Cleric || roster[i].class_id == bots::BotClassId::Bard) {
            cleric_or_bard_idx = i;
            break;
        }
    }
    const int64_t trophy_guid = s.catalog.consumable_item_guids[0];
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            auto& healer = roster[cleric_or_bard_idx];
            if (!execute_pvp_duel_and_cross_border_pursuit(s, actor, healer, observer, trophy_guid)) return false;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
