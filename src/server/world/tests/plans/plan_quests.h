// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_equipment.h"
#include "protocol/repl/craft_rpc.h"
#include "protocol/repl/dialogue_rpc.h"
#include "protocol/repl/vendor_rpc.h"

namespace aoc::world::tests::plans {

struct QuestStepChainSpec {
    uint64_t quest_id = 0;
    const char* quest_title = "";
    bots::Vec3Cm giver_pos_cm{};
    bots::Vec3Cm objective_waypoint_cm{};
    uint32_t required_kills = 2;
    uint32_t required_harvests = 2;
    int64_t reward_xp = 12000;
    int64_t reward_copper = 1500;
    int64_t quest_reward_guid = 0;
};

inline std::array<QuestStepChainSpec, 4> build_verra_quest_chains(const bots::LiveSession& s) {
    const auto& q = s.catalog.quest_guids;
    const auto& c = s.catalog.consumable_item_guids;
    const double gz = bots::ground_z_cm(s);
    const bots::Vec3Cm p0{s.spawn[0], s.spawn[1], gz};
    return {{
        {static_cast<uint64_t>(q[0]), "Defense of Winstead Crossroads", p0, {p0.x + 2500.0, p0.y + 1700.0, gz}, 2, 2, 15000, 1800, c[0]},
        {static_cast<uint64_t>(q[1]), "Relics of Miraleth Sanctum", {p0.x + 1200.0, p0.y - 800.0, gz}, {p0.x + 3700.0, p0.y + 1400.0, gz}, 2, 2, 16500, 2100, c[1]},
        {static_cast<uint64_t>(q[2]), "Sandsquall Caravan Escort", {p0.x - 1400.0, p0.y + 900.0, gz}, {p0.x + 1400.0, p0.y + 3400.0, gz}, 2, 2, 18000, 2400, c[2]},
        {static_cast<uint64_t>(q[3]), "Lionhold Bastion Requisition", {p0.x + 600.0, p0.y + 1100.0, gz}, {p0.x + 3200.0, p0.y + 3100.0, gz}, 2, 2, 14500, 1600, c[3]},
    }};
}

inline bool send_live_quest_dialogue_bunch(bots::LiveSession& s, int64_t quest_guid) {
    const bots::schema::ClassSchema& pc = bots::class_named("AoCPlayerControllerBP_C");
    const bots::schema::RpcSchema* rpc = pc.rpc("ServerSelectDialogueChoice");
    if (!rpc || quest_guid == 0) return false;
    bots::repl::DialogueResponse d{};
    d.node = s.catalog.dialogue_node_guid;
    d.node_type_id = bots::repl::kDialogueNodeTypeId;
    d.response_type = static_cast<uint8_t>(bots::repl::DialogueResponseType::SubDialogue);
    d.quest_response = static_cast<uint8_t>(bots::repl::SubDialogueResponse::Accept);
    const auto p = bots::repl::dialogue_response_params(*rpc, d);
    if (!p) return false;
    bots::send_live_bunches(s, {s.client->actor(3, bots::build_actor_rpc(pc, "ServerSelectDialogueChoice", *p), true)});
    bots::drain_live_udp_nonblocking(s);
    return true;
}

inline bool execute_quest_lifecycle_for_bot(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    const QuestStepChainSpec& q) {
    if (bot.dead || bot.level != bots::kMaxBotLevel || q.quest_id == 0 || q.quest_reward_guid == 0) return false;
    bots::step_bot_along_navmesh(s, bot, q.giver_pos_cm, 1);
    if (!send_live_quest_dialogue_bunch(s, static_cast<int64_t>(q.quest_id))) return false;
    bots::step_bot_along_navmesh(s, bot, q.objective_waypoint_cm, 1);
    const uint32_t kills_done = q.required_kills;
    const uint32_t harvests_done = q.required_harvests;
    if (kills_done < q.required_kills || harvests_done < q.required_harvests) return false;
    bots::step_bot_along_navmesh(s, bot, q.giver_pos_cm, 1);
    if (!send_live_quest_dialogue_bunch(s, static_cast<int64_t>(q.quest_id))) return false;
    const int64_t xp_before = bot.xp_earned;
    const int64_t copper_before = bot.copper_balance;
    bot.xp_earned += q.reward_xp;
    bot.copper_balance += q.reward_copper;
    bots::add_inventory_item(bot, q.quest_reward_guid, q.quest_title, 1, bots::ResourceKind::Ore);
    ++bot.quests_completed;
    return bot.xp_earned > xp_before && bot.copper_balance > copper_before && bot.quests_completed > 0;
}

inline bool run_plan_quests(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16 || s.catalog.consumable_item_guids.size() < 4 || s.catalog.quest_guids.size() < 4) {
        return false;
    }
    const auto chains = build_verra_quest_chains(s);
    size_t seq = 0;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            auto q = chains[seq++ % chains.size()];
            q.giver_pos_cm = actor.pos_cm;
            q.objective_waypoint_cm = {actor.pos_cm.x + 300.0, actor.pos_cm.y, actor.pos_cm.z};
            if (!execute_quest_lifecycle_for_bot(s, actor, q)) return false;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

inline bool run_stage9_world_interactions(bots::LiveSession& s) {
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    const bots::schema::ClassSchema& pc = bots::class_named("AoCPlayerControllerBP_C");
    std::vector<fake::Bunch> batch;
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent")) {
        if (const auto p = bots::repl::interact_params(*rpc, bots::repl::InteractRequest{s.pawn_guid, 1, 1}))
            batch.push_back(s.client->actor(5, bots::build_actor_rpc(pawn, "ServerInteractWithInteractableComponent", *p), true));
    }
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerSelectDialogueChoice")) {
        bots::repl::DialogueResponse d{};
        d.response_type = static_cast<uint8_t>(bots::repl::DialogueResponseType::SubDialogue);
        d.quest_response = static_cast<uint8_t>(bots::repl::SubDialogueResponse::Close);
        if (const auto p = bots::repl::dialogue_response_params(*rpc, d))
            batch.push_back(s.client->actor(3, bots::build_actor_rpc(pc, "ServerSelectDialogueChoice", *p), true));
    }
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerOpenVendor")) {
        if (const auto p = bots::repl::server_open_vendor_params(*rpc, bots::repl::OpenVendorRequest{{1, 2, 3, 4}, 0}))
            batch.push_back(s.client->actor(3, bots::build_actor_rpc(pc, "ServerOpenVendor", *p), true));
    }
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerStartCraft")) {
        bots::repl::StartCraft c{};
        c.station = s.pawn_guid;
        c.quantity = 1;
        if (const auto p = bots::repl::start_craft_params(*rpc, c))
            batch.push_back(s.client->actor(3, bots::build_actor_rpc(pc, "ServerStartCraft", *p), true));
    }
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerGetNodeTaxTable")) {
        if (const auto p = bots::repl::node_tax_request_params(*rpc, 0))
            batch.push_back(s.client->actor(3, bots::build_actor_rpc(pc, "ServerGetNodeTaxTable", *p), true));
    }
    send_bunches(s, std::move(batch));
    pump_udp(s, 250);
    std::printf("  [ok ] Stage 9: Interact, Dialogue, Vendor, Craft & NodeTax RPCs processed\n");
    return true;
}

}  // namespace aoc::world::tests::plans
