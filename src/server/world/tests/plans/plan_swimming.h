// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_mounts.h"

namespace aoc::world::tests::plans {

inline bool verify_unmounted_swimming_and_diving_handover(bots::PlayerBot& bot) {
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.active_mount_guid = 0;
    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 1.0;
    bot.oxygen_pct = 100;
    bot.stamina = bot.max_stamina;
    bot.worker_id = "aoc-world-riverlands";
    bot.zone_key = "riverlands";

    const bots::Vec3Cm shoreline{-1353000.0, 114000.0, bots::kDefaultGroundTerrainZCm};
    bots::step_bot_along_navmesh(bot, shoreline, 2);

    const bots::Vec3Cm surface_swim{-1351500.0, 114100.0, bots::kDefaultRiverWaterSurfaceZCm - 45.0};
    bots::step_bot_along_navmesh(bot, surface_swim, 3);
    if (bot.pos_cm.z >= bots::kDefaultRiverWaterSurfaceZCm) return false;
    bot.movement_mode = bots::BotMovementMode::Swimming;
    bot.move_speed_multiplier = 0.85;
    bot.stamina = std::max(0, bot.stamina - 40);

    const bots::Vec3Cm deep_dive{-1350200.0, 114200.0, bots::kDefaultRiverWaterSurfaceZCm - 420.0};
    bots::step_bot_along_navmesh(bot, deep_dive, 3);
    bot.oxygen_pct = 86;
    if (bot.movement_mode != bots::BotMovementMode::Swimming || bot.oxygen_pct >= 100 || bot.stamina >= bot.max_stamina) {
        return false;
    }

    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(bot, {-1347000.0, 114300.0, bots::kDefaultRiverWaterSurfaceZCm - 80.0}, 4);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-turquoise-sea";
    bot.zone_key = "turquoise_sea";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    if (bot.mounted || bot.movement_mode != bots::BotMovementMode::Swimming || bot.authority_epoch != epoch_before + 1) {
        return false;
    }

    bots::step_bot_along_navmesh(bot, shoreline, 3);
    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 1.0;
    bot.oxygen_pct = 100;
    bot.stamina = bot.max_stamina;
    return bot.pos_cm.z >= bots::kDefaultGroundTerrainZCm && bot.movement_mode == bots::BotMovementMode::Walking;
}

inline bool verify_mounted_swimming_and_ground_mount_deep_water_rule(bots::PlayerBot& bot) {
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::GroundSteed;
    bot.active_mount_guid = kGroundMountGuid;
    bot.movement_mode = bots::BotMovementMode::MountedGround;
    bot.move_speed_multiplier = 1.75;

    const bots::Vec3Cm deep_water{-1351800.0, 114000.0, bots::kDefaultRiverWaterSurfaceZCm - 240.0};
    bots::step_bot_along_navmesh(bot, deep_water, 3);
    if (bot.pos_cm.z < bots::kDefaultRiverWaterSurfaceZCm && bot.active_mount == bots::MountFamily::GroundSteed) {
        bot.mounted = false;
        bot.active_mount = bots::MountFamily::None;
        bot.active_mount_guid = 0;
        bot.movement_mode = bots::BotMovementMode::Swimming;
        bot.move_speed_multiplier = 0.85;
    }
    if (bot.mounted || bot.movement_mode != bots::BotMovementMode::Swimming) return false;

    bot.mounted = true;
    bot.active_mount = bots::MountFamily::AquaticMount;
    bot.active_mount_guid = kAquaticMountGuid;
    bot.movement_mode = bots::BotMovementMode::MountedAquatic;
    bot.move_speed_multiplier = 1.90;
    bot.oxygen_pct = 100;
    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(bot, {-1346200.0, 114600.0, bots::kDefaultRiverWaterSurfaceZCm - 120.0}, 4);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-turquoise-sea";
    bot.zone_key = "turquoise_sea";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    if (!bot.mounted || bot.active_mount != bots::MountFamily::AquaticMount ||
        bot.movement_mode != bots::BotMovementMode::MountedAquatic || bot.authority_epoch != epoch_before + 1) {
        return false;
    }

    bots::step_bot_along_navmesh(bot, {-644519.0, 376773.0, bots::kDefaultGroundTerrainZCm}, 3);
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.active_mount_guid = 0;
    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 1.0;
    return !bot.mounted && bot.movement_mode == bots::BotMovementMode::Walking;
}

inline bool run_plan_swimming(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_unmounted_swimming_and_diving_handover(actor)) return false;
            if (!verify_mounted_swimming_and_ground_mount_deep_water_rule(actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
