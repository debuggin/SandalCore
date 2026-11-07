// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_pack_mules.h"

namespace aoc::world::tests::plans {

inline bool verify_buddy_pair_visual_continuity_step(
    bots::PlayerBot& actor,
    bots::PlayerBot& observer,
    bots::BotTestEnv env) {
    if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;

    const bots::Vec3Cm delta{240.0, 180.0, env == bots::BotTestEnv::InWater ? -40.0 : 0.0};
    const bots::Vec3Cm next_actor_pos{
        actor.pos_cm.x + delta.x,
        actor.pos_cm.y + delta.y,
        actor.pos_cm.z + delta.z,
    };
    const bots::Vec3Cm next_observer_pos{
        observer.pos_cm.x + delta.x,
        observer.pos_cm.y + delta.y,
        observer.pos_cm.z + delta.z,
    };

    bots::step_bot_along_navmesh(actor, next_actor_pos, 3);
    bots::step_bot_along_navmesh(observer, next_observer_pos, 3);
    if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;

    if (env == bots::BotTestEnv::AcrossBoundary) {
        const std::string prev_worker = actor.worker_id;
        actor.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
        actor.worker_id = (prev_worker == "aoc-world-riverlands") ? "aoc-world-sandsquall" : "aoc-world-riverlands";
        ++actor.authority_epoch;
        ++actor.completed_handovers;
        if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;
    }
    return observer.partner_visible_in_aoi && observer.partner_visibility_checks >= 2;
}

inline bool run_plan_buddy_pairs(std::vector<bots::PlayerBot>& roster) {
    return bots::run_all_64_archetype_combinations_all_envs(
        roster,
        [](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv env) {
            return verify_buddy_pair_visual_continuity_step(actor, observer, env);
        });
}

}  // namespace aoc::world::tests::plans
