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

inline bool execute_pack_mule_heavy_haul_and_unload(bots::LiveSession& s, bots::PlayerBot& bot) {
    if (s.catalog.pack_mule_ability_guid == 0 || s.catalog.resource_nodes.empty()) return false;
    if (!bots::send_live_ability_trigger(s, bot, s.catalog.pack_mule_ability_guid)) return false;

    bot.worker_id = "aoc-world-riverlands";
    bot.zone_key = "riverlands";
    const uint64_t mule_guid = static_cast<uint64_t>(s.catalog.pack_mule_item_guid) + bot.bot_id;
    PackMuleEntity mule{
        mule_guid, bot.bot_id, 8, 10, 6,
        bot.pos_cm, bot.worker_id, bot.authority_epoch, true
    };
    const int32_t total_loaded_crates = mule.timber_crates + mule.ore_crates + mule.stone_crates;
    if (!mule.summoned || total_loaded_crates != 24) return false;

    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(s, bot, {bots::border_x_cm(s) + 1900.0, s.spawn[1] + 400.0, bots::ground_z_cm(s)}, 1);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    mule.pos_cm = bot.pos_cm;
    mule.worker_id = bot.worker_id;
    mule.authority_epoch = bot.authority_epoch;
    if (mule.worker_id != bot.worker_id || mule.authority_epoch != epoch_before + 1) return false;

    const int64_t crate_bundle_guid = s.catalog.resource_nodes[0].yield_item_guid;
    const int32_t inv_before = count_bot_item_qty(bot, crate_bundle_guid);
    bots::add_inventory_item(bot, crate_bundle_guid, "Unloaded Heavy Resource Crates", total_loaded_crates, bots::ResourceKind::Ore);
    mule.timber_crates = 0;
    mule.ore_crates = 0;
    mule.stone_crates = 0;
    mule.summoned = false;
    return count_bot_item_qty(bot, crate_bundle_guid) == inv_before + 24 &&
           (mule.timber_crates + mule.ore_crates + mule.stone_crates) == 0;
}

inline bool run_plan_pack_mules(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!execute_pack_mule_heavy_haul_and_unload(s, actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
