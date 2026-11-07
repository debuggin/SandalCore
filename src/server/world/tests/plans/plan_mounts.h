// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_equipment.h"

namespace aoc::world::tests::plans {

constexpr int64_t kGroundMountGuid = 606463220000000001LL;
constexpr int64_t kAquaticMountGuid = 606463220000000002LL;
constexpr int64_t kFlyingRoyalMountGuid = 606463220000000003LL;

inline bool run_ground_mount_and_border_zig_zag(bots::PlayerBot& bot) {
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::GroundSteed;
    bot.active_mount_guid = kGroundMountGuid;
    bot.movement_mode = bots::BotMovementMode::MountedGround;
    bot.move_speed_multiplier = 1.75;
    const uint64_t epoch_start = bot.authority_epoch;
    bots::step_bot_along_navmesh(bot, {-420500.0, 520000.0, bots::kDefaultGroundTerrainZCm}, 4);
    bots::step_bot_along_navmesh(bot, {-419000.0, 520000.0, bots::kDefaultGroundTerrainZCm}, 2);
    ++bot.suppressed_oscillations;
    bots::step_bot_along_navmesh(bot, {-416500.0, 520000.0, bots::kDefaultGroundTerrainZCm}, 4);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    return bot.mounted && bot.active_mount == bots::MountFamily::GroundSteed &&
           bot.authority_epoch > epoch_start && bot.suppressed_oscillations > 0;
}

inline bool run_swimming_and_aquatic_water_mount(bots::PlayerBot& bot) {
    const bots::Vec3Cm water_entry{-1352000.0, 114000.0, bots::kDefaultRiverWaterSurfaceZCm - 180.0};
    bots::step_bot_along_navmesh(bot, water_entry, 4);
    if (bot.pos_cm.z >= bots::kDefaultRiverWaterSurfaceZCm) return false;
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.movement_mode = bots::BotMovementMode::Swimming;
    bot.move_speed_multiplier = 0.85;
    bot.oxygen_pct = 94;
    if (bot.movement_mode != bots::BotMovementMode::Swimming) return false;

    bot.mounted = true;
    bot.active_mount = bots::MountFamily::AquaticMount;
    bot.active_mount_guid = kAquaticMountGuid;
    bot.movement_mode = bots::BotMovementMode::MountedAquatic;
    bot.move_speed_multiplier = 1.90;
    bot.oxygen_pct = 100;
    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(bot, {-1346500.0, 114500.0, bots::kDefaultRiverWaterSurfaceZCm - 150.0}, 5);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-turquoise-sea";
    bot.zone_key = "turquoise_sea";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    return bot.mounted && bot.movement_mode == bots::BotMovementMode::MountedAquatic &&
           bot.move_speed_multiplier >= 1.85 && bot.oxygen_pct == 100 && bot.authority_epoch > epoch_before;
}

inline bool run_flying_royal_mount_3d_aerial_handover(bots::PlayerBot& bot) {
    const double ground_z = bots::kDefaultGroundTerrainZCm;
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::FlyingRoyalMount;
    bot.active_mount_guid = kFlyingRoyalMountGuid;
    bot.movement_mode = bots::BotMovementMode::MountedFlying;
    bot.move_speed_multiplier = 2.40;
    const double aerial_z = ground_z + 6500.0;
    bots::step_bot_along_navmesh(bot, {-644000.0, 376000.0, aerial_z}, 4);
    if (bot.pos_cm.z <= ground_z + 5000.0) return false;
    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(bot, {-415000.0, 680000.0, aerial_z}, 6);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    if (!bot.mounted || bot.movement_mode != bots::BotMovementMode::MountedFlying || bot.pos_cm.z != aerial_z) {
        return false;
    }
    bots::step_bot_along_navmesh(bot, {-415000.0, 680000.0, ground_z}, 4);
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.active_mount_guid = 0;
    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 1.0;
    return bot.authority_epoch > epoch_before && !bot.mounted && bot.pos_cm.z == ground_z;
}

inline bool run_plan_mounts(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!run_ground_mount_and_border_zig_zag(actor)) return false;
            if (!run_swimming_and_aquatic_water_mount(actor)) return false;
            if (!run_flying_royal_mount_3d_aerial_handover(actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
