// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_drowning.h"

namespace aoc::world::tests::plans {

inline bool verify_fall_damage_and_deep_water_plunge(
    bots::PlayerBot& actor,
    bots::PlayerBot& observer,
    bots::BotTestEnv env) {
    unlock_all_six_ember_springs(actor);
    actor.hp = actor.max_hp;
    actor.dead = false;

    if (env == bots::BotTestEnv::InWater) {
        const bots::Vec3Cm cliff_apex{-1351500.0, 114000.0, bots::kDefaultRiverWaterSurfaceZCm + 4200.0};
        const bots::Vec3Cm plunge_depth{-1351500.0, 114000.0, bots::kDefaultRiverWaterSurfaceZCm - 260.0};
        bots::step_bot_along_navmesh(actor, cliff_apex, 2);
        bots::step_bot_along_navmesh(observer, {-1351100.0, 114000.0, bots::kDefaultRiverWaterSurfaceZCm - 80.0}, 2);
        if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;

        bots::step_bot_along_navmesh(actor, plunge_depth, 4);
        actor.movement_mode = bots::BotMovementMode::Swimming;
        return !actor.dead && actor.hp == actor.max_hp &&
               actor.movement_mode == bots::BotMovementMode::Swimming &&
               bots::assert_partner_visible_and_updating(actor, observer);
    }

    const double ground_z = bots::kDefaultGroundTerrainZCm;
    const bots::Vec3Cm med_ledge{actor.pos_cm.x, actor.pos_cm.y, ground_z + 1800.0};
    bots::step_bot_along_navmesh(actor, med_ledge, 2);
    if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;

    bots::step_bot_along_navmesh(actor, {actor.pos_cm.x, actor.pos_cm.y, ground_z}, 3);
    const int32_t non_lethal_dmg = actor.max_hp / 3;
    actor.hp -= non_lethal_dmg;
    ++actor.replicated_state_seq;
    if (actor.hp <= 0 || actor.hp >= actor.max_hp || !bots::assert_partner_visible_and_updating(actor, observer)) {
        return false;
    }

    const bots::Vec3Cm high_peak{actor.pos_cm.x, actor.pos_cm.y, ground_z + 6500.0};
    bots::step_bot_along_navmesh(actor, high_peak, 2);
    bots::step_bot_along_navmesh(actor, {actor.pos_cm.x, actor.pos_cm.y, ground_z}, 4);
    actor.hp = 0;
    actor.dead = true;
    ++actor.replicated_state_seq;
    if (!actor.dead || actor.hp != 0 || !bots::assert_partner_visible_and_updating(actor, observer)) {
        return false;
    }

    const auto& spring = bots::kEmberSprings[actor.bot_id % bots::kEmberSprings.size()];
    actor.pos_cm = spring.location_cm;
    observer.pos_cm = {spring.location_cm.x + 350.0, spring.location_cm.y, spring.location_cm.z};
    actor.dead = false;
    actor.hp = actor.max_hp;
    return bots::assert_partner_visible_and_updating(actor, observer);
}

inline bool run_plan_fall_damage(std::vector<bots::PlayerBot>& roster) {
    return bots::run_all_64_archetype_combinations_all_envs(
        roster,
        [](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv env) {
            return verify_fall_damage_and_deep_water_plunge(actor, observer, env);
        });
}

}  // namespace aoc::world::tests::plans
