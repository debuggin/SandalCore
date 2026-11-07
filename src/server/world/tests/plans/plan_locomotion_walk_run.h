// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_swimming.h"

namespace aoc::world::tests::plans {

constexpr double kBaseFootWalkSpeedCmPerSec = 300.0;
constexpr double kBaseFootRunSpeedCmPerSec = 600.0;
constexpr double kMountedWalkTrotSpeedCmPerSec = 525.0;
constexpr double kMountedGallopRunSpeedCmPerSec = 1050.0;

inline bool verify_on_foot_walk_vs_run_locomotion(bots::LiveSession& s, bots::PlayerBot& bot) {
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 0.50;
    const bots::Vec3Cm origin = bot.pos_cm;
    const bots::Vec3Cm walk_dest{origin.x + kBaseFootWalkSpeedCmPerSec, origin.y, origin.z};
    bots::step_bot_along_navmesh(s, bot, walk_dest, 1);
    const double walk_dist = bots::distance_2d_cm(origin, bot.pos_cm);

    if (!bots::send_live_ability_trigger(s, bot, s.catalog.sprint_ability_guid)) return false;
    bot.move_speed_multiplier = 1.00;
    const bots::Vec3Cm run_origin = bot.pos_cm;
    const bots::Vec3Cm run_dest{run_origin.x + kBaseFootRunSpeedCmPerSec, run_origin.y, run_origin.z};
    bots::step_bot_along_navmesh(s, bot, run_dest, 1);
    const double run_dist = bots::distance_2d_cm(run_origin, bot.pos_cm);

    const double ratio = run_dist / std::max(1.0, walk_dist);
    return !bot.mounted && bot.movement_mode == bots::BotMovementMode::Walking &&
           std::abs(walk_dist - kBaseFootWalkSpeedCmPerSec) < 0.01 &&
           std::abs(run_dist - kBaseFootRunSpeedCmPerSec) < 0.01 &&
           std::abs(ratio - 2.0) < 0.01;
}

inline bool verify_mounted_trot_walk_vs_gallop_run_locomotion(bots::LiveSession& s, bots::PlayerBot& bot) {
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::GroundSteed;
    bot.active_mount_guid = s.catalog.ground_mount_item_guid;
    bot.movement_mode = bots::BotMovementMode::MountedGround;
    bot.move_speed_multiplier = 0.875;
    const bots::Vec3Cm trot_origin = bot.pos_cm;
    const bots::Vec3Cm trot_dest{trot_origin.x + kMountedWalkTrotSpeedCmPerSec, trot_origin.y, trot_origin.z};
    bots::step_bot_along_navmesh(s, bot, trot_dest, 1);
    const double trot_dist = bots::distance_2d_cm(trot_origin, bot.pos_cm);

    bot.move_speed_multiplier = 1.75;
    const bots::Vec3Cm gallop_origin = bot.pos_cm;
    const bots::Vec3Cm gallop_dest{gallop_origin.x + kMountedGallopRunSpeedCmPerSec, gallop_origin.y, gallop_origin.z};
    bots::step_bot_along_navmesh(s, bot, gallop_dest, 1);
    const double gallop_dist = bots::distance_2d_cm(gallop_origin, bot.pos_cm);

    const double ratio = gallop_dist / std::max(1.0, trot_dist);
    const bool valid = bot.mounted && bot.movement_mode == bots::BotMovementMode::MountedGround &&
                       std::abs(trot_dist - kMountedWalkTrotSpeedCmPerSec) < 0.01 &&
                       std::abs(gallop_dist - kMountedGallopRunSpeedCmPerSec) < 0.01 &&
                       std::abs(ratio - 2.0) < 0.01;
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.active_mount_guid = 0;
    bot.movement_mode = bots::BotMovementMode::Walking;
    bot.move_speed_multiplier = 1.0;
    return valid;
}

inline bool run_plan_locomotion_walk_run(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_on_foot_walk_vs_run_locomotion(s, actor)) return false;
            if (!verify_mounted_trot_walk_vs_gallop_run_locomotion(s, actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
