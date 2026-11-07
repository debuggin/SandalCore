// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_combat.h"

namespace aoc::world::tests::plans {

struct WeaponCategorySpec {
    const char* category_name = "";
    int64_t weapon_guid = 0;
    double attack_range_cm = 300.0;
    int32_t phys_power_bonus = 0;
    int32_t mag_power_bonus = 0;
    int32_t base_swing_damage = 1200;
};

inline constexpr std::array<WeaponCategorySpec, 16> kAll16WeaponCategories = {{
    {"Greatsword", 606463600000000001LL, 350.0, 520, 0, 1850},
    {"Greataxe", 606463600000000002LL, 340.0, 540, 0, 1920},
    {"Warhammer", 606463600000000003LL, 330.0, 530, 0, 1880},
    {"Polearm/Halberd", 606463600000000004LL, 420.0, 500, 0, 1790},
    {"1H Sword + Shield", 606463600000000005LL, 300.0, 380, 0, 1450},
    {"1H Axe", 606463600000000006LL, 300.0, 410, 0, 1520},
    {"1H Mace", 606463600000000007LL, 300.0, 400, 120, 1490},
    {"Rapier", 606463600000000008LL, 320.0, 430, 0, 1580},
    {"Dual Daggers", 606463600000000009LL, 280.0, 460, 0, 1690},
    {"Longbow", 606463600000000010LL, 3500.0, 490, 0, 1780},
    {"Shortbow", 606463600000000011LL, 2800.0, 450, 0, 1660},
    {"2H Staff", 606463600000000012LL, 3000.0, 120, 560, 1890},
    {"Wand + Focus", 606463600000000013LL, 2600.0, 90, 490, 1680},
    {"Scepter + Grimoire", 606463600000000014LL, 2600.0, 110, 510, 1720},
    {"Spellbook", 606463600000000015LL, 2800.0, 80, 530, 1760},
    {"Lute", 606463600000000016LL, 2500.0, 140, 480, 1640},
}};

inline bool equip_and_attack_with_weapon_category(bots::PlayerBot& bot, const WeaponCategorySpec& weapon) {
    const int32_t phys_before = bot.phys_power;
    const int32_t mag_before = bot.mag_power;
    bot.phys_power += weapon.phys_power_bonus;
    bot.mag_power += weapon.mag_power_bonus;

    bots::MonsterTarget target{
        97000ULL + bot.bot_id, "Weapon Mastery Automaton", "Construct", 50, 80000, 80000,
        {bot.pos_cm.x + std::min(weapon.attack_range_cm * 0.5, 1200.0), bot.pos_cm.y, bot.pos_cm.z},
        bot.worker_id, true, false, bot.bot_id
    };
    if (bots::distance_2d_cm(bot.pos_cm, target.pos_cm) > weapon.attack_range_cm) return false;
    const int32_t swing_damage = weapon.base_swing_damage + (bot.phys_power / 2) + (bot.mag_power / 2);
    target.hp = std::max(0, target.hp - swing_damage);
    ++bot.abilities_cast;

    const bool valid = swing_damage > weapon.base_swing_damage && target.hp < target.max_hp &&
                       (bot.phys_power > phys_before || bot.mag_power > mag_before);
    bot.phys_power = phys_before;
    bot.mag_power = mag_before;
    return valid;
}

inline bool run_plan_weapons(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    size_t seq = 0;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            const auto& weapon = kAll16WeaponCategories[seq++ % kAll16WeaponCategories.size()];
            if (!equip_and_attack_with_weapon_category(actor, weapon)) return false;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

}  // namespace aoc::world::tests::plans
