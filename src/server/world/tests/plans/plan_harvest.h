// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../bots/bot_types.h"

namespace aoc::world::tests::plans {

inline std::vector<bots::ResourceNodeTarget> build_verra_resource_nodes() {
    return {
        {81001ULL, bots::ResourceKind::Tree, "Ancient Weeping Willow Tree", {-645200.0, 375800.0, 11400.0}, "aoc-world-riverlands", 606463310000000001LL, 4},
        {81002ULL, bots::ResourceKind::Ore, "Lumadon & Rividium Ore Vein", {-641800.0, 379200.0, 11450.0}, "aoc-world-riverlands", 606463310000000002LL, 4},
        {81003ULL, bots::ResourceKind::Rock, "Halcyonite Basalt Outcrop", {-593500.0, 341200.0, 11200.0}, "aoc-world-riverlands", 606463310000000003LL, 4},
        {81004ULL, bots::ResourceKind::Plant, "Spiritbloom Herbal Cluster", {-619200.0, 397400.0, 11600.0}, "aoc-world-riverlands", 606463310000000004LL, 4},
        {81005ULL, bots::ResourceKind::Huntable, "Briarmoor Antlered Stag", {-417500.0, 681400.0, 9800.0}, "aoc-world-sandsquall", 606463310000000005LL, 4},
    };
}

inline bool harvest_resource_node(bots::PlayerBot& bot, bots::ResourceNodeTarget& node) {
    if (bot.dead || bot.level != bots::kMaxBotLevel || node.remaining_harvests <= 0) return false;
    bots::step_bot_along_navmesh(bot, node.pos_cm, 4);
    if (bots::distance_2d_cm(bot.pos_cm, node.pos_cm) > 250.0) return false;
    const int32_t yield_qty = bot.gathering_speed_pct >= 130 ? 3 : 2;
    --node.remaining_harvests;
    bots::add_inventory_item(bot, node.yield_item_guid, node.name, yield_qty, node.kind);
    bot.xp_earned += 1250;
    return true;
}

inline bool run_plan_harvest(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    auto nodes = build_verra_resource_nodes();
    uint32_t kind_mask = 0;
    size_t seq = 0;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            auto node = nodes[seq++ % nodes.size()];
            node.pos_cm = {actor.pos_cm.x + 120.0, actor.pos_cm.y, actor.pos_cm.z};
            node.remaining_harvests = 4;
            const size_t inv_before = actor.inventory.size();
            if (!harvest_resource_node(actor, node)) return false;
            if (actor.inventory.size() < inv_before || actor.inventory.empty()) return false;
            kind_mask |= (1u << (static_cast<uint8_t>(node.kind) - 1u));
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
    return ok && kind_mask == 0x1Fu;
}

}  // namespace aoc::world::tests::plans
