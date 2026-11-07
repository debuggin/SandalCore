// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "plan_death.h"

namespace aoc::world::tests::plans {

struct MonsterCorpseLootTable {
    uint64_t monster_id = 0;
    const char* monster_name = "";
    bots::Vec3Cm corpse_pos_cm{};
    std::string worker_id = "aoc-world-riverlands";
    int64_t trophy_item_guid = 0;
    const char* trophy_name = "";
    int32_t trophy_qty = 1;
    int64_t material_item_guid = 0;
    const char* material_name = "";
    int32_t material_qty = 2;
    int32_t glint_drop = 3;
    int32_t copper_drop = 420;
    bool corpse_spawned = false;
    bool corpse_looted = false;
};

inline bool kill_monster_and_loot_corpse(bots::PlayerBot& bot, MonsterCorpseLootTable& corpse, bool cross_boundary) {
    if (bot.dead || corpse.corpse_looted) return false;
    bots::MonsterTarget mob{
        corpse.monster_id, corpse.monster_name, "Monster", 50, 22000, 22000,
        corpse.corpse_pos_cm, corpse.worker_id, true, false, bot.bot_id
    };
    const std::string initial_worker = bot.worker_id;
    bots::step_bot_along_navmesh(bot, mob.pos_cm, 4);
    if (cross_boundary && corpse.worker_id != initial_worker) {
        bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
        bot.worker_id = corpse.worker_id;
        ++bot.authority_epoch;
        ++bot.completed_handovers;
    }
    mob.hp = 0;
    mob.dead = true;
    ++bot.monsters_killed;
    corpse.corpse_spawned = mob.dead && (mob.hp == 0);
    if (!corpse.corpse_spawned || bots::distance_2d_cm(bot.pos_cm, corpse.corpse_pos_cm) > 250.0) return false;

    const int64_t copper_before = bot.copper_balance;
    bots::add_inventory_item(bot, corpse.trophy_item_guid, corpse.trophy_name, corpse.trophy_qty, bots::ResourceKind::Huntable);
    bots::add_inventory_item(bot, corpse.material_item_guid, corpse.material_name, corpse.material_qty, bots::ResourceKind::Ore);
    bot.copper_balance += corpse.copper_drop + (corpse.glint_drop * 100);
    corpse.trophy_qty = 0;
    corpse.material_qty = 0;
    corpse.glint_drop = 0;
    corpse.copper_drop = 0;
    corpse.corpse_looted = true;
    return bot.copper_balance > copper_before && corpse.corpse_looted;
}

inline bool leave_loot_pile_and_observer_loots(
    bots::PlayerBot& actor,
    bots::PlayerBot& observer,
    bots::BotTestEnv env) {
    constexpr int64_t kSharedLootItemGuid = 606463390000000777LL;
    bots::LootPileTarget shared_pile{
        78000ULL + actor.bot_id,
        "Abandoned Monster Loot Pile",
        actor.pos_cm,
        actor.worker_id,
        kSharedLootItemGuid,
        3,
        300,
        actor.bot_id,
        false,
        false,
    };
    bots::add_inventory_item(actor, shared_pile.loot_item_guid, "Partial Trophy", 1, bots::ResourceKind::Huntable);
    shared_pile.item_quantity -= 1;

    bots::step_bot_along_navmesh(actor, {actor.pos_cm.x + 1200.0, actor.pos_cm.y, actor.pos_cm.z}, 3);
    shared_pile.abandoned_for_public_loot = true;
    if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;

    bots::step_bot_along_navmesh(observer, shared_pile.pos_cm, 3);
    if (env == bots::BotTestEnv::AcrossBoundary && observer.worker_id != shared_pile.worker_id) {
        observer.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
        observer.worker_id = shared_pile.worker_id;
        ++observer.authority_epoch;
        ++observer.completed_handovers;
    }
    if (!shared_pile.abandoned_for_public_loot || shared_pile.item_quantity != 2) return false;

    const int64_t observer_copper_before = observer.copper_balance;
    bots::add_inventory_item(observer, shared_pile.loot_item_guid, "Abandoned Monster Loot", shared_pile.item_quantity, bots::ResourceKind::Ore);
    observer.copper_balance += shared_pile.ash_currency;
    shared_pile.item_quantity = 0;
    shared_pile.looted = true;
    return shared_pile.looted && observer.copper_balance == observer_copper_before + 300 &&
           bots::assert_partner_visible_and_updating(actor, observer);
}

inline bool loot_container_pile(bots::PlayerBot& bot, bots::LootPileTarget& pile, bool cross_boundary) {
    if (bot.dead || pile.looted || pile.item_quantity <= 0) return false;
    const std::string initial_worker = bot.worker_id;
    bots::step_bot_along_navmesh(bot, pile.pos_cm, 4);
    if (cross_boundary && pile.worker_id != initial_worker) {
        bot.dwell_ticks_across_border = bots::kBorderMinDwellTicks;
        bot.worker_id = pile.worker_id;
        ++bot.authority_epoch;
        ++bot.completed_handovers;
    }
    if (bots::distance_2d_cm(bot.pos_cm, pile.pos_cm) > 250.0) return false;
    bots::add_inventory_item(bot, pile.loot_item_guid, pile.source_name.c_str(), pile.item_quantity, bots::ResourceKind::Ore);
    bot.copper_balance += pile.ash_currency;
    pile.item_quantity = 0;
    pile.looted = true;
    return true;
}

inline bool run_plan_loot(std::vector<bots::PlayerBot>& roster, std::vector<bots::LootPileTarget>& death_piles) {
    if (roster.size() < 16 || death_piles.size() < roster.size()) return false;
    for (size_t i = 0; i < roster.size(); ++i) {
        auto& bot = roster[i];
        const bool cross_mob = (i % 2 == 1);
        MonsterCorpseLootTable corpse{
            77000ULL + i,
            cross_mob ? "Sandsquall Dune Ravager" : "Riverlands Goblin Warlock",
            cross_mob ? bots::Vec3Cm{-416500.0, 681500.0, 9800.0} : bots::Vec3Cm{-644100.0, 376900.0, 11400.0},
            cross_mob ? "aoc-world-sandsquall" : "aoc-world-riverlands",
            606463390000000100LL + static_cast<int64_t>(i),
            "Monster Trophy Fang",
            1,
            606463390000000200LL + static_cast<int64_t>(i),
            "Salvaged Monster Hide",
            2,
            3,
            420,
            false,
            false
        };
        bot.worker_id = "aoc-world-riverlands";
        if (!kill_monster_and_loot_corpse(bot, corpse, cross_mob)) return false;
        auto& pile = death_piles[i];
        if (!loot_container_pile(bot, pile, pile.worker_id != bot.worker_id)) return false;
    }
    return bots::run_all_64_archetype_combinations_all_envs(
        roster,
        [](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv env) {
            return leave_loot_pile_and_observer_loots(actor, observer, env);
        });
}

}  // namespace aoc::world::tests::plans
