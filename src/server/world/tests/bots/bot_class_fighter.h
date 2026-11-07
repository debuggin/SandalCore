// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_fighter_bot(uint32_t replica_idx) {
    PlayerBot bot{};
    bot.bot_id = 1000ULL + replica_idx;
    bot.replica_index = replica_idx;
    bot.name = replica_idx == 1 ? "TST FighterAlpha" : "TST FighterBravo";
    bot.class_id = BotClassId::Fighter;
    bot.class_name = "Fighter";
    bot.level = kMaxBotLevel;
    bot.max_hp = 26500;
    bot.hp = bot.max_hp;
    bot.max_mana = 7200;
    bot.mana = bot.max_mana;
    bot.base_phys_power = 1480;
    bot.base_mag_power = 320;
    bot.base_armor = 1850;
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.loadout_presets = {
        {"MainHand", "Verra Vanguard Greatsword", 606463110000000101LL, 460, 0, 40, 0},
        {"OffHand", "Tempered Warbanner", 606463110000000102LL, 190, 0, 120, 0},
        {"ChestArmor", "Lionhold Plate Cuirass", 606463110000000103LL, 85, 0, 520, 0},
        {"GatheringTool", "Master Logging Axe", 606463110000000104LL, 20, 0, 0, 35},
    };
    bot.skill_tree_presets = {
        {"Fighter_WeaponMastery_Rank5", 606462910000000101LL, 606462980000000100LL, true, 180},
        {"Fighter_Whirlwind_Unlock", 606462910000000102LL, 606462980000000100LL, false, 120},
        {"Fighter_Overpower_Unlock", 606462910000000103LL, 606462980000000100LL, false, 150},
        {"Fighter_BattleFrenzy_Passive", 606462910000000104LL, 606462980000000100LL, true, 140},
    };
    bot.abilities = {
        {"CleaveCombo", 606463180000000101LL, 350.0, 2600, 0, 120, 1000, false, false},
        {"WhirlwindSlash", 606463180000000102LL, 400.0, 3400, 0, 220, 4000, false, false},
        {"OverpowerStrike", 606463180000000103LL, 350.0, 4200, 0, 280, 6000, false, false},
        {"BlitzCharge", 606463180000000104LL, 1800.0, 1900, 0, 180, 8000, false, true},
    };
    return bot;
}

inline int32_t execute_fighter_rotation(PlayerBot& bot, MonsterTarget& target) {
    if (bot.dead || bot.level != kMaxBotLevel) return 0;
    int32_t total_dmg = 0;
    for (const auto& ab : bot.abilities) {
        if (bot.mana < ab.mana_cost) break;
        bot.mana -= ab.mana_cost;
        if (ab.is_buff_or_summon) bot.has_active_buff = true;
        const int32_t hit = ab.base_damage + (bot.phys_power / 2);
        target.hp = std::max(0, target.hp - hit);
        total_dmg += hit;
        ++bot.abilities_cast;
    }
    return total_dmg;
}

}  // namespace aoc::world::tests::bots
