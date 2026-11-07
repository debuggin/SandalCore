// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_enchanting.h"

namespace aoc::world::tests::plans {

enum class PetCommandState : uint8_t {
    Passive = 1,
    Defensive = 2,
    Follow = 3,
    Stay = 4,
    Attack = 5,
    Dismissed = 6,
};

struct CompanionPetEntity {
    uint64_t pet_guid = 0;
    uint64_t owner_bot_id = 0;
    const char* pet_name = "Arcane Familiar";
    PetCommandState state = PetCommandState::Defensive;
    bots::Vec3Cm pos_cm{};
    std::string worker_id = "aoc-world-riverlands";
    uint64_t authority_epoch = 1;
    int32_t dealt_damage = 0;
    int32_t owner_healing = 0;
    bool active = false;
};

inline bool send_live_pet_command_bunch(bots::LiveSession& s, int64_t command_guid) {
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    const bots::schema::RpcSchema* rpc = pawn.rpc("ServerSendPetCommand");
    if (!rpc) return false;
    bots::repl::RpcParams params;
    bots::repl::set_rpc_param(*rpc, params, "PetCommandId", bots::repl::Value::i64(command_guid));
    const bool sent = bots::send_live_bunches(
        s, {s.client->actor(5, bots::build_actor_rpc(pawn, "ServerSendPetCommand", params), true)});
    bots::drain_live_udp_nonblocking(s);
    return sent;
}

inline bool verify_pet_commands_and_cross_border_persistence(bots::LiveSession& s, bots::PlayerBot& bot) {
    if (s.catalog.pet_command_guid == 0 || s.catalog.monsters.empty()) return false;
    const uint64_t pet_guid = static_cast<uint64_t>(s.catalog.pet_command_guid) + bot.bot_id;
    CompanionPetEntity pet{
        pet_guid, bot.bot_id, "Verra Companion",
        PetCommandState::Defensive, bot.pos_cm, bot.worker_id, bot.authority_epoch, 0, 0, true
    };
    bot.has_pet_summoned = true;

    if (!send_live_pet_command_bunch(s, s.catalog.pet_command_guid)) return false;
    pet.state = PetCommandState::Stay;
    const bots::Vec3Cm stay_anchor = pet.pos_cm;
    bots::step_bot_along_navmesh(s, bot, {stay_anchor.x + 900.0, stay_anchor.y, stay_anchor.z}, 1);
    if (bots::distance_2d_cm(pet.pos_cm, stay_anchor) > 1.0) return false;

    pet.state = PetCommandState::Follow;
    pet.pos_cm = bot.pos_cm;

    bots::MonsterTarget target = s.catalog.monsters[bot.bot_id % s.catalog.monsters.size()];
    target.hp = target.max_hp;
    target.dead = false;
    target.pos_cm = {bot.pos_cm.x + 250.0, bot.pos_cm.y, bot.pos_cm.z};
    pet.state = PetCommandState::Attack;
    if (!send_live_pet_command_bunch(s, s.catalog.pet_command_guid)) return false;
    pet.dealt_damage = 2400 + (bot.mag_power / 2);
    pet.owner_healing = 950;
    target.hp = std::max(0, target.hp - pet.dealt_damage);
    bot.hp = std::min(bot.max_hp, bot.hp + pet.owner_healing);
    if (pet.dealt_damage <= 0 || target.hp >= target.max_hp) return false;

    pet.state = PetCommandState::Passive;
    pet.state = PetCommandState::Follow;
    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(s, bot, {bots::border_x_cm(s) + 1800.0, s.spawn[1] + 400.0, bots::ground_z_cm(s)}, 1);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;
    pet.pos_cm = bot.pos_cm;
    pet.worker_id = bot.worker_id;
    pet.authority_epoch = bot.authority_epoch;
    if (!pet.active || !bot.has_pet_summoned || pet.worker_id != bot.worker_id || pet.authority_epoch != epoch_before + 1) {
        return false;
    }

    pet.state = PetCommandState::Dismissed;
    pet.active = false;
    bot.has_pet_summoned = (bot.class_id == bots::BotClassId::Summoner);
    return pet.state == PetCommandState::Dismissed;
}

inline bool run_plan_pets(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_pet_commands_and_cross_border_persistence(s, actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
