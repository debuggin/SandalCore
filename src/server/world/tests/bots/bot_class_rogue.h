// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_rogue_bot(uint32_t replica_idx) {
    PlayerBot bot{};
    bot.bot_id = 3000ULL + replica_idx;
    bot.replica_index = replica_idx;
    bot.name = replica_idx == 1 ? "TST RogueAlpha" : "TST RogueBravo";
    bot.class_id = BotClassId::Rogue;
    bot.class_name = "Rogue";
    bot.level = kMaxBotLevel;
    bot.max_hp = 21500;
    bot.hp = bot.max_hp;
    bot.max_mana = 7800;
    bot.mana = bot.max_mana;
    bot.base_phys_power = 1680;
    bot.base_mag_power = 380;
    bot.base_armor = 1320;
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.loadout_presets = {
        {"MainHand", "Nightshade Stiletto", 606463110000000301LL, 490, 0, 20, 0},
        {"OffHand", "Venomfang Kris", 606463110000000302LL, 420, 0, 20, 0},
        {"ChestArmor", "Shadowstalker Leather Jerkin", 606463110000000303LL, 110, 0, 340, 0},
        {"GatheringTool", "Obsidian Skinning Knife", 606463110000000304LL, 30, 0, 0, 40},
    };
    bot.skill_tree_presets = {
        {"Rogue_Lethality_Rank5", 606462910000000301LL, 606462980000000300LL, true, 220},
        {"Rogue_Shadowstep_Unlock", 606462910000000302LL, 606462980000000300LL, false, 140},
        {"Rogue_Backstab_Unlock", 606462910000000303LL, 606462980000000300LL, false, 190},
        {"Rogue_PoisonedBlades_Passive", 606462910000000304LL, 606462980000000300LL, true, 160},
    };
    bot.abilities = {
        {"ShadowstepAmbush", 606463180000000301LL, 2200.0, 2800, 0, 150, 6000, false, true},
        {"BackstabExecute", 606463180000000302LL, 300.0, 4600, 0, 240, 4000, false, false},
        {"VenomousLacerate", 606463180000000303LL, 300.0, 3100, 0, 180, 2500, false, false},
        {"ThrowingDaggerFan", 606463180000000304LL, 1600.0, 2400, 0, 160, 3000, true, false},
    };
    return bot;
}

inline int32_t execute_rogue_rotation(PlayerBot& bot, MonsterTarget& target) {
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
