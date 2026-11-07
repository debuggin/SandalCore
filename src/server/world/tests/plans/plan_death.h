// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "../bots/bot_types.h"

namespace aoc::world::tests::plans {

inline bool unlock_all_ember_springs(bots::PlayerBot& bot) {
    bot.unlocked_ember_springs.clear();
    for (const auto& sp : bots::kEmberSprings) {
        bot.unlocked_ember_springs.push_back(sp.spring_guid);
    }
    return bot.unlocked_ember_springs.size() == bots::kEmberSprings.size();
}

inline bool unlock_all_six_ember_springs(bots::PlayerBot& bot) {
    return unlock_all_ember_springs(bot);
}

inline bots::LootPileTarget apply_lethal_damage_and_spawn_death_pile(bots::PlayerBot& bot, uint64_t container_id) {
    bot.hp = 0;
    bot.dead = true;
    bot.in_combat = false;
    bot.mounted = false;
    bot.active_mount = bots::MountFamily::None;
    bot.movement_mode = bots::BotMovementMode::Walking;
    bots::LootPileTarget pile{};
    pile.container_id = container_id;
    pile.source_name = bot.name + " Death LootContainer";
    pile.pos_cm = bot.pos_cm;
    pile.worker_id = bot.worker_id;
    pile.loot_item_guid = 606463310000000900LL + static_cast<int64_t>(bot.replica_index);
    pile.item_quantity = 2;
    pile.ash_currency = 300;
    pile.looted = false;
    return pile;
}

inline bool respawn_at_unlocked_ember_spring(bots::PlayerBot& bot, size_t spring_index) {
    if (!bot.dead || spring_index >= bots::kEmberSprings.size()) return false;
    const auto& spring = bots::kEmberSprings[spring_index];
    bool unlocked = false;
    for (uint64_t id : bot.unlocked_ember_springs) {
        if (id == spring.spring_guid) unlocked = true;
    }
    if (!unlocked) return false;
    bot.pos_cm = spring.location_cm;
    bot.zone_key = spring.region_key;
    bot.worker_id = std::string("aoc-world-") + spring.region_key;
    bot.dead = false;
    bot.hp = bot.max_hp;
    bot.mana = bot.max_mana;
    return bot.hp == bot.max_hp && !bot.dead;
}

inline bool run_plan_death(std::vector<bots::PlayerBot>& roster, std::vector<bots::LootPileTarget>& out_death_piles) {
    if (roster.size() < 16) return false;
    out_death_piles.clear();
    uint32_t spring_mask = 0;
    size_t seq = 0;
    const bool ok = bots::run_all_64_archetype_combinations_all_envs(
        roster, [&](bots::PlayerBot& actor, bots::PlayerBot& observer, bots::BotTestEnv) {
            if (!unlock_all_ember_springs(actor)) return false;
            const size_t sp_idx = seq % bots::kEmberSprings.size();
            const auto pile = apply_lethal_damage_and_spawn_death_pile(actor, 91000ULL + seq++);
            if (out_death_piles.size() < roster.size()) out_death_piles.push_back(pile);
            if (!actor.dead || actor.hp != 0) return false;
            if (!bots::assert_partner_visible_and_updating(actor, observer)) return false;
            if (!respawn_at_unlocked_ember_spring(actor, sp_idx)) return false;
            observer.pos_cm = {actor.pos_cm.x + 150.0, actor.pos_cm.y, actor.pos_cm.z};
            observer.worker_id = actor.worker_id;
            observer.zone_key = actor.zone_key;
            spring_mask |= (1u << sp_idx);
            return bots::assert_partner_visible_and_updating(actor, observer);
        });
    return ok && spring_mask == 0x3Fu && out_death_piles.size() == roster.size();
}

}  // namespace aoc::world::tests::plans
