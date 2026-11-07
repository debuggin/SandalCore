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
    int64_t reward_item_guid = 0;
};

inline std::array<QuestStepChainSpec, 4> build_verra_quest_chains() {
    return {{
        {5001ULL, "Defense of Winstead Crossroads", {-644519.0, 376773.0, 11400.0}, {-642000.0, 378500.0, 11400.0}, 2, 2, 15000, 1800, 606463410000000001LL},
        {5002ULL, "Relics of Miraleth Sanctum", {-592000.0, 342000.0, 11200.0}, {-589500.0, 344200.0, 11200.0}, 2, 2, 16500, 2100, 606463410000000002LL},
        {5003ULL, "Sandsquall Caravan Escort", {-418000.0, 682000.0, 9800.0}, {-415200.0, 684500.0, 9800.0}, 2, 2, 18000, 2400, 606463410000000003LL},
        {5004ULL, "Lionhold Bastion Requisition", {-685000.0, 412000.0, 12500.0}, {-682400.0, 414000.0, 12500.0}, 2, 2, 14500, 1600, 606463410000000004LL},
    }};
}

inline bool execute_quest_lifecycle_for_bot(bots::PlayerBot& bot, const QuestStepChainSpec& q) {
    if (bot.dead || bot.level != bots::kMaxBotLevel) return false;
    bots::step_bot_along_navmesh(bot, q.giver_pos_cm, 3);
    bots::step_bot_along_navmesh(bot, q.objective_waypoint_cm, 4);
    const uint32_t kills_done = q.required_kills;
    const uint32_t harvests_done = q.required_harvests;
    if (kills_done < q.required_kills || harvests_done < q.required_harvests) return false;
    bots::step_bot_along_navmesh(bot, q.giver_pos_cm, 4);
    const int64_t xp_before = bot.xp_earned;
    const int64_t copper_before = bot.copper_balance;
    bot.xp_earned += q.reward_xp;
    bot.copper_balance += q.reward_copper;
    bots::add_inventory_item(bot, q.reward_item_guid, q.quest_title, 1, bots::ResourceKind::Ore);
    ++bot.quests_completed;
    return bot.xp_earned > xp_before && bot.copper_balance > copper_before && bot.quests_completed > 0;
}

inline bool run_plan_quests(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const auto chains = build_verra_quest_chains();
    size_t seq = 0;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            auto q = chains[seq++ % chains.size()];
            q.giver_pos_cm = actor.pos_cm;
            q.objective_waypoint_cm = {actor.pos_cm.x + 300.0, actor.pos_cm.y, actor.pos_cm.z};
            if (!execute_quest_lifecycle_for_bot(actor, q)) return false;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
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
