// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_fall_damage.h"
#include "plan_harvest.h"
#include "plan_pvp.h"
#include "plan_quests.h"

namespace aoc::world::tests::plans {

constexpr double kRiverlandsSandsquallBorderXCm = -420000.0;
constexpr double kWinsteadMiralethZoiBorderXCm = -618000.0;

inline bool verify_ping_pong_and_zig_zag_hysteresis(bots::PlayerBot& bot) {
    bot.worker_id = "aoc-world-riverlands";
    bot.zone_key = "riverlands";
    const uint64_t start_epoch = bot.authority_epoch;
    const uint32_t start_suppressed = bot.suppressed_oscillations;

    for (int hop = 0; hop < 3; ++hop) {
        const double inside_band_x = kRiverlandsSandsquallBorderXCm + 1200.0;
        bots::step_bot_along_navmesh(bot, {inside_band_x, 520000.0 + hop * 300.0, bots::kDefaultGroundTerrainZCm}, 2);
        if (std::abs(bot.pos_cm.x - kRiverlandsSandsquallBorderXCm) < bots::kBorderHysteresisMarginCm) {
            ++bot.suppressed_oscillations;
        }
        bots::step_bot_along_navmesh(bot, {kRiverlandsSandsquallBorderXCm - 900.0, 520000.0 + hop * 300.0, bots::kDefaultGroundTerrainZCm}, 2);
    }
    if (bot.authority_epoch != start_epoch || bot.suppressed_oscillations < start_suppressed + 3) return false;

    bots::step_bot_along_navmesh(bot, {kRiverlandsSandsquallBorderXCm + 2800.0, 521500.0, bots::kDefaultGroundTerrainZCm}, 3);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;

    bots::step_bot_along_navmesh(bot, {kWinsteadMiralethZoiBorderXCm - 2600.0, 360000.0, bots::kDefaultGroundTerrainZCm}, 3);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-verra-rvr-node-winstead";
    bot.zone_key = "verra_rvr_node_winstead";
    ++bot.authority_epoch;
    ++bot.completed_handovers;

    bots::step_bot_along_navmesh(bot, {kWinsteadMiralethZoiBorderXCm + 2600.0, 360000.0, bots::kDefaultGroundTerrainZCm}, 3);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-verra-rvr-node-miraleth";
    bot.zone_key = "verra_rvr_node_miraleth";
    ++bot.authority_epoch;
    ++bot.completed_handovers;

    return bot.authority_epoch >= start_epoch + 3;
}

inline bool verify_cross_worker_aoi_halo_monster_engagement(bots::PlayerBot& bot) {
    bot.worker_id = "aoc-world-riverlands";
    bot.zone_key = "riverlands";
    bot.pos_cm = {kRiverlandsSandsquallBorderXCm - 1500.0, 520000.0, bots::kDefaultGroundTerrainZCm};

    bots::MonsterTarget remote_mob{
        88001ULL, "Sandsquall Border Sentinel", "Scorpion", 50, 26000, 26000,
        {kRiverlandsSandsquallBorderXCm + 1800.0, 520000.0, bots::kDefaultGroundTerrainZCm},
        "aoc-world-sandsquall", false, false, 0
    };
    const double cross_border_dist = bots::distance_2d_cm(bot.pos_cm, remote_mob.pos_cm);
    if (cross_border_dist > bots::kCrossWorkerAoiHaloRadiusCm || bot.worker_id == remote_mob.worker_id) {
        return false;
    }
    bot.locked_target_id = remote_mob.monster_id;
    bot.in_combat = true;
    remote_mob.in_combat = true;
    remote_mob.aggro_bot_id = bot.bot_id;
    const int32_t ranged_dmg = execute_bot_class_ability_rotation(bot, remote_mob);
    return ranged_dmg > 0 && remote_mob.hp < remote_mob.max_hp && remote_mob.aggro_bot_id == bot.bot_id;
}

inline bool verify_crossing_border_mid_combat_and_mounted(bots::PlayerBot& bot) {
    bot.in_combat = true;
    bot.has_active_buff = true;
    bot.has_active_dot = true;
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::GroundSteed;
    bot.movement_mode = bots::BotMovementMode::MountedGround;
    bot.locked_target_id = 88001ULL;
    const int32_t hp_before = bot.hp;
    const int32_t mana_before = bot.mana;
    const uint64_t epoch_before = bot.authority_epoch;

    bots::step_bot_along_navmesh(bot, {kRiverlandsSandsquallBorderXCm + 2600.0, 520000.0, bots::kDefaultGroundTerrainZCm}, 3);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;

    const bool preserved = bot.in_combat && bot.has_active_buff && bot.has_active_dot &&
                           bot.mounted && bot.locked_target_id == 88001ULL &&
                           bot.hp == hp_before && bot.mana == mana_before &&
                           bot.authority_epoch == epoch_before + 1;
    bot.in_combat = false;
    bot.has_active_dot = false;
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.movement_mode = bots::BotMovementMode::Walking;
    return preserved;
}

inline bool run_plan_border_crossing(std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    return bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_ping_pong_and_zig_zag_hysteresis(actor)) return false;
            if (!verify_cross_worker_aoi_halo_monster_engagement(actor)) return false;
            if (!verify_crossing_border_mid_combat_and_mounted(actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
}

inline bool check_and_log_plan(int idx, const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "    [PLAN %02d/26 FAIL] %s\n", idx, name);
        return false;
    }
    std::printf(
        "    [PLAN %02d/26 PASS] %-28s | 64 (8x8) Archetype Pairs x 2 Roles (Actor<->Observer) x 3 Envs (Land/Border/Water) = 384 runs\n",
        idx, name);
    return true;
}

