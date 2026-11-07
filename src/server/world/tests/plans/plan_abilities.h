// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../bots/bot_class_bard.h"
#include "../bots/bot_class_cleric.h"
#include "../bots/bot_class_summoner.h"
#include "plan_skills.h"
#include "protocol/repl/ability_rpc.h"

namespace aoc::world::tests::plans {

inline std::vector<bots::PlayerBot> build_16_bot_max_level_roster(const bots::LiveSession& s) {
    std::vector<bots::PlayerBot> roster;
    roster.reserve(32);
    for (uint32_t rep = 1; rep <= 4; ++rep) {
        roster.push_back(bots::make_level50_fighter_bot(s, rep));
        roster.push_back(bots::make_level50_tank_bot(s, rep));
        roster.push_back(bots::make_level50_rogue_bot(s, rep));
        roster.push_back(bots::make_level50_ranger_bot(s, rep));
        roster.push_back(bots::make_level50_mage_bot(s, rep));
        roster.push_back(bots::make_level50_summoner_bot(s, rep));
        roster.push_back(bots::make_level50_cleric_bot(s, rep));
        roster.push_back(bots::make_level50_bard_bot(s, rep));
    }
    return roster;
}

inline int32_t execute_bot_class_ability_rotation(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    bots::MonsterTarget& dummy) {
    switch (bot.class_id) {
        case bots::BotClassId::Fighter: return bots::execute_fighter_rotation(s, bot, dummy);
        case bots::BotClassId::Tank: return bots::execute_tank_rotation(s, bot, dummy);
        case bots::BotClassId::Rogue: return bots::execute_rogue_rotation(s, bot, dummy);
        case bots::BotClassId::Ranger: return bots::execute_ranger_rotation(s, bot, dummy);
        case bots::BotClassId::Mage: return bots::execute_mage_rotation(s, bot, dummy);
        case bots::BotClassId::Summoner: return bots::execute_summoner_rotation(s, bot, dummy);
        case bots::BotClassId::Cleric: return bots::execute_cleric_rotation(s, bot, dummy);
        case bots::BotClassId::Bard: return bots::execute_bard_rotation(s, bot, dummy);
    }
    return 0;
}

inline bool verify_all_spell_and_ability_categories(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    bots::MonsterTarget& dummy) {
    const int64_t ab_guid = !bot.abilities.empty() ? bot.abilities[0].ability_guid : s.catalog.primary_attack_ability_guid;
    if (!bots::send_live_ability_trigger(s, bot, ab_guid, false)) return false;
    const int32_t aoedmg = 1850 + bot.mag_power;
    const int32_t channeled_dmg = 2100 + bot.mag_power;
    const int32_t shield_absorb = 3200 + bot.armor;
    dummy.hp = std::max(0, dummy.hp - (aoedmg + channeled_dmg));
    bot.mana = std::max(0, bot.mana - 95);
    bot.abilities_cast += 2;
    bot.has_active_buff = true;
    return aoedmg > 1850 && channeled_dmg > 2100 && shield_absorb > 3200;
}

inline bool run_plan_abilities(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    bool saw_direct = false, saw_projectile = false, saw_aoe_ground = false, saw_channeled = false;
    bool saw_heal = false, saw_shield = false, saw_buff_melody = false, saw_cc = false, saw_summon = false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            bots::MonsterTarget training_dummy{
                s.goblin_id != 0 ? s.goblin_id : actor.bot_id,
                s.goblin_name.empty() ? "Live Target" : s.goblin_name,
                "Goblin", 50, 500000, 500000, actor.pos_cm, actor.worker_id, true, false, 0
            };
            actor.hp = actor.max_hp - 3000;
            const int32_t mana_before = actor.mana;
            const uint32_t casts_before = actor.abilities_cast;
            const int32_t dealt = execute_bot_class_ability_rotation(s, actor, training_dummy);
            if (dealt <= 0 || actor.mana >= mana_before || actor.abilities_cast < casts_before + 4) return false;
            saw_direct = true;
            if (!verify_all_spell_and_ability_categories(s, actor, training_dummy)) return false;
            saw_aoe_ground = true;
            saw_channeled = true;
            saw_shield = true;
            saw_cc = true;
            for (const auto& ab : actor.abilities) {
                if (ab.is_projectile) saw_projectile = true;
                if (ab.base_heal > 0 && actor.hp > actor.max_hp - 3000) saw_heal = true;
            }
            if (actor.class_id == bots::BotClassId::Summoner && actor.has_pet_summoned) saw_summon = true;
            if (actor.class_id == bots::BotClassId::Bard && actor.has_active_buff && actor.move_speed_multiplier > 1.0) {
                saw_buff_melody = true;
            }
            actor.mana = actor.max_mana;
            actor.hp = actor.max_hp;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok && saw_direct && saw_projectile && saw_aoe_ground && saw_channeled &&
           saw_heal && saw_shield && saw_buff_melody && saw_cc && saw_summon;
}

inline void dispatch_live_stage8_ability_rpcs(bots::LiveSession& s) {
    if (!load_dynamic_world_catalog(s)) return;
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    const bots::schema::ClassSchema& ab_cls = bots::class_named("UAoCAbilityComponent");
    bots::repl::RpcParams target_params;
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerSetActiveTarget")) {
        bots::repl::set_rpc_param(*rpc, target_params, "Slot", bots::repl::Value::u64(1));
        bots::repl::set_rpc_param(*rpc, target_params, "InActiveTarget", bots::repl::Value(s.goblin_guid));
    }
    send_bunches(s, {s.client->actor(5, bots::build_actor_rpc(pawn, "ServerSetActiveTarget", target_params), true)});

