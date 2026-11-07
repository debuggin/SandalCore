// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_tank_bot(uint32_t replica_idx) {
    PlayerBot bot{};
    bot.bot_id = 2000ULL + replica_idx;
    bot.replica_index = replica_idx;
    bot.name = replica_idx == 1 ? "TST TankAlpha" : "TST TankBravo";
    bot.class_id = BotClassId::Tank;
    bot.class_name = "Tank";
    bot.level = kMaxBotLevel;
    bot.max_hp = 34000;
    bot.hp = bot.max_hp;
    bot.max_mana = 6800;
    bot.mana = bot.max_mana;
    bot.base_phys_power = 1150;
    bot.base_mag_power = 360;
    bot.base_armor = 2900;
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.loadout_presets = {
        {"MainHand", "Aegis Bastion Broadsword", 606463110000000201LL, 340, 0, 120, 0},
        {"OffHand", "Bulwark Tower Shield", 606463110000000202LL, 80, 0, 680, 0},
        {"ChestArmor", "Fortress Heavy Plate", 606463110000000203LL, 60, 0, 740, 0},
        {"GatheringTool", "Reinforced Mining Pick", 606463110000000204LL, 25, 0, 0, 35},
    };
    bot.skill_tree_presets = {
        {"Tank_Ironskin_Rank5", 606462910000000201LL, 606462980000000200LL, true, 240},
        {"Tank_ShieldBash_Unlock", 606462910000000202LL, 606462980000000200LL, false, 110},
        {"Tank_InciteTaunt_Unlock", 606462910000000203LL, 606462980000000200LL, false, 100},
        {"Tank_AegisWall_Unlock", 606462910000000204LL, 606462980000000200LL, true, 200},
    };
    bot.abilities = {
        {"ShieldBash", 606463180000000201LL, 300.0, 2100, 0, 110, 1200, false, false},
        {"InciteRoarTaunt", 606463180000000202LL, 1500.0, 1400, 0, 160, 5000, false, true},
        {"GroundSlamTremor", 606463180000000203LL, 500.0, 2900, 0, 240, 6000, false, false},
        {"AegisFortification", 606463180000000204LL, 300.0, 1500, 1800, 220, 10000, false, true},
    };
    return bot;
}

inline int32_t execute_tank_rotation(PlayerBot& bot, MonsterTarget& target) {
    if (bot.dead || bot.level != kMaxBotLevel) return 0;
    target.aggro_bot_id = bot.bot_id;
    int32_t total_dmg = 0;
    for (const auto& ab : bot.abilities) {
        if (bot.mana < ab.mana_cost) break;
        bot.mana -= ab.mana_cost;
        if (ab.is_buff_or_summon) bot.has_active_buff = true;
        if (ab.base_heal > 0) bot.hp = std::min(bot.max_hp, bot.hp + ab.base_heal);
        const int32_t hit = ab.base_damage + (bot.phys_power / 2) + (bot.armor / 6);
        target.hp = std::max(0, target.hp - hit);
        total_dmg += hit;
        ++bot.abilities_cast;
    }
    return total_dmg;
}

}  // namespace aoc::world::tests::bots
