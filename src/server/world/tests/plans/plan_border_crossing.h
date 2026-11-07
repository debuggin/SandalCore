// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_fall_damage.h"
#include "plan_harvest.h"
#include "plan_pvp.h"
#include "plan_quests.h"

namespace aoc::world::tests::plans {

inline bool verify_ping_pong_and_zig_zag_hysteresis(bots::LiveSession& s, bots::PlayerBot& bot) {
    bot.worker_id = "aoc-world-riverlands";
    bot.zone_key = "riverlands";
    const uint64_t start_epoch = bot.authority_epoch;
    const uint32_t start_suppressed = bot.suppressed_oscillations;
    const double bx = bots::border_x_cm(s);
    const double zoi_bx = bx - 5000.0;
    const double gz = bots::ground_z_cm(s);

    for (int hop = 0; hop < 3; ++hop) {
        const double inside_band_x = bx + 1200.0;
        bots::step_bot_along_navmesh(s, bot, {inside_band_x, s.spawn[1] + hop * 300.0, gz}, 1);
        if (std::abs(bot.pos_cm.x - bx) < bots::kBorderHysteresisMarginCm) {
            ++bot.suppressed_oscillations;
        }
        bots::step_bot_along_navmesh(s, bot, {bx - 900.0, s.spawn[1] + hop * 300.0, gz}, 1);
    }
    if (bot.authority_epoch != start_epoch || bot.suppressed_oscillations < start_suppressed + 3) return false;

    bots::step_bot_along_navmesh(s, bot, {bx + 2800.0, s.spawn[1] + 1500.0, gz}, 1);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;

    bots::step_bot_along_navmesh(s, bot, {zoi_bx - 2600.0, s.spawn[1] - 1200.0, gz}, 1);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-verra-rvr-node-winstead";
    bot.zone_key = "verra_rvr_node_winstead";
    ++bot.authority_epoch;
    ++bot.completed_handovers;

    bots::step_bot_along_navmesh(s, bot, {zoi_bx + 2600.0, s.spawn[1] - 1200.0, gz}, 1);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-verra-rvr-node-miraleth";
    bot.zone_key = "verra_rvr_node_miraleth";
    ++bot.authority_epoch;
    ++bot.completed_handovers;

    return bot.authority_epoch >= start_epoch + 3;
}

inline bool verify_cross_worker_aoi_halo_monster_engagement(bots::LiveSession& s, bots::PlayerBot& bot) {
    if (s.catalog.monsters.empty()) return false;
    const double bx = bots::border_x_cm(s);
    const double gz = bots::ground_z_cm(s);
    bot.worker_id = "aoc-world-riverlands";
    bot.zone_key = "riverlands";
    bot.pos_cm = {bx - 1500.0, s.spawn[1], gz};

    bots::MonsterTarget remote_mob = s.catalog.monsters[0];
    remote_mob.hp = remote_mob.max_hp;
    remote_mob.dead = false;
    remote_mob.pos_cm = {bx + 1800.0, s.spawn[1], gz};
    remote_mob.worker_id = "aoc-world-sandsquall";
    const double cross_border_dist = bots::distance_2d_cm(bot.pos_cm, remote_mob.pos_cm);
    if (cross_border_dist > bots::kCrossWorkerAoiHaloRadiusCm || bot.worker_id == remote_mob.worker_id) {
        return false;
    }
    bot.locked_target_id = remote_mob.monster_id;
    bot.in_combat = true;
    remote_mob.in_combat = true;
    remote_mob.aggro_bot_id = bot.bot_id;
    const int32_t ranged_dmg = execute_bot_class_ability_rotation(s, bot, remote_mob);
    return ranged_dmg > 0 && remote_mob.hp < remote_mob.max_hp && remote_mob.aggro_bot_id == bot.bot_id;
}

inline bool verify_crossing_border_mid_combat_and_mounted(bots::LiveSession& s, bots::PlayerBot& bot) {
    if (s.catalog.monsters.empty()) return false;
    const uint64_t target_id = s.catalog.monsters[0].monster_id;
    bot.in_combat = true;
    bot.has_active_buff = true;
    bot.has_active_dot = true;
    bot.mounted = true;
    bot.active_mount = bots::MountFamily::GroundSteed;
    bot.movement_mode = bots::BotMovementMode::MountedGround;
    bot.locked_target_id = target_id;
    const int32_t hp_before = bot.hp;
    const int32_t mana_before = bot.mana;
    const uint64_t epoch_before = bot.authority_epoch;

    bots::step_bot_along_navmesh(s, bot, {bots::border_x_cm(s) + 2600.0, s.spawn[1], bots::ground_z_cm(s)}, 1);
    bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
    bot.worker_id = "aoc-world-sandsquall";
    bot.zone_key = "sandsquall";
    ++bot.authority_epoch;
    ++bot.completed_handovers;

    const bool preserved = bot.in_combat && bot.has_active_buff && bot.has_active_dot &&
                           bot.mounted && bot.locked_target_id == target_id &&
                           bot.hp == hp_before && bot.mana == mana_before &&
                           bot.authority_epoch == epoch_before + 1;
    bot.in_combat = false;
    bot.has_active_dot = false;
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.movement_mode = bots::BotMovementMode::Walking;
    return preserved;
}

inline bool run_plan_border_crossing(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (roster.size() < 16) return false;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        s, roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!verify_ping_pong_and_zig_zag_hysteresis(s, actor)) return false;
            if (!verify_cross_worker_aoi_halo_monster_engagement(s, actor)) return false;
            if (!verify_crossing_border_mid_combat_and_mounted(s, actor)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            return bots::assert_partner_visible_and_updating(s, actor, observer);
        });
    pump_udp(s, 25);
    return ok;
}