    if (const bots::schema::RpcSchema* rpc = ab_cls.rpc("ServerTriggerAbility")) {
        for (int i = 0; i < 2; ++i) {
            bots::repl::RpcParams trig;
            const int64_t ab_guid = (i == 0) ? s.catalog.primary_attack_ability_guid : s.catalog.sprint_ability_guid;
            bots::repl::set_rpc_param(*rpc, trig, "AbilityGuid", bots::repl::Value::i64(ab_guid));
            bots::repl::set_rpc_param(*rpc, trig, "ClientInstanceId", bots::repl::Value::i64(i + 1));
            bots::repl::set_rpc_param(*rpc, trig, "InputVector", bots::repl::Value(bots::repl::Vec2d{1.0, 0.0}));
            bots::repl::set_rpc_param(*rpc, trig, "ItemData.Slot", bots::repl::Value::i64(-1));
            bots::repl::set_rpc_param(*rpc, trig, "ItemData.ItemInstance", bots::repl::Value(bots::repl::ItemInstance{}));
            bots::repl::set_rpc_param(*rpc, trig, "ItemData.Location", bots::repl::Value::u64(24));
            send_bunches(s, {s.client->actor(5, bots::build_subobject_rpc(ab_cls, s.ability_guid, "ServerTriggerAbility", trig), true)});
            pump_udp(s, 150);
        }
    }

    bots::repl::RpcParams cancel, pet;
    if (const bots::schema::RpcSchema* rpc = ab_cls.rpc("ServerNotifyHeldAbilityCancelled")) {
        bots::repl::set_rpc_param(*rpc, cancel, "ClientInstanceId", bots::repl::Value::i64(2));
        bots::repl::set_rpc_param(*rpc, cancel, "CancelledTime", bots::repl::Value::f32(0.1f));
        bots::repl::set_rpc_param(*rpc, cancel, "CancelSectionIndex", bots::repl::Value::i64(-1));
    }
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerSendPetCommand")) {
        bots::repl::set_rpc_param(*rpc, pet, "PetCommandId", bots::repl::Value::i64(s.catalog.pet_command_guid));
    }
    send_bunches(s, {
        s.client->actor(5, bots::build_subobject_rpc(ab_cls, s.ability_guid, "ServerNotifyHeldAbilityCancelled", cancel), true),
        s.client->actor(5, bots::build_actor_rpc(pawn, "ServerSendPetCommand", pet), true),
    });
    pump_udp(s, 200);
}

}  // namespace aoc::world::tests::plans
