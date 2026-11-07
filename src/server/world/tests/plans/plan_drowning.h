// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_buddy_pairs.h"
#include "plan_loot.h"

namespace aoc::world::tests::plans {

inline bool verify_drowning_lifecycle_with_observer(
    bots::PlayerBot& actor,
    bots::PlayerBot& observer,
    bots::BotTestEnv env) {
    unlock_all_six_ember_springs(actor);
    const double deep_submerged_z = bots::kDefaultRiverWaterSurfaceZCm - 480.0;
    const bots::Vec3Cm dive_pos{
        env == bots::BotTestEnv::AcrossBoundary ? -1348500.0 : -1351500.0,
        114200.0,
        deep_submerged_z,
    };
    bots::step_bot_along_navmesh(actor, dive_pos, 3);
    bots::step_bot_along_navmesh(observer, {dive_pos.x + 350.0, dive_pos.y, bots::kDefaultRiverWaterSurfaceZCm - 40.0}, 3);
    if (actor.pos_cm.z >= bots::kDefaultRiverWaterSurfaceZCm - 300.0) return false;
    actor.movement_mode = bots::BotMovementMode::Swimming;
    if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;

    actor.oxygen_pct = 0;
    for (int tick = 0; tick < 4; ++tick) {
        const int32_t hp_before = actor.hp;
        actor.hp = std::max(0, actor.hp - (actor.max_hp / 4 + 500));
        ++actor.replicated_state_seq;
        if (actor.hp >= hp_before) return false;
        if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;
    }
    actor.hp = 0;
    actor.dead = true;
    ++actor.replicated_state_seq;
    if (!actor.dead || actor.oxygen_pct != 0) return false;

    bots::LootPileTarget drown_pile{
        79000ULL + actor.bot_id,
        actor.name + " Drowned Pack",
        actor.pos_cm,
        actor.worker_id,
        606463390000000888LL,
        1,
        180,
        actor.bot_id,
        true,
        false,
    };
    if (bots::distance_2d_cm(observer.pos_cm, drown_pile.pos_cm) > bots::kCrossWorkerAoiHaloRadiusCm) {
        return false;
    }

    const auto& spring = bots::kEmberSprings[actor.bot_id % bots::kEmberSprings.size()];
    actor.pos_cm = spring.location_cm;
    observer.pos_cm = {spring.location_cm.x + 300.0, spring.location_cm.y, spring.location_cm.z};
    actor.dead = false;
    actor.hp = actor.max_hp;
    actor.oxygen_pct = 100;
    actor.movement_mode = bots::BotMovementMode::Walking;
    return bots::assert_partner_visible_and_updating(actor, observer);
}

inline bool run_plan_drowning(std::vector<bots::PlayerBot>& roster) {
    return bots::run_all_64_archetype_combinations_all_envs(
        roster,
        [](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv env) {
            return verify_drowning_lifecycle_with_observer(actor, observer, env);
        });
}

}  // namespace aoc::world::tests::plans