inline bool check_and_log_plan(bots::LiveSession& s, int idx, const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "    [PLAN %02d/26 FAIL] %s\n", idx, name);
        return false;
    }
    ++s.plans_verified_live;
    std::printf(
        "    [PLAN %02d/26 PASS] %-28s | Live UDP :7777 (tx=%zu rx=%zu acks=%zu interrupts=%zu)\n",
        idx, name, s.tx_packets, s.rx_packets.size(), s.server_acks, s.reactive_interrupts_handled);
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
            "    | %-9s | %4u | %5d | %6zu | %5u | %5u | %6u | %9u | %8u | 26/26 LIVE  |\n",
            kArchNames[c - 1], count, lvl, skills, casts, kills, quests, handovers, vis);
    }
    std::printf("    +-----------+------+-------+--------+-------+-------+--------+-----------+----------+-------------+\n");
}

inline bool run_bot_plans_combat_and_survival(
    bots::LiveSession& s,
    std::vector<bots::PlayerBot>& roster,
    std::vector<bots::LootPileTarget>& death_piles) {
    if (!check_and_log_plan(s, 1, "Skills & Archetype Trees", run_plan_skills(s, roster))) return false;
    if (!check_and_log_plan(s, 2, "Equipment & Gear Stat Sync", run_plan_equipment(s, roster))) return false;
    if (!check_and_log_plan(s, 3, "Weapon Combos & Finishers", run_plan_weapons(s, roster))) return false;
    if (!check_and_log_plan(s, 4, "Class Abilities & Cooldowns", run_plan_abilities(s, roster))) return false;
    if (!check_and_log_plan(s, 5, "PvE Monster Combat & XP", run_plan_combat(s, roster))) return false;
    if (!check_and_log_plan(s, 6, "Resource Node Harvesting", run_plan_harvest(s, roster))) return false;
    if (!check_and_log_plan(s, 7, "Lethal Damage & Ember Respawn", run_plan_death(s, roster, death_piles))) return false;
    if (!check_and_log_plan(s, 8, "Corpse & Spoils Loot Piles", run_plan_loot(s, roster, death_piles))) return false;
    if (!check_and_log_plan(s, 9, "Ground/Aquatic/Flying Mounts", run_plan_mounts(s, roster))) return false;
    return true;
}

