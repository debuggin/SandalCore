// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_equipment.h"
#include "protocol/repl/vendor_rpc.h"

namespace aoc::world::tests::plans {

inline bool send_live_interact_bunch(bots::LiveSession& s) {
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent")) {
        const auto target = s.goblin_guid.ObjectId != 0 ? s.goblin_guid : s.pawn_guid;
        if (const auto p = bots::repl::interact_params(*rpc, bots::repl::InteractRequest{target, ++s.next_interact_counter, 1})) {
            const bool sent = send_bunches(
                s, {s.client->actor(5, bots::build_actor_rpc(pawn, "ServerInteractWithInteractableComponent", *p), true)});
            bots::drain_live_udp_nonblocking(s);
            return sent;
        }
    }
    return false;
}

inline bool harvest_resource_node(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    bots::ResourceNodeTarget& node) {
    if (bot.dead || bot.level != bots::kMaxBotLevel || node.remaining_harvests <= 0) return false;
    bots::step_bot_along_navmesh(s, bot, node.pos_cm, 2);
    if (bots::distance_2d_cm(bot.pos_cm, node.pos_cm) > 250.0) return false;
    if (!send_live_interact_bunch(s)) return false;
    const int32_t yield_qty = bot.gathering_speed_pct >= 130 ? 3 : 2;
    --node.remaining_harvests;
    bots::add_inventory_item(bot, node.yield_item_guid, node.name, yield_qty, node.kind);
    bot.xp_earned += 1250;
    return true;
}

inline bool run_plan_harvest(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16 || s.catalog.resource_nodes.empty()) return false;
    uint32_t kind_mask = 0;
    size_t seq = 0;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            auto node = s.catalog.resource_nodes[seq++ % s.catalog.resource_nodes.size()];
            node.pos_cm = {actor.pos_cm.x + 120.0, actor.pos_cm.y, actor.pos_cm.z};
            node.remaining_harvests = 4;
            const size_t inv_before = actor.inventory.size();
            if (!harvest_resource_node(s, actor, node)) return false;
            if (actor.inventory.size() < inv_before || actor.inventory.empty()) return false;
            kind_mask |= (1u << (static_cast<uint8_t>(node.kind) - 1u));
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok && kind_mask == 0x1Fu;
}

}  // namespace aoc::world::tests::plans
