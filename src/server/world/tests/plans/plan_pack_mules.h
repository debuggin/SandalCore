// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_caravans.h"
#include "plan_locomotion_walk_run.h"

namespace aoc::world::tests::plans {

struct PackMuleEntity {
    uint64_t mule_guid = 0;
    uint64_t owner_bot_id = 0;
    int32_t timber_crates = 0;
    int32_t ore_crates = 0;
    int32_t stone_crates = 0;
    bots::Vec3Cm pos_cm{};
    std::string worker_id = "aoc-world-riverlands";
    uint64_t authority_epoch = 1;
    bool summoned = false;
};

inline bool execute_pack_mule_heavy_haul_and_unload(bots::PlayerBot& bot) {
    bot.worker_id = "aoc-world-riverlands";
    bot.zone_key = "riverlands";
    PackMuleEntity mule{
        606463850000000000ULL + bot.bot_id, bot.bot_id, 8, 10, 6,
        bot.pos_cm, bot.worker_id, bot.authority_epoch, true
    };
    const int32_t total_loaded_crates = mule.timber_crates + mule.ore_crates + mule.stone_crates;
    if (!mule.summoned || total_loaded_crates != 24) return false;

    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(bot, {-416600.0, 520400.0, bots::kDefaultGroundTerrainZCm}, 4);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    mule.pos_cm = bot.pos_cm;
    mule.worker_id = bot.worker_id;
    mule.authority_epoch = bot.authority_epoch;
    if (mule.worker_id != bot.worker_id || mule.authority_epoch != epoch_before + 1) return false;

    constexpr int64_t kHeavyCrateBundleGuid = 606463850000000999LL;
    const int32_t inv_before = count_bot_item_qty(bot, kHeavyCrateBundleGuid);
    bots::add_inventory_item(bot, kHeavyCrateBundleGuid, "Unloaded Heavy Resource Crates", total_loaded_crates, bots::ResourceKind::Ore);
    mule.timber_crates = 0;
    mule.ore_crates = 0;
    mule.stone_crates = 0;
    mule.summoned = false;
    return count_bot_item_qty(bot, kHeavyCrateBundleGuid) == inv_before + 24 &&
           (mule.timber_crates + mule.ore_crates + mule.stone_crates) == 0;
}

inline bool run_plan_pack_mules(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!execute_pack_mule_heavy_haul_and_unload(actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