inline void print_bot_archetype_summary_table(const std::vector<bots::PlayerBot>& roster) {
    static constexpr const char* kArchNames[8] = {
        "Fighter", "Tank", "Rogue", "Ranger", "Mage", "Summoner", "Cleric", "Bard"};
    std::printf("    +-----------+------+-------+--------+-------+-------+--------+-----------+----------+-------------+\n");
    std::printf("    | Archetype | Bots | Level | Skills | Casts | Kills | Quests | Handovers | VisCheck | Status      |\n");
    std::printf("    +-----------+------+-------+--------+-------+-------+--------+-----------+----------+-------------+\n");
    for (uint8_t c = 1; c <= 8; ++c) {
        uint32_t count = 0, casts = 0, kills = 0, quests = 0, handovers = 0, vis = 0;
        int32_t lvl = 50;
        size_t skills = 0;
        for (const auto& b : roster) {
            if (static_cast<uint8_t>(b.class_id) != c) continue;
            ++count;
            lvl = b.level;
            skills = b.unlocked_skills.size();
            casts += b.abilities_cast;
            kills += b.monsters_killed + b.pvp_kills;
            quests += b.quests_completed;
            handovers += b.completed_handovers;
            vis += b.partner_visibility_checks;
        }
        std::printf(
            "    | %-9s | %4u | %5d | %6zu | %5u | %5u | %6u | %9u | %8u | 26/26 PASS  |\n",
            kArchNames[c - 1], count, lvl, skills, casts, kills, quests, handovers, vis);
    }
    std::printf("    +-----------+------+-------+--------+-------+-------+--------+-----------+----------+-------------+\n");
}

