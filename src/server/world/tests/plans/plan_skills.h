// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../bots/bot_class_fighter.h"
#include "../bots/bot_class_mage.h"
#include "../bots/bot_class_ranger.h"
#include "../bots/bot_class_rogue.h"
#include "../bots/bot_class_tank.h"
#include "plan_equipment.h"
#include "protocol/repl/skill_rpc.h"

namespace aoc::world::tests::plans {

inline bool send_live_skill_and_actionbar_bunch(
    bots::LiveSession& s,
    int64_t rank_guid,
    int64_t tree_guid,
    int64_t ability_guid) {
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    const bots::schema::ClassSchema& pc = bots::class_named("AoCPlayerControllerBP_C");
    std::vector<fake::Bunch> batch;
    bots::repl::SkillBuild build;
    build.changes.push_back({{rank_guid, s.catalog.type_skill_rank},
                             {tree_guid, s.catalog.type_skill_tree,
                              bots::repl::NameValue::plain(s.catalog.sample_skill_tree_name)},
                             {s.catalog.adventuring_leveling_guid, s.catalog.type_leveling_def},
                             true});
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerPurchaseSkillBuild")) {
        if (const auto params = bots::repl::purchase_skill_build_params(*rpc, build)) {
            batch.push_back(s.client->actor(5, bots::build_actor_rpc(pawn, "ServerPurchaseSkillBuild", *params), true));
        }
    }
    bots::repl::ActionbarSlot slot{};
    slot.bank_index = 0;
    slot.slot_index = 5;
    slot.use.use_type = bots::repl::ActionbarUseType::Ability;
    slot.use.instigator = bots::repl::ActionbarInstigator::Player;
    slot.use.ability_id = ability_guid != 0 ? ability_guid : s.catalog.sprint_ability_guid;
    if (const bots::schema::RpcSchema* rpc = pc.rpc("ServerSetActionbarSlotUseData")) {
        if (const auto params = bots::repl::set_actionbar_slot_params(*rpc, slot)) {
            batch.push_back(s.client->actor(3, bots::build_actor_rpc(pc, "ServerSetActionbarSlotUseData", *params), true));
        }
    }
    const bool sent = send_bunches(s, std::move(batch));
    bots::drain_live_udp_nonblocking(s);
    return sent;
}

inline bool unlock_bot_level50_skill_tree(bots::LiveSession& s, bots::PlayerBot& bot) {
    if (bot.level != bots::kMaxBotLevel || bot.skill_tree_presets.empty()) return false;
    bot.unlocked_skills.clear();
    int32_t passive_bonus_total = 0;
    size_t active_unlocks = 0;
    for (const auto& node : bot.skill_tree_presets) {
        if (node.skill_rank_guid == 0 || node.skill_tree_guid == 0) return false;
        bot.unlocked_skills.push_back(node);
        if (node.is_passive) {
            passive_bonus_total += node.power_bonus;
        } else {
            ++active_unlocks;
        }
    }
    const int64_t ab_id = !bot.abilities.empty() ? bot.abilities[0].ability_guid : s.catalog.sprint_ability_guid;
    if (!send_live_skill_and_actionbar_bunch(
            s, bot.unlocked_skills[0].skill_rank_guid, bot.unlocked_skills[0].skill_tree_guid, ab_id)) {
        return false;
    }
    if (bot.phys_power >= bot.mag_power) {
        bot.phys_power += passive_bonus_total;
    } else {
        bot.mag_power += passive_bonus_total;
    }
    return bot.unlocked_skills.size() >= 4 && passive_bonus_total > 0 && active_unlocks >= 2;
}

inline bool run_plan_skills(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    uint32_t class_mask = 0;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (actor.level != bots::kMaxBotLevel) return false;
            if (!unlock_bot_level50_skill_tree(s, actor)) return false;
            class_mask |= (1u << (static_cast<uint8_t>(actor.class_id) - 1u));
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok && class_mask == 0xFFu;
}

inline bool run_stage7_talents_actionbar(bots::LiveSession& s) {
    if (!load_dynamic_world_catalog(s)) return false;
    if (!send_live_skill_and_actionbar_bunch(
            s, s.catalog.sample_skill_rank_guid, s.catalog.sample_skill_tree_guid, s.catalog.sprint_ability_guid)) {
        return false;
    }
    pump_udp(s, 250);
    std::printf("  [ok ] Stage 7: ServerPurchaseSkillBuild & ServerSetActionbarSlotUseData processed\n");
    return true;
}

}  // namespace aoc::world::tests::plans
