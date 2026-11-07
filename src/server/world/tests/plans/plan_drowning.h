// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_buddy_pairs.h"
#include "plan_loot.h"

namespace aoc::world::tests::plans {

inline bool verify_drowning_lifecycle_with_observer(
    bots::LiveSession& s,
    bots::PlayerBot& actor,
    bots::PlayerBot& observer,
    bots::BotTestEnv env) {
    if (s.catalog.ember_springs.empty() || s.catalog.consumable_item_guids.empty()) return false;
    unlock_all_six_ember_springs(s, actor);
    const double wz = bots::water_surface_z_cm(s);
    const double bx = bots::border_x_cm(s);
    const double deep_submerged_z = wz - 480.0;
    const bots::Vec3Cm dive_pos{
        env == bots::BotTestEnv::AcrossBoundary ? bx + 1500.0 : bx - 1500.0,
        s.spawn[1] + 200.0,
        deep_submerged_z,
    };
    actor.movement_mode = bots::BotMovementMode::Swimming;
    observer.movement_mode = bots::BotMovementMode::Swimming;
    bots::step_bot_along_navmesh(s, actor, dive_pos, 1);
    bots::step_bot_along_navmesh(s, observer, {dive_pos.x + 350.0, dive_pos.y, wz - 40.0}, 1);
    if (actor.pos_cm.z >= wz - 300.0) return false;
    if (!bots::assert_partner_visible_and_updating(s, actor, observer)) return false;

    actor.oxygen_pct = 0;
    for (int tick = 0; tick < 4; ++tick) {
        const int32_t hp_before = actor.hp;
        actor.hp = std::max(0, actor.hp - (actor.max_hp / 4 + 500));
        ++actor.replicated_state_seq;
        if (actor.hp >= hp_before) return false;
        if (!bots::assert_partner_visible_and_updating(s, actor, observer)) return false;
    }
    actor.hp = 0;
    actor.dead = true;
    ++actor.replicated_state_seq;
    if (!actor.dead || actor.oxygen_pct != 0) return false;

    bots::LootPileTarget drown_pile{
        s.pawn_guid.ObjectId + actor.bot_id,
        actor.name + " Drowned Pack",
        actor.pos_cm,
        actor.worker_id,
        s.catalog.consumable_item_guids[0],
        1,
        180,
        actor.bot_id,
        true,
        false,
    };
    if (bots::distance_2d_cm(observer.pos_cm, drown_pile.pos_cm) > bots::kCrossWorkerAoiHaloRadiusCm) {
        return false;
    }

    const auto& spring = s.catalog.ember_springs[actor.bot_id % s.catalog.ember_springs.size()];
    actor.pos_cm = spring.location_cm;
    observer.pos_cm = {spring.location_cm.x + 300.0, spring.location_cm.y, spring.location_cm.z};
    actor.dead = false;
    actor.hp = actor.max_hp;
    actor.oxygen_pct = 100;
    actor.movement_mode = bots::BotMovementMode::Walking;
    observer.movement_mode = bots::BotMovementMode::Walking;
    return bots::assert_partner_visible_and_updating(s, actor, observer);
}

inline bool run_plan_drowning(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster,
        [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv env) {
            return verify_drowning_lifecycle_with_observer(s, actor, observer, env);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
