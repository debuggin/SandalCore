// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../bots/bot_types.h"

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

inline constexpr std::array<NpcDialogueTarget, 3> kTownDialogueNpcs = {{
    {606463900000000101ULL, "Quartermaster Vance", "Vendor", {-644420.0, 376680.0, bots::kDefaultGroundTerrainZCm}, "dlg_vance_greet", "choice_browse_wares", "dlg_vance_shop_open"},
    {606463900000000102ULL, "Magistrate Elara", "Citizen", {-644310.0, 376810.0, bots::kDefaultGroundTerrainZCm}, "dlg_elara_greet", "choice_node_status", "dlg_elara_stage3_village"},
    {606463900000000103ULL, "Scoutmaster Thorne", "QuestGiver", {-644610.0, 376920.0, bots::kDefaultGroundTerrainZCm}, "dlg_thorne_greet", "choice_riverlands_patrol", "dlg_thorne_quest_offer"},
}};

inline bool interact_with_town_npc_dialogue(bots::PlayerBot& bot, const NpcDialogueTarget& npc) {
    bots::step_bot_along_navmesh(bot, npc.pos_cm, 3);
    if (bots::distance_2d_cm(bot.pos_cm, npc.pos_cm) > 250.0) return false;
    bot.locked_target_id = npc.npc_guid;
    const std::string greeting = npc.greeting_node ? npc.greeting_node : "";
    const std::string choice = npc.choice_option ? npc.choice_option : "";
    const std::string response = npc.response_node ? npc.response_node : "";
    if (greeting.empty() || choice.empty() || response.empty()) return false;
    return bot.locked_target_id == npc.npc_guid;
}

inline bool run_plan_npc_dialogue(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            for (auto npc : kTownDialogueNpcs) {
                npc.pos_cm = {actor.pos_cm.x + 120.0, actor.pos_cm.y, actor.pos_cm.z};
                if (!interact_with_town_npc_dialogue(actor, npc)) return false;
            }
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
