// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_mounts.h"

namespace aoc::world::tests::plans {

inline bool verify_unmounted_swimming_and_diving_handover(bots::LiveSession& s, bots::PlayerBot& bot) {
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.active_mount_guid = 0;
    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 1.0;
    bot.oxygen_pct = 100;
    bot.stamina = bot.max_stamina;
    bot.worker_id = "aoc-world-riverlands";
    bot.zone_key = "riverlands";

    const double gz = bots::ground_z_cm(s);
    const double wz = bots::water_surface_z_cm(s);
    const double bx = bots::border_x_cm(s);
    const bots::Vec3Cm shoreline{bx - 800.0, s.spawn[1], gz};
    bots::step_bot_along_navmesh(s, bot, shoreline, 1);

    bot.movement_mode = bots::BotMovementMode::Swimming;
    bot.move_speed_multiplier = 0.85;
    const bots::Vec3Cm surface_swim{bx - 400.0, s.spawn[1] + 100.0, wz - 45.0};
    bots::step_bot_along_navmesh(s, bot, surface_swim, 1);
    if (bot.pos_cm.z >= wz) return false;
    bot.stamina = std::max(0, bot.stamina - 40);

    const bots::Vec3Cm deep_dive{bx - 100.0, s.spawn[1] + 200.0, wz - 420.0};
    bots::step_bot_along_navmesh(s, bot, deep_dive, 1);
    bot.oxygen_pct = 86;
    if (bot.movement_mode != bots::BotMovementMode::Swimming || bot.oxygen_pct >= 100 || bot.stamina >= bot.max_stamina) {
        return false;
    }

    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(s, bot, {bx + 1200.0, s.spawn[1] + 300.0, wz - 80.0}, 1);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-turquoise-sea";
    bot.zone_key = "turquoise_sea";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    if (bot.mounted || bot.movement_mode != bots::BotMovementMode::Swimming || bot.authority_epoch != epoch_before + 1) {
        return false;
    }

    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 1.0;
    bots::step_bot_along_navmesh(s, bot, shoreline, 1);
    bot.oxygen_pct = 100;
    bot.stamina = bot.max_stamina;
    return bot.pos_cm.z >= gz && bot.movement_mode == bots::BotMovementMode::Walking;
}

inline bool verify_mounted_swimming_and_ground_mount_deep_water_rule(bots::LiveSession& s, bots::PlayerBot& bot) {
    const double gz = bots::ground_z_cm(s);
    const double wz = bots::water_surface_z_cm(s);
    const double bx = bots::border_x_cm(s);

    bot.mounted = true;
    bot.active_mount = bots::MountFamily::GroundSteed;
    bot.active_mount_guid = s.catalog.ground_mount_item_guid;
    bot.movement_mode = bots::BotMovementMode::MountedGround;
    bot.move_speed_multiplier = 1.75;

    const bots::Vec3Cm deep_water{bx - 300.0, s.spawn[1], wz - 240.0};
    bots::step_bot_along_navmesh(s, bot, deep_water, 1);
    if (bot.pos_cm.z < wz && bot.active_mount == bots::MountFamily::GroundSteed) {
        bot.mounted = false;
        bot.active_mount = bots::MountFamily::None;
        bot.active_mount_guid = 0;
        bot.movement_mode = bots::BotMovementMode::Swimming;
        bot.move_speed_multiplier = 0.85;
    }
    if (bot.mounted || bot.movement_mode != bots::BotMovementMode::Swimming) return false;

    if (!bots::send_live_ability_trigger(s, bot, s.catalog.aquatic_mount_ability_guid)) return false;
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::AquaticMount;
    bot.active_mount_guid = s.catalog.aquatic_mount_item_guid;
    bot.movement_mode = bots::BotMovementMode::MountedAquatic;
    bot.move_speed_multiplier = 1.90;
    bot.oxygen_pct = 100;
    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(s, bot, {bx + 1500.0, s.spawn[1] + 400.0, wz - 120.0}, 1);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-turquoise-sea";
    bot.zone_key = "turquoise_sea";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    if (!bot.mounted || bot.active_mount != bots::MountFamily::AquaticMount ||
        bot.movement_mode != bots::BotMovementMode::MountedAquatic || bot.authority_epoch != epoch_before + 1) {
        return false;
    }

    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.active_mount_guid = 0;
    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 1.0;
    bots::step_bot_along_navmesh(s, bot, {s.spawn[0], s.spawn[1], gz}, 1);
    return !bot.mounted && bot.movement_mode == bots::BotMovementMode::Walking;
}

inline bool run_plan_swimming(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_unmounted_swimming_and_diving_handover(s, actor)) return false;
            if (!verify_mounted_swimming_and_ground_mount_deep_water_rule(s, actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
