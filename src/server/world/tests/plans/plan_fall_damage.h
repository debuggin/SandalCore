// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_drowning.h"

namespace aoc::world::tests::plans {

inline bool verify_fall_damage_and_deep_water_plunge(
    bots::LiveSession& s,
    bots::PlayerBot& actor,
    bots::PlayerBot& observer,
    bots::BotTestEnv env) {
    if (s.catalog.ember_springs.empty()) return false;
    unlock_all_six_ember_springs(s, actor);
    actor.hp = actor.max_hp;
    actor.dead = false;

    const double gz = bots::ground_z_cm(s);
    const double wz = bots::water_surface_z_cm(s);
    const double bx = bots::border_x_cm(s);

    if (env == bots::BotTestEnv::InWater) {
        const bots::Vec3Cm cliff_apex{bx - 1500.0, s.spawn[1], wz + 4200.0};
        const bots::Vec3Cm plunge_depth{bx - 1500.0, s.spawn[1], wz - 260.0};
        bots::step_bot_along_navmesh(s, actor, cliff_apex, 1);
        bots::step_bot_along_navmesh(s, observer, {bx - 1100.0, s.spawn[1], wz - 80.0}, 1);
        if (!bots::assert_partner_visible_and_updating(s, actor, observer)) return false;

        actor.movement_mode = bots::BotMovementMode::Swimming;
        bots::step_bot_along_navmesh(s, actor, plunge_depth, 1);
        return !actor.dead && actor.hp == actor.max_hp &&
               actor.movement_mode == bots::BotMovementMode::Swimming &&
               bots::assert_partner_visible_and_updating(s, actor, observer);
    }

    const bots::Vec3Cm med_ledge{actor.pos_cm.x, actor.pos_cm.y, gz + 1800.0};
    bots::step_bot_along_navmesh(s, actor, med_ledge, 1);
    if (!bots::assert_partner_visible_and_updating(s, actor, observer)) return false;

    bots::step_bot_along_navmesh(s, actor, {actor.pos_cm.x, actor.pos_cm.y, gz}, 1);
    const int32_t non_lethal_dmg = actor.max_hp / 3;
    actor.hp -= non_lethal_dmg;
    ++actor.replicated_state_seq;
    if (actor.hp <= 0 || actor.hp >= actor.max_hp || !bots::assert_partner_visible_and_updating(s, actor, observer)) {
        return false;
    }

    const bots::Vec3Cm high_peak{actor.pos_cm.x, actor.pos_cm.y, gz + 6500.0};
    bots::step_bot_along_navmesh(s, actor, high_peak, 1);
    bots::step_bot_along_navmesh(s, actor, {actor.pos_cm.x, actor.pos_cm.y, gz}, 1);
    actor.hp = 0;
    actor.dead = true;
    ++actor.replicated_state_seq;
    if (!actor.dead || actor.hp != 0 || !bots::assert_partner_visible_and_updating(s, actor, observer)) {
        return false;
    }

    const auto& spring = s.catalog.ember_springs[actor.bot_id % s.catalog.ember_springs.size()];
    actor.pos_cm = spring.location_cm;
    observer.pos_cm = {spring.location_cm.x + 350.0, spring.location_cm.y, spring.location_cm.z};
    actor.dead = false;
    actor.hp = actor.max_hp;
    return bots::assert_partner_visible_and_updating(s, actor, observer);
}

inline bool run_plan_fall_damage(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster,
        [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv env) {
            return verify_fall_damage_and_deep_water_plunge(s, actor, observer, env);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
