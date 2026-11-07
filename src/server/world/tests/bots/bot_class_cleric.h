// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bot_types.h"

namespace aoc::world::tests::bots {

inline PlayerBot make_level50_cleric_bot(const LiveSession& s, uint32_t replica_idx) {
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
    bot.pos_cm = {s.spawn[0], s.spawn[1], ground_z_cm(s)};
    bot.loadout_presets = s.catalog.gear_items;
    bot.skill_tree_presets = s.catalog.skill_nodes_by_class[6];
    bot.abilities = s.catalog.abilities_by_class[6];
    return bot;
}

inline int32_t execute_cleric_rotation(LiveSession& s, PlayerBot& bot, MonsterTarget& target) {
    if (bot.dead || bot.level != kMaxBotLevel) return 0;
    int32_t total_dmg = 0;
    for (const auto& ab : bot.abilities) {
        if (bot.mana < ab.mana_cost) break;
        if (!send_live_ability_trigger(s, bot, ab.ability_guid, bot.pvp_flagged)) return 0;
        bot.mana -= ab.mana_cost;
        if (ab.is_buff_or_summon) bot.has_active_buff = true;
        if (ab.base_heal > 0) bot.hp = std::min(bot.max_hp, bot.hp + ab.base_heal);
        const int32_t hit = ab.base_damage + (bot.mag_power / 2);
        target.hp = std::max(0, target.hp - hit);
        total_dmg += hit;
    }
    return total_dmg;
}

}  // namespace aoc::world::tests::bots