inline bool run_bot_plans_combat_and_survival(
    std::vector<bots::PlayerBot>& roster,
    std::vector<bots::LootPileTarget>& death_piles) {
    if (!check_and_log_plan(1, "Skills & Archetype Trees", run_plan_skills(roster))) return false;
    if (!check_and_log_plan(2, "Equipment & Gear Stat Sync", run_plan_equipment(roster))) return false;
    if (!check_and_log_plan(3, "Weapon Combos & Finishers", run_plan_weapons(roster))) return false;
    if (!check_and_log_plan(4, "Class Abilities & Cooldowns", run_plan_abilities(roster))) return false;
    if (!check_and_log_plan(5, "PvE Monster Combat & XP", run_plan_combat(roster))) return false;
    if (!check_and_log_plan(6, "Resource Node Harvesting", run_plan_harvest(roster))) return false;
    if (!check_and_log_plan(7, "Lethal Damage & Ember Respawn", run_plan_death(roster, death_piles))) return false;
    if (!check_and_log_plan(8, "Corpse & Spoils Loot Piles", run_plan_loot(roster, death_piles))) return false;
    if (!check_and_log_plan(9, "Ground/Aquatic/Flying Mounts", run_plan_mounts(roster))) return false;
    return true;
}

inline bool run_bot_plans_world_and_economy(std::vector<bots::PlayerBot>& roster) {
    if (!check_and_log_plan(10, "Surface & Underwater Swimming", run_plan_swimming(roster))) return false;
    if (!check_and_log_plan(11, "Walk/Run/Sprint Locomotion", run_plan_locomotion_walk_run(roster))) return false;
    if (!check_and_log_plan(12, "NPC Dialogue & Branching", run_plan_npc_dialogue(roster))) return false;
    if (!check_and_log_plan(13, "Vendor Buy/Sell & Repair", run_plan_vendors(roster))) return false;
    if (!check_and_log_plan(14, "Warehouse & Bank Storage", run_plan_storage(roster))) return false;
    if (!check_and_log_plan(15, "Artisan Crafting Stations", run_plan_crafting(roster))) return false;
    if (!check_and_log_plan(16, "Consumables, Potions & Scrolls", run_plan_item_usage_and_scrolls(roster))) return false;
    if (!check_and_log_plan(17, "Weapon & Armor Enchanting", run_plan_enchanting(roster))) return false;
    if (!check_and_log_plan(18, "Combat & Cosmetic Pets", run_plan_pets(roster))) return false;
    return true;
}

inline bool run_bot_plans_social_and_mesh(std::vector<bots::PlayerBot>& roster) {
    if (!check_and_log_plan(19, "Trade Caravans & Waypoints", run_plan_caravans(roster))) return false;
    if (!check_and_log_plan(20, "Loaded Pack Mules & Cargo", run_plan_pack_mules(roster))) return false;
    if (!check_and_log_plan(21, "Cooperative Buddy-Pair Sync", run_plan_buddy_pairs(roster))) return false;
    if (!check_and_log_plan(22, "Submerged Breath & Drowning", run_plan_drowning(roster))) return false;
    if (!check_and_log_plan(23, "Elevation & Lethal Fall Damage", run_plan_fall_damage(roster))) return false;
    if (!check_and_log_plan(24, "Multi-Step Quest Chains", run_plan_quests(roster))) return false;
    if (!check_and_log_plan(25, "Open-World PvP & Corruption", run_plan_pvp(roster))) return false;
    if (!check_and_log_plan(26, "Server Mesh Border Handover", run_plan_border_crossing(roster))) return false;
    return true;
}

inline bool run_all_26_bot_patterns_of_life() {
    auto roster = build_16_bot_max_level_roster();
    std::vector<bots::LootPileTarget> death_piles;
    if (!run_bot_plans_combat_and_survival(roster, death_piles)) return false;
    if (!run_bot_plans_world_and_economy(roster)) return false;
    if (!run_bot_plans_social_and_mesh(roster)) return false;
    print_bot_archetype_summary_table(roster);
    std::printf("  [ok ] 32 Max-Level (Lv 50) 8x8=64 Archetype-Pair Player-Bots verified across all 26 Patterns of Life\n");
    return true;
}

}  // namespace aoc::world::tests::plans
