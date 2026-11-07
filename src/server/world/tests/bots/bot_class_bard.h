// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_bard_bot(uint32_t replica_idx) {
    PlayerBot bot{};
    bot.bot_id = 8000ULL + replica_idx;
    bot.replica_index = replica_idx;
    bot.name = replica_idx == 1 ? "TST BardAlpha" : "TST BardBravo";
    bot.class_id = BotClassId::Bard;
    bot.class_name = "Bard";
    bot.level = kMaxBotLevel;
    bot.max_hp = 21800;
    bot.hp = bot.max_hp;
    bot.max_mana = 12600;
    bot.mana = bot.max_mana;
    bot.base_phys_power = 820;
    bot.base_mag_power = 1540;
    bot.base_armor = 1360;
    bot.phys_power = bot.base_phys_power;
    bot.mag_power = bot.base_mag_power;
    bot.armor = bot.base_armor;
    bot.loadout_presets = {
        {"MainHand", "Miraleth Resonant Flute", 6064630871707615237LL, 180, 460, 30, 0},
        {"OffHand", "Virtuoso Rapier", 606463110000000802LL, 260, 240, 40, 0},
        {"ChestArmor", "Troubadour Silk Doublet", 606463110000000803LL, 40, 130, 320, 0},
        {"GatheringTool", "Artisan Lumberjacking Saw", 606463110000000804LL, 20, 20, 0, 40},
    };
    bot.skill_tree_presets = {
        {"Bard_Resonance_Rank5", 6064629885900499588LL, 6064628725236039697LL, true, 210},
        {"Bard_MelodyOfAlacrity_Unlock", 606462910000000802LL, 6064628725236039697LL, false, 160},
        {"Bard_DiscordantChord_Unlock", 606462910000000803LL, 6064628725236039697LL, false, 180},
        {"Bard_AriaOfCourage_Passive", 606462910000000804LL, 6064628725236039697LL, true, 175},
    };
    bot.abilities = {
        {"DiscordantChordBlast", 606463180000000801LL, 2800.0, 2700, 0, 190, 1400, true, false},
        {"MelodyOfAlacritySong", 606463180000000802LL, 2000.0, 1500, 2200, 240, 3000, false, true},
        {"AriaOfCourageAnthem", 606463180000000803LL, 2000.0, 2400, 1800, 260, 4500, false, true},
        {"CrescendoFinale", 606463180000000804LL, 2600.0, 3900, 0, 300, 6000, true, false},
    };
    return bot;
}

inline int32_t execute_bard_rotation(PlayerBot& bot, MonsterTarget& target) {
    if (bot.dead || bot.level != kMaxBotLevel) return 0;
    int32_t total_dmg = 0;
    for (const auto& ab : bot.abilities) {
        if (bot.mana < ab.mana_cost) break;
        bot.mana -= ab.mana_cost;
        if (ab.is_buff_or_summon) {
            bot.has_active_buff = true;
            bot.move_speed_multiplier = std::max(bot.move_speed_multiplier, 1.25);
        }
        if (ab.base_heal > 0) bot.hp = std::min(bot.max_hp, bot.hp + ab.base_heal);
        const int32_t hit = ab.base_damage + (bot.mag_power / 2) + (bot.phys_power / 4);
        target.hp = std::max(0, target.hp - hit);
        total_dmg += hit;
        ++bot.abilities_cast;
    }
    return total_dmg;
}

}  // namespace aoc::world::tests::bots
