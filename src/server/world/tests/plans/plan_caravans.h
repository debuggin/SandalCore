// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_crafting.h"
#include "plan_pets.h"

namespace aoc::world::tests::plans {

struct CaravanConvoyEntity {
    uint64_t caravan_guid = 0;
    uint64_t owner_bot_id = 0;
    const char* origin_node = "Winstead";
    const char* destination_node = "Miraleth";
    int32_t hp = 65000;
    int32_t max_hp = 65000;
    int32_t cargo_commodity_crates = 0;
    int64_t cargo_glint_value = 0;
    bots::Vec3Cm pos_cm{};
    std::string worker_id = "aoc-verra-rvr-node-winstead";
    uint64_t authority_epoch = 1;
    bool in_transit = false;
    bool delivered = false;
};

inline bool execute_caravan_corridor_and_border_handover(bots::LiveSession& s, bots::PlayerBot& bot) {
    if (s.catalog.ember_springs.empty() || s.catalog.monsters.empty()) return false;
    const auto& origin_spring = s.catalog.ember_springs[0];
    const auto& dest_spring = s.catalog.ember_springs[s.catalog.ember_springs.size() > 1 ? 1 : 0];
    bot.worker_id = "aoc-verra-rvr-node-winstead";
    bot.zone_key = "verra_rvr_node_winstead";
    bot.pos_cm = origin_spring.location_cm;

    const uint64_t caravan_guid = static_cast<uint64_t>(s.catalog.ground_mount_item_guid) + bot.bot_id;
    CaravanConvoyEntity caravan{
        caravan_guid, bot.bot_id, "Winstead", "Miraleth",
        65000, 65000, 12, 1800, bot.pos_cm, bot.worker_id, bot.authority_epoch, true, false
    };

    const bots::Vec3Cm border_waypoint{bots::border_x_cm(s) + 1200.0, s.spawn[1] + 600.0, bots::ground_z_cm(s)};
    bots::step_bot_along_navmesh(s, bot, border_waypoint, 1);
    caravan.pos_cm = bot.pos_cm;
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-verra-rvr-node-miraleth";
    bot.zone_key = "verra_rvr_node_miraleth";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    caravan.worker_id = bot.worker_id;
    caravan.authority_epoch = bot.authority_epoch;

    bots::MonsterTarget highway_raider = s.catalog.monsters[bot.bot_id % s.catalog.monsters.size()];
    highway_raider.hp = 22000;
    highway_raider.max_hp = 22000;
    highway_raider.dead = false;
    highway_raider.pos_cm = {caravan.pos_cm.x + 220.0, caravan.pos_cm.y, caravan.pos_cm.z};
    highway_raider.worker_id = caravan.worker_id;
    caravan.hp -= 3500;
    const int32_t dealt = execute_bot_class_ability_rotation(s, bot, highway_raider);
    highway_raider.hp = 0;
    highway_raider.dead = true;
    if (dealt <= 0 || caravan.hp <= 0 || caravan.cargo_commodity_crates != 12) return false;

    bots::step_bot_along_navmesh(s, bot, dest_spring.location_cm, 1);
    caravan.pos_cm = bot.pos_cm;
    caravan.in_transit = false;
    caravan.delivered = true;
    const int64_t copper_before = bot.copper_balance;
    bot.copper_balance += caravan.cargo_glint_value * 2;
    return caravan.delivered && caravan.worker_id == "aoc-verra-rvr-node-miraleth" &&
           bot.copper_balance == copper_before + 3600;
}

inline bool run_plan_caravans(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!execute_caravan_corridor_and_border_handover(s, actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
