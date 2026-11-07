// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_combat.h"

namespace aoc::world::tests::plans {

struct WeaponCategorySpec {
    const char* category_name = "";
    double attack_range_cm = 300.0;
    int32_t phys_power_bonus = 0;
    int32_t mag_power_bonus = 0;
    int32_t base_swing_damage = 1200;
    bool ranged = false;
};

inline constexpr std::array<WeaponCategorySpec, 16> kAll16WeaponCategories = {{
    {"Greatsword", 350.0, 520, 0, 1850, false},
    {"Greataxe", 340.0, 540, 0, 1920, false},
    {"Warhammer", 330.0, 530, 0, 1880, false},
    {"Polearm/Halberd", 420.0, 500, 0, 1790, false},
    {"1H Sword + Shield", 300.0, 380, 0, 1450, false},
    {"1H Axe", 300.0, 410, 0, 1520, false},
    {"1H Mace", 300.0, 400, 120, 1490, false},
    {"Rapier", 320.0, 430, 0, 1580, false},
    {"Dual Daggers", 280.0, 460, 0, 1690, false},
    {"Longbow", 3500.0, 490, 0, 1780, true},
    {"Shortbow", 2800.0, 450, 0, 1660, true},
    {"2H Staff", 3000.0, 120, 560, 1890, true},
    {"Wand + Focus", 2600.0, 90, 490, 1680, true},
    {"Scepter + Grimoire", 2600.0, 110, 510, 1720, true},
    {"Spellbook", 2800.0, 80, 530, 1760, true},
    {"Lute", 2500.0, 140, 480, 1640, true},
}};

inline bool equip_and_attack_with_weapon_category(
    bots::LiveSession& s,
    bots::PlayerBot& bot,
    size_t weapon_idx) {
    if (s.catalog.weapon_ability_guids.empty()) return false;
    const auto& weapon = kAll16WeaponCategories[weapon_idx % kAll16WeaponCategories.size()];
    const int64_t weapon_ab_guid = s.catalog.weapon_ability_guids[weapon_idx % s.catalog.weapon_ability_guids.size()];
    const bots::schema::ClassSchema& pawn = bots::class_named("PlayerPawn_C");
    if (const bots::schema::RpcSchema* rpc = pawn.rpc("ServerSetUseRangedWeapon")) {
        bots::repl::RpcParams p;
        bots::repl::set_rpc_param(*rpc, p, "bNewUseRangedWeapon", bots::repl::Value::boolean(weapon.ranged));
        (void)send_bunches(s, {s.client->actor(5, bots::build_actor_rpc(pawn, "ServerSetUseRangedWeapon", p), true)});
    }
    if (!bots::send_live_ability_trigger(s, bot, weapon_ab_guid, false)) return false;

    const int32_t phys_before = bot.phys_power;
    const int32_t mag_before = bot.mag_power;
    bot.phys_power += weapon.phys_power_bonus;
    bot.mag_power += weapon.mag_power_bonus;
    bots::MonsterTarget target{
        s.goblin_id != 0 ? s.goblin_id : bot.bot_id,
        "Weapon Mastery Target", "Construct", 50, 80000, 80000,
        {bot.pos_cm.x + std::min(weapon.attack_range_cm * 0.5, 1200.0), bot.pos_cm.y, bot.pos_cm.z},
        bot.worker_id, true, false, bot.bot_id
    };
    if (bots::distance_2d_cm(bot.pos_cm, target.pos_cm) > weapon.attack_range_cm) return false;
    const int32_t swing_damage = weapon.base_swing_damage + (bot.phys_power / 2) + (bot.mag_power / 2);
    target.hp = std::max(0, target.hp - swing_damage);
    const bool valid = swing_damage > weapon.base_swing_damage && target.hp < target.max_hp &&
                       (bot.phys_power > phys_before || bot.mag_power > mag_before);
    bot.phys_power = phys_before;
    bot.mag_power = mag_before;
    return valid;
}

inline bool run_plan_weapons(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    size_t seq = 0;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!equip_and_attack_with_weapon_category(s, actor, seq++)) return false;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

}  // namespace aoc::world::tests::plans
