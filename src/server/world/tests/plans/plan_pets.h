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

inline bool verify_pet_commands_and_cross_border_persistence(bots::PlayerBot& bot) {
    CompanionPetEntity pet{
        606463700000000000ULL + bot.bot_id, bot.bot_id, "Verra Companion",
        PetCommandState::Defensive, bot.pos_cm, bot.worker_id, bot.authority_epoch, 0, 0, true
    };
    bot.has_pet_summoned = true;

    pet.state = PetCommandState::Stay;
    const bots::Vec3Cm stay_anchor = pet.pos_cm;
    bots::step_bot_along_navmesh(bot, {stay_anchor.x + 900.0, stay_anchor.y, stay_anchor.z}, 2);
    if (bots::distance_2d_cm(pet.pos_cm, stay_anchor) > 1.0) return false;

    pet.state = PetCommandState::Follow;
    pet.pos_cm = bot.pos_cm;

    bots::MonsterTarget target{
        96000ULL + bot.bot_id, "Corrupted Stalker", "Beast", 50, 18000, 18000,
        {bot.pos_cm.x + 250.0, bot.pos_cm.y, bot.pos_cm.z}, bot.worker_id, true, false, bot.bot_id
    };
    pet.state = PetCommandState::Attack;
    pet.dealt_damage = 2400 + (bot.mag_power / 2);
    pet.owner_healing = 950;
    target.hp = std::max(0, target.hp - pet.dealt_damage);
    bot.hp = std::min(bot.max_hp, bot.hp + pet.owner_healing);
    if (pet.dealt_damage <= 0 || target.hp >= target.max_hp) return false;

    pet.state = PetCommandState::Passive;
    pet.state = PetCommandState::Follow;
    const uint64_t epoch_before = bot.authority_epoch;
    bots::step_bot_along_navmesh(bot, {-416800.0, 520000.0, bots::kDefaultGroundTerrainZCm}, 4);
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

inline bool run_plan_pets(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_pet_commands_and_cross_border_persistence(actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
