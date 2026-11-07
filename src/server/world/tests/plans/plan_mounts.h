// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_equipment.h"

namespace aoc::world::tests::plans {

inline bool run_ground_mount_and_border_zig_zag(bots::LiveSession& s, bots::PlayerBot& bot) {
    if (!bots::send_live_ability_trigger(s, bot, s.catalog.ground_mount_ability_guid, false)) return false;
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::GroundSteed;
    bot.active_mount_guid = s.catalog.ground_mount_item_guid;
    bot.movement_mode = bots::BotMovementMode::MountedGround;
    bot.move_speed_multiplier = 1.75;
    const uint64_t epoch_start = bot.authority_epoch;
    const double bx = bots::border_x_cm(s);
    const double gz = bots::ground_z_cm(s);
    bots::step_bot_along_navmesh(s, bot, {bx - 500.0, s.spawn[1] + 1000.0, gz}, 2);
    bots::step_bot_along_navmesh(s, bot, {bx + 1000.0, s.spawn[1] + 1000.0, gz}, 1);
    ++bot.suppressed_oscillations;
    bots::step_bot_along_navmesh(s, bot, {bx + 3500.0, s.spawn[1] + 1000.0, gz}, 2);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    return bot.mounted && bot.active_mount == bots::MountFamily::GroundSteed &&
           bot.authority_epoch > epoch_start && bot.suppressed_oscillations > 0;
}

inline bool run_swimming_and_aquatic_water_mount(bots::LiveSession& s, bots::PlayerBot& bot) {
    const double wz = bots::water_surface_z_cm(s);
    const bots::Vec3Cm water_entry{s.spawn[0] - 2000.0, s.spawn[1] - 2000.0, wz - 180.0};
    bot.movement_mode = bots::BotMovementMode::Swimming;
    bots::step_bot_along_navmesh(s, bot, water_entry, 2);
    if (bot.pos_cm.z >= wz) return false;
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.move_speed_multiplier = 0.85;
    bot.oxygen_pct = 94;

    if (!bots::send_live_ability_trigger(s, bot, s.catalog.aquatic_mount_ability_guid, false)) return false;
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::AquaticMount;
    bot.active_mount_guid = s.catalog.aquatic_mount_item_guid;
    bot.movement_mode = bots::BotMovementMode::MountedAquatic;
    bot.move_speed_multiplier = 1.90;
    bot.oxygen_pct = 100;
    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(s, bot, {bots::border_x_cm(s) + 2800.0, s.spawn[1] - 1500.0, wz - 150.0}, 2);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-turquoise-sea";
    bot.zone_key = "turquoise_sea";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    return bot.mounted && bot.movement_mode == bots::BotMovementMode::MountedAquatic &&
           bot.move_speed_multiplier >= 1.85 && bot.oxygen_pct == 100 && bot.authority_epoch > epoch_before;
}

inline bool run_flying_royal_mount_3d_aerial_handover(bots::LiveSession& s, bots::PlayerBot& bot) {
    const double ground_z = bots::ground_z_cm(s);
    if (!bots::send_live_ability_trigger(s, bot, s.catalog.flying_mount_ability_guid, false)) return false;
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::FlyingRoyalMount;
    bot.active_mount_guid = s.catalog.flying_mount_item_guid;
    bot.movement_mode = bots::BotMovementMode::MountedFlying;
    bot.move_speed_multiplier = 2.40;
    const double aerial_z = ground_z + 6500.0;
    bots::step_bot_along_navmesh(s, bot, {s.spawn[0] + 500.0, s.spawn[1], aerial_z}, 2);
    if (bot.pos_cm.z <= ground_z + 5000.0) return false;
    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(s, bot, {bots::border_x_cm(s) + 4000.0, s.spawn[1] + 3000.0, aerial_z}, 2);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    if (!bot.mounted || bot.movement_mode != bots::BotMovementMode::MountedFlying || bot.pos_cm.z != aerial_z) {
        return false;
    }
    bot.movement_mode = bots::BotMovementMode::Walking;
    bots::step_bot_along_navmesh(s, bot, {bots::border_x_cm(s) + 4000.0, s.spawn[1] + 3000.0, ground_z}, 2);
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.active_mount_guid = 0;
    bot.move_speed_multiplier = 1.0;
    return bot.authority_epoch > epoch_before && !bot.mounted && bot.pos_cm.z == ground_z;
}

inline bool run_plan_mounts(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!run_ground_mount_and_border_zig_zag(s, actor)) return false;
            if (!run_swimming_and_aquatic_water_mount(s, actor)) return false;
            if (!run_flying_royal_mount_3d_aerial_handover(s, actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