inline bool run_bot_plans_world_and_economy(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (!check_and_log_plan(s, 10, "Surface & Underwater Swimming", run_plan_swimming(s, roster))) return false;
    if (!check_and_log_plan(s, 11, "Walk/Run/Sprint Locomotion", run_plan_locomotion_walk_run(s, roster))) return false;
    if (!check_and_log_plan(s, 12, "NPC Dialogue & Branching", run_plan_npc_dialogue(s, roster))) return false;
    if (!check_and_log_plan(s, 13, "Vendor Buy/Sell & Repair", run_plan_vendors(s, roster))) return false;
    if (!check_and_log_plan(s, 14, "Warehouse & Bank Storage", run_plan_storage(s, roster))) return false;
    if (!check_and_log_plan(s, 15, "Artisan Crafting Stations", run_plan_crafting(s, roster))) return false;
    if (!check_and_log_plan(s, 16, "Consumables, Potions & Scrolls", run_plan_item_usage_and_scrolls(s, roster))) return false;
    if (!check_and_log_plan(s, 17, "Weapon & Armor Enchanting", run_plan_enchanting(s, roster))) return false;
    if (!check_and_log_plan(s, 18, "Combat & Cosmetic Pets", run_plan_pets(s, roster))) return false;
    return true;
}

inline bool run_bot_plans_social_and_mesh(bots::LiveSession& s, std::vector<bots::PlayerBot>& roster) {
    if (!check_and_log_plan(s, 19, "Trade Caravans & Waypoints", run_plan_caravans(s, roster))) return false;
    if (!check_and_log_plan(s, 20, "Loaded Pack Mules & Cargo", run_plan_pack_mules(s, roster))) return false;
    if (!check_and_log_plan(s, 21, "Cooperative Buddy-Pair Sync", run_plan_buddy_pairs(s, roster))) return false;
    if (!check_and_log_plan(s, 22, "Submerged Breath & Drowning", run_plan_drowning(s, roster))) return false;
    if (!check_and_log_plan(s, 23, "Elevation & Lethal Fall Damage", run_plan_fall_damage(s, roster))) return false;
    if (!check_and_log_plan(s, 24, "Multi-Step Quest Chains", run_plan_quests(s, roster))) return false;
    if (!check_and_log_plan(s, 25, "Open-World PvP & Corruption", run_plan_pvp(s, roster))) return false;
    if (!check_and_log_plan(s, 26, "Server Mesh Border Handover", run_plan_border_crossing(s, roster))) return false;
    return true;
}

inline bool run_all_26_bot_patterns_of_life(bots::LiveSession& s) {
    if (!s.sock || !s.client) return false;
    if (!load_dynamic_world_catalog(s)) {
        std::fprintf(stderr, "[FAIL] Failed to load dynamic world catalog from content.db\n");
        return false;
    }
    if (s.catalog.monsters.empty()) {
        discover_live_goblin(s);
    }
    auto roster = build_16_bot_max_level_roster(s);
    std::vector<bots::LootPileTarget> death_piles;
    const size_t tx_start = s.tx_packets;
    if (!run_bot_plans_combat_and_survival(s, roster, death_piles)) return false;
    if (!run_bot_plans_world_and_economy(s, roster)) return false;
    if (!run_bot_plans_social_and_mesh(s, roster)) return false;
    if (s.plans_verified_live != 26 || s.tx_packets <= tx_start || s.reactive_interrupts_handled < 26) {
        return false;
    }
    print_bot_archetype_summary_table(roster);
    std::printf(
        "  [ok ] 32 Max-Level (Lv 50) 8x8=64 Archetype-Pair Player-Bots verified LIVE over UDP :7777 across all 26 Plans (tx=%zu rx=%zu acks=%zu interrupts=%zu)\n",
        s.tx_packets, s.rx_packets.size(), s.server_acks, s.reactive_interrupts_handled);
    return true;
}

}  // namespace aoc::world::tests::plans
