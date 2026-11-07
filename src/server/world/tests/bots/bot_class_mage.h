// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_mage_bot(uint32_t replica_idx) {
    PlayerBot bot{};
    bot.bot_id = 5000ULL + replica_idx;
    bot.replica_index = replica_idx;
    bot.name = replica_idx == 1 ? "TST MageAlpha" : "TST MageBravo";
    bot.class_id = BotClassId::Mage;
    bot.class_name = "Mage";
    bot.level = kMaxBotLevel;
    bot.max_hp = 19500;
    bot.hp = bot.max_hp;
    bot.max_mana = 12800;
    bot.mana = bot.max_mana;
    bot.base_phys_power = 380;
    bot.base_mag_power = 1780;
    bot.base_armor = 1040;
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.loadout_presets = {
        {"MainHand", "Archmage Emberstaff", 606463110000000501LL, 40, 560, 20, 0},
        {"OffHand", "Starlight Focus Orb", 606463110000000502LL, 0, 340, 30, 0},
        {"ChestArmor", "Suncrest Silk Robes", 606463110000000503LL, 0, 160, 260, 0},
        {"GatheringTool", "Runed Stonecutting Chisel", 606463110000000504LL, 0, 35, 0, 40},
    };
    bot.skill_tree_presets = {
        {"Mage_ArcaneConduit_Rank5", 606462910000000501LL, 606462980000000500LL, true, 240},
        {"Mage_Pyroblast_Unlock", 606462910000000502LL, 606462980000000500LL, false, 200},
        {"Mage_ChainLightning_Unlock", 606462910000000503LL, 606462980000000500LL, false, 180},
        {"Mage_PrismaticBarrier_Passive", 606462910000000504LL, 606462980000000500LL, true, 150},
    };
    bot.abilities = {
        {"FireballProjectile", 606463180000000501LL, 3400.0, 3200, 0, 260, 1500, true, false},
        {"FrostboltLance", 606463180000000502LL, 3400.0, 2800, 0, 220, 1500, true, false},
        {"ChainLightningArc", 606463180000000503LL, 3000.0, 4400, 0, 380, 4500, true, false},
        {"PrismaticWardShield", 606463180000000504LL, 100.0, 1600, 1500, 300, 9000, false, true},
    };
    return bot;
}

inline int32_t execute_mage_rotation(PlayerBot& bot, MonsterTarget& target) {
    if (bot.dead || bot.level != kMaxBotLevel) return 0;
    int32_t total_dmg = 0;
    for (const auto& ab : bot.abilities) {
        if (bot.mana < ab.mana_cost) break;
        bot.mana -= ab.mana_cost;
        if (ab.is_buff_or_summon) bot.has_active_buff = true;
        const int32_t hit = ab.base_damage + (bot.mag_power / 2);
        target.hp = std::max(0, target.hp - hit);
        total_dmg += hit;
        ++bot.abilities_cast;
    }
    return total_dmg;
}

}  // namespace aoc::world::tests::bots
