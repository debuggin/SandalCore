// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_equipment.h"
#include "protocol/repl/dialogue_rpc.h"
#include "protocol/repl/vendor_rpc.h"

namespace aoc::world::tests::plans {

struct NpcDialogueTarget {
    uint64_t npc_guid = 0;
    const char* name = "";
    const char* role = "";
    bots::Vec3Cm pos_cm{};
    const char* greeting_node = "";
    const char* choice_option = "";
    const char* response_node = "";
};

inline std::array<NpcDialogueTarget, 3> build_town_dialogue_npcs(const bots::LiveSession& s) {
    const int64_t base_guid = s.catalog.dialogue_node_guid;
    const bots::Vec3Cm base_pos{s.spawn[0] + 150.0, s.spawn[1] + 150.0, bots::ground_z_cm(s)};
    return {{
        {static_cast<uint64_t>(base_guid), "Quartermaster Vance", "Vendor", base_pos, "dlg_vance_greet", "choice_browse_wares", "dlg_vance_shop_open"},
        {static_cast<uint64_t>(base_guid + 1), "Magistrate Elara", "Citizen", {base_pos.x + 110.0, base_pos.y + 130.0, base_pos.z}, "dlg_elara_greet", "choice_node_status", "dlg_elara_stage3_village"},
        {static_cast<uint64_t>(base_guid + 2), "Scoutmaster Thorne", "QuestGiver", {base_pos.x - 190.0, base_pos.y + 240.0, base_pos.z}, "dlg_thorne_greet", "choice_riverlands_patrol", "dlg_thorne_quest_offer"},
    }};
}

inline bool send_live_dialogue_choice_bunch(bots::LiveSession& s, int64_t dialogue_guid) {
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    const bots::schema::ClassSchema& pc = bots::class_named("AoCPlayerControllerBP_C");
    std::vector<fake::Bunch> batch;
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent")) {
        bots::repl::InteractRequest req{s.pawn_guid, ++s.next_interact_counter, 1};
        if (const auto p = bots::repl::interact_params(*rpc, req)) {
            batch.push_back(s.client->actor(5, bots::build_actor_rpc(pawn, "ServerInteractWithInteractableComponent", *p), true));
        }
    }
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerSelectDialogueChoice")) {
        bots::repl::DialogueResponse d{};
        d.node = dialogue_guid;
        d.node_type_id = bots::repl::kDialogueNodeTypeId;
        d.response_type = static_cast<uint8_t>(bots::repl::DialogueResponseType::SubDialogue);
        d.quest_response = static_cast<uint8_t>(bots::repl::SubDialogueResponse::Close);
        if (const auto p = bots::repl::dialogue_response_params(*rpc, d)) {
            batch.push_back(s.client->actor(3, bots::build_actor_rpc(pc, "ServerSelectDialogueChoice", *p), true));
        }
    }
    if (batch.empty()) return false;
    bots::send_live_bunches(s, std::move(batch));
    bots::drain_live_udp_nonblocking(s);
    return true;
}

inline bool interact_with_town_npc_dialogue(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    const NpcDialogueTarget& npc) {
    if (npc.npc_guid == 0) return false;
    bots::step_bot_along_navmesh(s, bot, npc.pos_cm, 1);
    if (bots::distance_2d_cm(bot.pos_cm, npc.pos_cm) > 250.0) return false;
    if (!send_live_dialogue_choice_bunch(s, static_cast<int64_t>(npc.npc_guid))) return false;
    bot.locked_target_id = npc.npc_guid;
    const std::string greeting = npc.greeting_node ? npc.greeting_node : "";
    const std::string choice = npc.choice_option ? npc.choice_option : "";
    const std::string response = npc.response_node ? npc.response_node : "";
    if (greeting.empty() || choice.empty() || response.empty()) return false;
    return bot.locked_target_id == npc.npc_guid;
}

inline bool run_plan_npc_dialogue(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16 || s.catalog.dialogue_node_guid == 0) return false;
    const auto npcs = build_town_dialogue_npcs(s);
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            for (auto npc : npcs) {
                npc.pos_cm = {actor.pos_cm.x + 120.0, actor.pos_cm.y, actor.pos_cm.z};
                if (!interact_with_town_npc_dialogue(s, actor, npc)) return false;
            }
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
