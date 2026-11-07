// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_ranger_bot(uint32_t replica_idx) {
    PlayerBot bot{};
    bot.bot_id = 4000ULL + replica_idx;
    bot.replica_index = replica_idx;
    bot.name = replica_idx == 1 ? "TST RangerAlpha" : "TST RangerBravo";
    bot.class_id = BotClassId::Ranger;
    bot.class_name = "Ranger";
    bot.level = kMaxBotLevel;
    bot.max_hp = 22000;
    bot.hp = bot.max_hp;
    bot.max_mana = 8200;
    bot.mana = bot.max_mana;
    bot.base_phys_power = 1620;
    bot.base_mag_power = 410;
    bot.base_armor = 1380;
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.loadout_presets = {
        {"MainHand", "Windrunner Greatbow", 606463110000000401LL, 510, 0, 20, 0},
        {"OffHand", "Briarmoor Hunting Spear", 606463110000000402LL, 280, 0, 40, 0},
        {"ChestArmor", "Strider Brigandine Coat", 606463110000000403LL, 95, 0, 360, 0},
        {"GatheringTool", "Master Hunting Bowknife", 606463110000000404LL, 30, 0, 0, 45},
    };
    bot.skill_tree_presets = {
        {"Ranger_HawkEye_Rank5", 606462910000000401LL, 606462980000000400LL, true, 210},
        {"Ranger_SnipeShot_Unlock", 606462910000000402LL, 606462980000000400LL, false, 180},
        {"Ranger_ArrowBarrage_Unlock", 606462910000000403LL, 606462980000000400LL, false, 160},
        {"Ranger_WildStride_Passive", 606462910000000404LL, 606462980000000400LL, true, 150},
    };
    bot.abilities = {
        {"PowershotArrow", 606463180000000401LL, 3500.0, 2900, 0, 140, 1200, true, false},
        {"PrecisionSnipe", 606463180000000402LL, 4200.0, 4800, 0, 260, 5000, true, false},
        {"TempestBarrage", 606463180000000403LL, 3200.0, 3500, 0, 220, 4000, true, false},
        {"HawkAspectBuff", 606463180000000404LL, 100.0, 1200, 0, 150, 8000, false, true},
    };
    return bot;
}

inline int32_t execute_ranger_rotation(PlayerBot& bot, MonsterTarget& target) {
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
