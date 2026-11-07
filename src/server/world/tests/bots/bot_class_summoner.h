// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_summoner_bot(uint32_t replica_idx) {
    PlayerBot bot{};
    bot.bot_id = 6000ULL + replica_idx;
    bot.replica_index = replica_idx;
    bot.name = replica_idx == 1 ? "TST SummonerAlpha" : "TST SummonerBravo";
    bot.class_id = BotClassId::Summoner;
    bot.class_name = "Summoner";
    bot.level = kMaxBotLevel;
    bot.max_hp = 20800;
    bot.hp = bot.max_hp;
    bot.max_mana = 12200;
    bot.mana = bot.max_mana;
    bot.base_phys_power = 420;
    bot.base_mag_power = 1660;
    bot.base_armor = 1120;
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.loadout_presets = {
        {"MainHand", "Codex of the Astral Pact", 606463110000000601LL, 30, 520, 30, 0},
        {"OffHand", "Soulbinder Scepter", 606463110000000602LL, 20, 360, 40, 0},
        {"ChestArmor", "Conjurer Vestments", 606463110000000603LL, 0, 150, 290, 0},
        {"GatheringTool", "Botanist Herbal Sickle", 606463110000000604LL, 0, 35, 0, 45},
    };
    bot.skill_tree_presets = {
        {"Summoner_SpiritBond_Rank5", 606462910000000601LL, 606462980000000600LL, true, 210},
        {"Summoner_SummonGuardian_Unlock", 606462910000000602LL, 606462980000000600LL, false, 190},
        {"Summoner_HexBolt_Unlock", 606462910000000603LL, 606462980000000600LL, false, 170},
        {"Summoner_EssenceLink_Passive", 606462910000000604LL, 606462980000000600LL, true, 160},
    };
    bot.abilities = {
        {"SummonGuardianSpirit", 606463180000000601LL, 1200.0, 2200, 1200, 320, 8000, false, true},
        {"HexBoltProjectile", 606463180000000602LL, 3200.0, 2900, 0, 210, 1400, true, false},
        {"SpiritClawCommand", 606463180000000603LL, 2800.0, 3600, 0, 250, 3000, false, false},
        {"SoulHarvestDrain", 606463180000000604LL, 2600.0, 3100, 1400, 280, 5000, true, false},
    };
    return bot;
}

inline int32_t execute_summoner_rotation(PlayerBot& bot, MonsterTarget& target) {
    if (bot.dead || bot.level != kMaxBotLevel) return 0;
    bot.has_pet_summoned = true;
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
