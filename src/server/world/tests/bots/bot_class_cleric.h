// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_cleric_bot(uint32_t replica_idx) {
    PlayerBot bot{};
    bot.bot_id = 7000ULL + replica_idx;
    bot.replica_index = replica_idx;
    bot.name = replica_idx == 1 ? "TST ClericAlpha" : "TST ClericBravo";
    bot.class_id = BotClassId::Cleric;
    bot.class_name = "Cleric";
    bot.level = kMaxBotLevel;
    bot.max_hp = 23800;
    bot.hp = bot.max_hp;
    bot.max_mana = 13200;
    bot.mana = bot.max_mana;
    bot.base_phys_power = 640;
    bot.base_mag_power = 1620;
    bot.base_armor = 1760;
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.loadout_presets = {
        {"MainHand", "Sanctified Dawnmace", 606463110000000701LL, 160, 480, 80, 0},
        {"OffHand", "Reliquary Buckler", 606463110000000702LL, 40, 260, 380, 0},
        {"ChestArmor", "Templar Radiant Hauberk", 606463110000000703LL, 30, 140, 480, 0},
        {"GatheringTool", "Blessed Herbalism Sickle", 606463110000000704LL, 0, 30, 0, 40},
    };
    bot.skill_tree_presets = {
        {"Cleric_DivineGrace_Rank5", 606462910000000701LL, 606462980000000700LL, true, 230},
        {"Cleric_Deliverance_Unlock", 606462910000000702LL, 606462980000000700LL, false, 180},
        {"Cleric_JudgmentSmite_Unlock", 606462910000000703LL, 606462980000000700LL, false, 160},
        {"Cleric_SacredBarrier_Passive", 606462910000000704LL, 606462980000000700LL, true, 170},
    };
    bot.abilities = {
        {"JudgmentSmiteBolt", 606463180000000701LL, 3000.0, 2800, 0, 200, 1400, true, false},
        {"DeliveranceHealRay", 606463180000000702LL, 3000.0, 1600, 4200, 280, 2500, false, true},
        {"PurifyingNovaAoE", 606463180000000703LL, 1200.0, 3100, 2600, 320, 5000, false, true},
        {"SacredBlessingWard", 606463180000000704LL, 2500.0, 1400, 3200, 260, 8000, false, true},
    };
    return bot;
}

inline int32_t execute_cleric_rotation(PlayerBot& bot, MonsterTarget& target) {
    if (bot.dead || bot.level != kMaxBotLevel) return 0;
    int32_t total_dmg = 0;
    for (const auto& ab : bot.abilities) {
        if (bot.mana < ab.mana_cost) break;
        bot.mana -= ab.mana_cost;
        if (ab.is_buff_or_summon) bot.has_active_buff = true;
        if (ab.base_heal > 0) bot.hp = std::min(bot.max_hp, bot.hp + ab.base_heal);
        const int32_t hit = ab.base_damage + (bot.mag_power / 2);
        target.hp = std::max(0, target.hp - hit);
        total_dmg += hit;
        ++bot.abilities_cast;
    }
    return total_dmg;
}

}  // namespace aoc::world::tests::bots
