// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat_test_fixture.h"

#include "bootstrap/constants.h"

#include <set>

namespace sandal::worldd::tests {
namespace {

struct BurnResult {
    bool   ok = false;
    double pool = 0, dur = 0, pool_added = 0, expected_tick = 0;
    double formula_pool = 0;
};

struct BurnTick {
    double  at = 0, value = 0, pool = 0, dur = 0;
    uint8_t flags = 0;
    bool    predicted = false;
    int64_t effect = 0;
};

struct BurnEnv {
    Fixture&                     f;
    Rig                          rig;
    combat_rig::World            w;
    const abilities::HitDef*     application = nullptr;
    const abilities::HitDef*     tick_hit = nullptr;
    const abilities::EffectDef*  burning_stat = nullptr;
    const abilities::EffectDef*  aura = nullptr;
    const combat::CombatStatIds& ids;
    std::string                  wolf_path;
    combat::NpcStatBook          book;
    combat::CombatWorld          world;
    combat_rig::FakePlayer       player;
    bool                         dump = false;
    double                       t0 = 100.0;

    explicit BurnEnv(Fixture& fixture)
        : f(fixture),
          rig(8, 25, {}),
          application(f.catalog->hit_named("Mage_EE_Fire_Damage")),
          tick_hit(f.catalog->hit_named("Status_BurningStatTick_Dmg")),
          burning_stat(f.catalog->effect_named("Status_BurningStat")),
          aura(f.catalog->effect_named("Status_BurningAura")),
          ids(f.data->ids()),
          wolf_path(creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C")),
          book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          player(rig, 77, {0, 0, 1098}),
          dump(std::getenv("AOC_EFFECT_DUMP") != nullptr) {
        if (rig.ok() && w.problem.empty()) world.add_player(&player);
    }

    double value_of(uint64_t wolf, int64_t stat) {
        combat::Fighter* fgt = world.fighter_of(wolf);
        const stats::StatValue* v = fgt ? fgt->stats().find(stat) : nullptr;
        return v ? static_cast<double>(v->value) : -1.0;
    }

    double value_of_player(int64_t stat) const {
        const stats::StatValue* v = rig.cs->find(stat);
        return v ? static_cast<double>(v->value) : -1.0;
    }

    std::vector<std::pair<std::string, combat::EffectItemWrite>> items_of(uint64_t wolf) const {
        std::vector<std::pair<std::string, combat::EffectItemWrite>> out;
        if (const world::WorldActor* a = w.registry.find(wolf)) {
            for (const combat::EffectItemWrite& it : combat::read_effect_items(*a)) {
                const abilities::EffectDef* d = f.catalog->effect(it.item.effect_guid);
                out.emplace_back(d ? d->row.name : std::string("?"), it);
            }
        }
        return out;
    }

    std::optional<combat::EffectItemWrite> item_named(uint64_t wolf, const char* name) const {
        for (const auto& [n, it] : items_of(wolf)) {
            if (n == name) return it;
        }
        return std::nullopt;
    }

    void print_items(uint64_t wolf, double since, const char* when) const {
        if (!dump) return;
        std::printf("    [effect items %s, t0 + %.3f s]\n", when, since);
        for (const auto& [n, it] : items_of(wolf)) {
            std::printf("      id %lld  %-22s start t0 + %.3f  duration %.3f  tick %.2f  stacks %d  from %08x%08x%08x%08x\n",
                        static_cast<long long>(it.item.effect_id), n.c_str(), it.item.start_time - bootstrap::effect_clock(100.0),
                        static_cast<double>(it.item.duration), static_cast<double>(it.item.tick_time), it.item.stacks,
                        it.applier[0], it.applier[1], it.applier[2], it.applier[3]);
        }
    }

    BurnResult burn(uint64_t wolf, double t) {
        BurnResult b;
        combat::Fighter* target = world.fighter_of(wolf);
        if (!target) return b;
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeReaction;
        combat::Resolved r;
        std::string why;
        if (!world.resolver().resolve(player.fighter(), *target, *application, meta, r, why)) return b;
        for (combat::ResolvedMod& m : r.mods) {
            if (m.stat_guid != ids.status_burning) continue;
            b.formula_pool = m.value;
            b.pool_added = m.value;
        }
        world.apply_hit_to_npc(wolf, player.actor_id(), r, t);
        b.pool = value_of(wolf, ids.status_burning);
        b.dur = value_of(wolf, ids.status_burning_dur);
        combat::HitMeta tick_meta;
        tick_meta.source_type = content::kSourceTypeEffect;
        tick_meta.source_guid = burning_stat->row.guid;
        for (int tries = 0; tries < 60; ++tries) {
            combat::Resolved probe;
            if (world.resolver().resolve(player.fighter(), *target, *tick_hit, tick_meta, probe, why) && probe.flags == 0 && !probe.mods.empty()) {
                b.expected_tick = probe.mods[0].value;
                break;
            }
        }
        combat::Resolved gate;
        if (!world.resolver().resolve_effects(player.fighter(), *target, *application, meta, gate, why)) return b;
        world.apply_effects(wolf, player.actor_id(), gate.effects, t);
        b.ok = true;
        return b;
    }

    void drain(uint64_t wolf, std::vector<BurnTick>& ticks) {
        for (const combat::EffectHitNote& n : player.effect_notes) {
            if (n.holder != wolf || n.hit_guid != tick_hit->row.guid) continue;
            ticks.push_back({n.at, n.mods.empty() ? 0.0 : n.mods[0].value, value_of(wolf, ids.status_burning),
                             value_of(wolf, ids.status_burning_dur), n.flags, n.predicted, n.effect_guid});
        }
        player.effect_notes.clear();
    }

    void run(uint64_t wolf, double from, double to, std::vector<BurnTick>& ticks) {
        size_t before = ticks.size();
        for (double t = from; t <= to + 1e-9; t += 0.05) {
            world.tick(t);
            drain(wolf, ticks);
            if (ticks.size() != before) {
                print_items(wolf, t - t0, "after a tick");
                before = ticks.size();
            }
        }
    }
};

bool check_fresh_burn_application(TestHarness& h, BurnEnv& env, uint64_t wolf, std::vector<BurnTick>& ticks, BurnResult& b) {
    b = env.burn(wolf, env.t0);
    CHECK(b.ok, "the Fire Empowerment's hit lands on the Wolf and its effect goes on");
    CHECKF(b.formula_pool > 0.0, "the burn's formula gives the Mage a pool above 0 (%.4f for a level-25 Mage with %.2f magical power)",
           b.formula_pool, env.value_of_player(env.f.data->profile().magical.damage));
    CHECKF(near(b.dur, 8.0, 1e-4) && b.pool > 0.0 && near(b.pool, b.pool_added, b.pool * 1e-5),
           "the hit set the burn: %.2f fire damage over %.1f s (the hit's formula gave %.2f)", b.pool, b.dur, b.pool_added);
    env.drain(wolf, ticks);
    CHECKF(ticks.size() == 1 && !ticks[0].predicted && ticks[0].effect == env.burning_stat->row.guid && ticks[0].value < 0,
           "the first tick comes with the application: one damage tick from Status_BurningStat, not predicted by the client (%zu)", ticks.size());
    const auto first_stat = env.item_named(wolf, "Status_BurningStat");
    const auto first_aura = env.item_named(wolf, "Status_BurningAura");
    CHECK(first_stat && first_aura, "the creature's effect list has the burn's one-second effect and its aura");
    if (!first_stat || !first_aura) return false;
    CHECKF(near(first_stat->item.duration, 1.0, 1e-4) && near(first_aura->item.duration, 8.0, 1e-4) && first_aura->item.stacks == 1 &&
               first_stat->item.stacks == 1 && first_stat->item.start_time == first_aura->item.start_time && first_aura->applier == env.player.guid,
           "...the effect lasts 1.0 s and the aura 8.0 s (%.3f, %.3f), one stack each, from one start, from the player",
           static_cast<double>(first_stat->item.duration), static_cast<double>(first_aura->item.duration));
    env.print_items(wolf, 0.0, "at the application");
    env.run(wolf, env.t0 + 0.05, env.t0 + 0.55, ticks);
    const auto stat_a = env.item_named(wolf, "Status_BurningStat");
    env.run(wolf, env.t0 + 0.6, env.t0 + 1.55, ticks);
    const auto stat_b = env.item_named(wolf, "Status_BurningStat");
    const auto aura_b = env.item_named(wolf, "Status_BurningAura");
    CHECK(stat_a && stat_b && aura_b && stat_b->item.effect_id > stat_a->item.effect_id && aura_b->item.effect_id == first_aura->item.effect_id &&
              aura_b->item.start_time == first_aura->item.start_time,
          "a second on, the one-second effect is a new item (put on again as it ends) and the aura is the same item, not refreshed while it burns");
    return true;
}

void check_fresh_burn_ticks(TestHarness& h, BurnEnv& env, uint64_t wolf, std::vector<BurnTick>& ticks, const BurnResult& b) {
    env.run(wolf, env.t0 + 1.6, env.t0 + 9.0, ticks);
    CHECKF(ticks.size() == 8, "a fresh burn ticks 8 times (%zu)", ticks.size());
    double worst_gap = 0;
    for (size_t i = 1; i < ticks.size(); ++i) worst_gap = std::max(worst_gap, std::fabs(ticks[i].at - ticks[i - 1].at - 1.0));
    CHECKF(worst_gap <= 0.05, "...one every second (the worst gap is off by %.4f s)", worst_gap);
    int plain = 0;
    bool same = true, falls = true;
    for (size_t i = 0; i < ticks.size(); ++i) {
        const bool crit = (ticks[i].flags & stats::kHitCrit) != 0;
        const bool miss = (ticks[i].flags & stats::kHitMiss) != 0;
        if (!crit && !miss) {
            ++plain;
            same = same && near(ticks[i].value, b.expected_tick, std::fabs(b.expected_tick) * 0.01);
        } else if (crit) {
            same = same && std::fabs(ticks[i].value) > std::fabs(b.expected_tick);
        }
        const double left = static_cast<double>(8 - static_cast<int>(i) - 1);
        falls = falls && near(ticks[i].dur, left, 1e-4) && near(ticks[i].pool, b.pool * left / 8.0, b.pool * 1e-4);
    }
    CHECKF(plain >= 4 && same && b.expected_tick < 0,
           "...each tick is the pool over its seconds through the damage equation, the same every second (%.3f expected; %d of 8 without a critical hit or miss)",
           b.expected_tick, plain);
    CHECK(falls, "...and after each tick the pool and the seconds have fallen by one share: pool x (8 - k) / 8 over 8 - k seconds");
    CHECK(near(env.value_of(wolf, env.ids.status_burning), 0.0, 1e-6) && near(env.value_of(wolf, env.ids.status_burning_dur), 0.0, 1e-6),
          "the burn is used up: nothing left to burn and no seconds left");
    CHECK(env.world.effect_count(wolf) == 0 && env.items_of(wolf).empty(), "...the creature's effect list is empty again (the aura ended with the last second)");
    double sum = 0;
    for (const BurnTick& tk : ticks) sum += tk.value;
    CHECKF(near(env.world.max_health_of(wolf) - env.world.health_of(wolf), -sum, 0.5),
           "the Wolf lost what the ticks took: %.2f of %.2f Health", env.world.max_health_of(wolf) - env.world.health_of(wolf), -sum);
}

void check_burn_effect_confirmation(TestHarness& h, BurnEnv& env, uint64_t wolf, double first_val) {
    combat::EffectHitNote note;
    note.holder = wolf;
    note.hit_guid = env.tick_hit->row.guid;
    note.effect_guid = env.burning_stat->row.guid;
    note.mods.push_back({env.tick_hit->mods[0].stat_mod_guid, content::kGuidHealth, first_val});
    note.effective.push_back(first_val);
    note.at = env.t0;
    combat::WorldHitPort port(env.world, env.w.registry, env.player.fighter());
    env.rig.pipe->set_combat(&port);
    env.rig.pipe->confirm_effect_hit(note, env.t0);
    const std::vector<Seen> seen = env.rig.take();
    const bootstrap::HitConfirm* conf = nullptr;
    for (const Seen& s : seen) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == env.tick_hit->row.guid) conf = &c;
        }
    }
    const bool valid_source = conf && conf->source_type == content::kSourceTypeEffect && conf->source_guid == env.burning_stat->row.guid &&
                              !conf->source_is_actor && conf->source && conf->source->ObjectId == wolf && !conf->instigator;
    const bool valid_target = conf && conf->targets.size() == 1 && conf->targets[0].component.ObjectId == wolf &&
                              conf->targets[0].mods.size() == 1 && conf->targets[0].mods[0].value < 0 &&
                              !conf->predicted_autonomous && !conf->predicted_simulated;
    CHECK(valid_source && valid_target,
          "a tick is confirmed as an effect's hit: the player is its instigator, the Wolf its source and its target, the number is the tick's, and neither client predicted it");
    env.rig.pipe->set_combat(nullptr);
}

void check_second_burning_hit_and_cap(TestHarness& h, BurnEnv& env) {
    const double t0 = env.t0;
    const uint64_t wolf4 = env.w.spawn(env.wolf_path, 600, 120, 45, 100);
    std::vector<BurnTick> ticks4;
    env.burn(wolf4, t0);
    env.drain(wolf4, ticks4);
    env.run(wolf4, t0 + 0.05, t0 + 3.5, ticks4);
    CHECKF(ticks4.size() == 4 && near(env.value_of(wolf4, env.ids.status_burning_dur), 4.0, 1e-4),
           "(after 3.5 s the burn has ticked 4 times and has 4 s left: %zu, %.1f)", ticks4.size(), env.value_of(wolf4, env.ids.status_burning_dur));
    const double pool_before = env.value_of(wolf4, env.ids.status_burning);
    const BurnResult again = env.burn(wolf4, t0 + 3.5);
    CHECKF(near(again.dur, 8.0, 1e-4) && near(again.pool, pool_before + again.pool_added, again.pool * 1e-4),
           "a second burning hit adds its pool and max(8 - left, 4) seconds: %.2f over %.1f s (it was %.2f over 4 s, the hit added %.2f)",
           again.pool, again.dur, pool_before, again.pool_added);
    env.drain(wolf4, ticks4);
    CHECKF(ticks4.size() == 5 && near(ticks4.back().at, t0 + 3.5, 1e-6), "...and the extra application ticks at once (%zu ticks so far)", ticks4.size());
    env.run(wolf4, t0 + 3.55, t0 + 14.0, ticks4);
    CHECKF(ticks4.size() == 12 && near(ticks4.back().at, t0 + 10.5, 0.05),
           "...so it ticked 12 times in all, the last at 10.5 s (%zu, %.3f)", ticks4.size(), ticks4.empty() ? 0.0 : ticks4.back().at - t0);
    CHECK(env.world.effect_count(wolf4) == 0 && near(env.value_of(wolf4, env.ids.status_burning), 0.0, 1e-6), "...and then nothing is left on it");

    const uint64_t wolf6 = env.w.spawn(env.wolf_path, 600, 240, 45, 100);
    for (int i = 0; i < 10; ++i) env.burn(wolf6, t0);
    const double capped = env.value_of(wolf6, env.ids.status_burning_dur);
    CHECKF(capped > 8.0 && capped <= 24.0 + 1e-6, "ten burning hits at once cannot make it last more than 24 s (%.1f)", capped);
}

void check_burn_kill_leash_and_leave(TestHarness& h, BurnEnv& env) {
    const double t0 = env.t0;
    const uint64_t wolf2 = env.w.spawn(env.wolf_path, 600, 360, 45, 100);
    std::vector<BurnTick> ticks2;
    env.burn(wolf2, t0);
    env.drain(wolf2, ticks2);
    env.run(wolf2, t0 + 0.05, t0 + 2.5, ticks2);
    CHECKF(ticks2.size() == 3 && env.world.effect_count(wolf2) >= 2, "(a second Wolf burns: %zu ticks in 2.5 s)", ticks2.size());
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    env.world.apply_hit_to_npc(wolf2, 77, kill, t0 + 2.5);
    CHECK(env.world.dead(wolf2) && env.world.effect_count(wolf2) == 0 && env.items_of(wolf2).empty(),
          "a kill ends the burn: the Wolf is dead, holds no effect, and its effect list is empty");
    env.run(wolf2, t0 + 2.55, t0 + 12.0, ticks2);
    CHECKF(ticks2.size() == 3 && env.world.health_of(wolf2) == 0.0 && env.world.effect_count(wolf2) == 0,
           "...and nothing ticks after it (%zu ticks, Health %.1f)", ticks2.size(), env.world.health_of(wolf2));

    const uint64_t wolf3 = env.w.spawn(env.wolf_path, 600, 480, 45, 100);
    std::vector<BurnTick> ticks3;
    env.burn(wolf3, t0);
    env.drain(wolf3, ticks3);
    env.run(wolf3, t0 + 0.05, t0 + 2.5, ticks3);
    env.world.gm_reset(wolf3, t0 + 2.5);
    CHECKF(ticks3.size() == 3 && env.world.effect_count(wolf3) >= 2 && env.item_named(wolf3, "Status_BurningAura"),
           "a reset leaves its effects: the Wolf still holds them and shows the aura (%zu effects)", env.world.effect_count(wolf3));
    env.run(wolf3, t0 + 2.55, t0 + 9.0, ticks3);
    CHECKF(ticks3.size() == 8 && near(env.value_of(wolf3, env.ids.status_burning), 0.0, 1e-6),
           "...and the burn goes on ticking after the reset: 8 ticks in all (%zu), the pool used up", ticks3.size());

    const uint64_t wolf5 = env.w.spawn(env.wolf_path, 600, 600, 45, 100);
    std::vector<BurnTick> ticks5;
    env.burn(wolf5, t0);
    env.drain(wolf5, ticks5);
    env.run(wolf5, t0 + 0.05, t0 + 2.5, ticks5);
    env.world.remove_player(&env.player);
    CHECK(env.world.effect_count(wolf5) == 0 && env.items_of(wolf5).empty() && near(env.value_of(wolf5, env.ids.status_burning), 0.0, 1e-6),
          "a burn ends when the player who lit it leaves: nothing is held, the list is empty, nothing is left to burn");
    env.world.add_player(&env.player);
    env.run(wolf5, t0 + 2.55, t0 + 9.0, ticks5);
    CHECKF(ticks5.size() == 3, "...and it ticks no more (%zu ticks)", ticks5.size());
    CHECK(env.rig.pawn.broken.empty(), "(the pawn took every change)");
}

void test_creature_burn(TestHarness& h) {
    std::printf("a creature takes a burn: its pool and seconds, one tick a second for 8 s, the aura, the end; a second burning hit; a kill, a leash reset and the player leaving\n");
    BurnEnv env(h.f);
    CHECKF(env.rig.ok(), "the rig: %s", env.rig.problem.c_str());
    if (!env.rig.ok() || !env.w.problem.empty()) return;
    CHECK(env.application && env.tick_hit && env.burning_stat && env.aura && env.ids.status_burning != 0 && env.ids.status_burning_dur != 0,
          "the burn's application hit, its tick, its two effects and its two status stats are in the data");
    if (!env.application || !env.tick_hit || !env.burning_stat || !env.aura || env.ids.status_burning == 0 || env.ids.status_burning_dur == 0) return;

    const uint64_t wolf = env.w.spawn(env.wolf_path, 600, 0, 45, 100);
    CHECK(wolf != 0, "a Wolf 6 m away");
    if (!wolf) return;
    std::vector<BurnTick> ticks;
    BurnResult b;
    if (!check_fresh_burn_application(h, env, wolf, ticks, b)) return;
    check_fresh_burn_ticks(h, env, wolf, ticks, b);
    check_burn_effect_confirmation(h, env, wolf, ticks.empty() ? 0.0 : ticks[0].value);
    check_second_burning_hit_and_cap(h, env);
    check_burn_kill_leash_and_leave(h, env);
}

bool setup_wand_burning_rig(TestHarness& h, Rig& rig) {
    const talents::Tree* wand = rig.talent_catalog->tree_named("Weapon_Ranged_Wand");
    const talents::Node* burning = nullptr;
    if (wand) {
        for (const talents::Node& n : wand->nodes) {
            if (n.skill_name == "Weapon_Finisher_Burning" && !n.ranks.empty()) burning = &n;
        }
    }
    CHECK(wand && burning, "the wand's tree and its Burning pick");
    if (!wand || !burning) return false;
    rig.book->load(wand->guid, {burning->ranks.front().guid});
    rig.know({"WeaponCombo_Ranged_Wand"});
    rig.cs->set_passives(rig.book->passives(wand->weapon_line));
    rig.cs->set_ranks(rig.book->held_ranks(wand->weapon_line));
    rig.pipe->set_reaction_carriers(rig.book->reaction_carriers(wand->weapon_line));
    rig.pipe->refresh(rig.clock, "the wand in hand", true);
    rig.take();
    return true;
}

int64_t find_combo_hit_targeting(const abilities::AbilityDef& combo, int64_t hit_guid) {
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& hr : combo.hits) {
        if (hr.hit_guid != hit_guid || hr.source != "hit") continue;
        for (const content::TargetingRow& tg : combo.targetings) {
            if (tg.key == hr.key) targeting = tg.guid;
        }
    }
    return targeting;
}

void test_creature_burn_from_the_wand(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the wand's Burning finisher burns a creature: the reaction's hit is not confirmed, the burn's first tick is\n");
    Rig rig(8, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok() || !setup_wand_burning_rig(h, rig)) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 600, 0, 45, 100);
    combat_rig::FakePlayer player(rig, 77, {0, 0, 1098});
    world.add_player(&player);
    combat::WorldHitPort port(world, w.registry, player.fighter());
    rig.pipe->set_combat(&port);
    const abilities::AbilityDef* wand_combo = f.catalog->ability_named("WeaponCombo_Ranged_Wand");
    const abilities::HitDef* beam_end = f.catalog->hit_named("Weapon_Wand_Beam_ComboEnd_Hit");
    const abilities::HitDef* burning_hit = f.catalog->hit_named("Weapon_Finisher_Burning");
    const abilities::HitDef* tick_hit = f.catalog->hit_named("Status_BurningStatTick_Dmg");
    const combat::CombatStatIds& ids = f.data->ids();
    CHECK(wolf != 0 && wand_combo && beam_end && burning_hit && tick_hit, "a Wolf, the wand's combo, its finishing hit and the Burning reaction's hit");
    if (!wolf || !wand_combo || !beam_end || !burning_hit || !tick_hit) return;
    const int64_t targeting = find_combo_hit_targeting(*wand_combo, beam_end->row.guid);

    constexpr double t = 100.0;
    TriggerInfo info;
    info.animation_index = 4;
    info.target_count = 1;
    info.target_actors = {wolf};
    CHECK(rig.pipe->trigger(wand_combo->row.guid, 500, t, info) == Refusal::None, "the wand's combo is cast at the Wolf");
    rig.run_to(t + 0.4);
    CHECK(rig.pipe->hit_request(wand_combo->row.guid, targeting, beam_end->row.guid, 1, 0, t + 0.8, {wolf}) == AbilityPipeline::HitVerdict::Scheduled,
          "the beam's end is asked for");
    int reaction_confirmed = 0, beam_confirmed = 0;
    for (const Seen& s : rig.run_to(t + 1.6)) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            reaction_confirmed += (c.record_guid == burning_hit->row.guid && c.source_type == content::kSourceTypeReaction) ? 1 : 0;
            beam_confirmed += c.record_guid == beam_end->row.guid ? 1 : 0;
        }
    }
    CHECKF(rig.pipe->reactions_fired() >= 1, "the Burning reaction fired (%zu)", rig.pipe->reactions_fired());
    CHECKF(beam_confirmed == 1 && reaction_confirmed == 0,
           "the beam's end is confirmed (%d) and the reaction's hit, which only raises the burn, is not (%d)", beam_confirmed, reaction_confirmed);
    const double pool = world.fighter_of(wolf)->stats().find(ids.status_burning)->value;
    const double dur = world.fighter_of(wolf)->stats().find(ids.status_burning_dur)->value;
    CHECKF(pool > 0 && near(dur, 7.0, 1e-4),
           "the Wolf burns: %.2f fire damage to burn over %.1f s (a fresh 8 s burn, 1 s spent on the first tick)", pool, dur);
    const world::WorldActor* actor = w.registry.find(wolf);
    bool has_aura = false, has_stat = false;
    if (actor) {
        for (const combat::EffectItemWrite& it : combat::read_effect_items(*actor)) {
            const abilities::EffectDef* d = f.catalog->effect(it.item.effect_guid);
            has_aura = has_aura || (d && d->row.name == "Status_BurningAura");
            has_stat = has_stat || (d && d->row.name == "Status_BurningStat");
        }
    }
    CHECK(has_aura && has_stat, "...and shows the burn: the aura and the one-second effect are in its effect list");
    CHECKF(player.effect_notes.size() == 1 && player.effect_notes[0].hit_guid == tick_hit->row.guid,
           "the first tick is waiting to be confirmed to the player (%zu)", player.effect_notes.size());
    player.deliver_effect_notes();
    bool tick_confirmed = false;
    for (const bootstrap::HitConfirm& c : rig.pawn.confirmations) {
        tick_confirmed = tick_confirmed || (c.record_guid == tick_hit->row.guid && c.source_type == content::kSourceTypeEffect &&
                                            !c.targets.empty() && c.targets[0].component.ObjectId == wolf);
    }
    CHECK(tick_confirmed, "...and the session confirms it: an effect's hit on the Wolf");
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}

struct BurnResetEnv {
    using AiState = npc::ai::State;
    struct Tick {
        double  at = 0, value = 0;
        AiState state = AiState::Idle;
    };
    struct Walk {
        bool              moved = false, reset = false, arrived = false;
        double            reset_at = 0, arrived_at = 0;
        int               steps = 0, with_threat = 0, with_flag = 0, with_target = 0, fights_begun = 0;
        std::vector<Tick> ticks;
        double            lost_after = 0;
    };

    Fixture&                     f;
    Rig                          rig;
    combat_rig::World            w;
    const abilities::HitDef*     application = nullptr;
    const abilities::HitDef*     tick_hit = nullptr;
    const combat::CombatStatIds& ids;
    std::string                  wolf_path;
    combat::NpcStatBook          book;
    combat::CombatWorld          world;
    combat_rig::FakePlayer       player;
    double                       now = 1.0;

    explicit BurnResetEnv(Fixture& fixture)
        : f(fixture),
          rig(8, 25, {}),
          application(f.catalog->hit_named("Mage_EE_Fire_Damage")),
          tick_hit(f.catalog->hit_named("Status_BurningStatTick_Dmg")),
          ids(f.data->ids()),
          wolf_path(creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C")),
          book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          player(rig, 77, {0, 0, 1098}) {
        if (rig.ok() && w.problem.empty()) world.add_player(&player);
    }

    double value_of_player(int64_t stat) const {
        const stats::StatValue* v = rig.cs->find(stat);
        return v ? static_cast<double>(v->value) : -1.0;
    }

    double value_of(uint64_t wolf, int64_t stat) {
        combat::Fighter* fgt = world.fighter_of(wolf);
        const stats::StatValue* v = fgt ? fgt->stats().find(stat) : nullptr;
        return v ? static_cast<double>(v->value) : -1.0;
    }

    bool burn(uint64_t wolf, double t) {
        combat::Fighter* target = world.fighter_of(wolf);
        if (!target) return false;
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeReaction;
        combat::Resolved r;
        std::string why;
        if (!world.resolver().resolve(player.fighter(), *target, *application, meta, r, why)) return false;
        world.apply_hit_to_npc(wolf, player.actor_id(), r, t);
        combat::Resolved gate;
        if (!world.resolver().resolve_effects(player.fighter(), *target, *application, meta, gate, why)) return false;
        world.apply_effects(wolf, player.actor_id(), gate.effects, t);
        return true;
    }

    void step(uint64_t wolf, std::vector<Tick>& ticks) {
        const std::optional<AiState> before = world.ai_state(wolf);
        now += 0.05;
        w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
        w.npcs->tick(now);
        world.tick(now);
        for (const combat::EffectHitNote& n : player.effect_notes) {
            if (n.holder == wolf && n.hit_guid == tick_hit->row.guid) {
                ticks.push_back({n.at, n.mods.empty() ? 0.0 : n.mods[0].value, before.value_or(AiState::Idle)});
            }
        }
        player.effect_notes.clear();
    }

    bool in_combat_flag(uint64_t wolf) const {
        const world::WorldActor* a = w.registry.find(wolf);
        return a && combat_rig::combat_flag(*a, "bInCombat");
    }

    uint64_t target_of(uint64_t wolf) const {
        const world::WorldActor* a = w.registry.find(wolf);
        return a ? combat_rig::active_target(*a) : uint64_t{0};
    }

    void record_reset_step(uint64_t wolf, Walk& out) {
        if (!out.reset) {
            out.reset = true;
            out.reset_at = now;
        }
        ++out.steps;
        const npc::ai::NpcAi* ai = world.ai_of(wolf);
        if (ai && !ai->threat().empty()) ++out.with_threat;
        if (in_combat_flag(wolf)) ++out.with_flag;
        if (target_of(wolf) != 0) ++out.with_target;
    }

    void drag_and_watch(uint64_t wolf, double home_x, double drag_y, double give_up_after, Walk& out) {
        const npc::NpcWorld::RelocateResult rel = w.npcs->relocate(wolf, home_x, drag_y, 1000.0, false);
        out.moved = rel.status == npc::NpcWorld::Relocated::Ok;
        w.npcs->restore_home(wolf, nullptr);
        const uint64_t aggros0 = world.stats().aggros;
        const double started = now;
        while (now < started + give_up_after) {
            step(wolf, out.ticks);
            const std::optional<AiState> st = world.ai_state(wolf);
            if (st && *st == AiState::Reset) {
                record_reset_step(wolf, out);
            } else if (out.reset) {
                out.arrived = true;
                out.arrived_at = now;
                break;
            }
        }
        out.fights_begun = static_cast<int>(world.stats().aggros - aggros0);
        for (const Tick& tk : out.ticks) {
            if (tk.state == AiState::Reset) out.lost_after += -tk.value;
        }
    }

    static int ticks_in_reset(const Walk& walk) {
        int n = 0;
        for (const Tick& tk : walk.ticks) n += tk.state == AiState::Reset ? 1 : 0;
        return n;
    }
};

void check_burn_ends_on_way_home(TestHarness& h, BurnResetEnv& env) {
    using AiState = BurnResetEnv::AiState;
    const uint64_t wolf_b = env.w.spawn(env.wolf_path, 600, 2400, 45, 100);
    CHECK(wolf_b != 0, "a Wolf 25 m from the player");
    if (!wolf_b) return;
    CHECK(env.burn(wolf_b, env.now), "...burned once: 8 s");
    std::vector<BurnResetEnv::Tick> home_ticks_b;
    const double lit_b = env.now;
    while (env.now < lit_b + 1.5) env.step(wolf_b, home_ticks_b);
    const double max_hp_b = env.world.max_health_of(wolf_b);
    BurnResetEnv::Walk walk_b;
    env.drag_and_watch(wolf_b, 600, 7400, 30.0, walk_b);
    CHECK(walk_b.moved && walk_b.reset && walk_b.arrived, "dragged 50 m from its home it resets and walks home");
    const int b_ticks = BurnResetEnv::ticks_in_reset(walk_b);
    CHECKF(b_ticks >= 4 && walk_b.arrived_at - lit_b > 8.0,
           "the burn ended on the way: %d of its ticks landed during the walk, and it arrived %.1f s after the burn began",
           b_ticks, walk_b.arrived ? walk_b.arrived_at - lit_b : -1.0);
    CHECKF(walk_b.with_threat == 0 && walk_b.with_flag == 0 && walk_b.with_target == 0 && walk_b.fights_begun == 0,
           "...no threat and no fight on the way (threat in %d of %d steps, flag in %d, target in %d, fights %d)",
           walk_b.with_threat, walk_b.steps, walk_b.with_flag, walk_b.with_target, walk_b.fights_begun);
    CHECKF(near(max_hp_b - env.world.health_of(wolf_b), walk_b.lost_after, 0.5),
           "...and the ticks of the walk took %.2f Health (%.2f gone)", walk_b.lost_after, max_hp_b - env.world.health_of(wolf_b));
    std::printf("    1: burn of 8 s, dragged 50 m: its reset began %.2f s after the burn, it was home %.2f s after it (%.2f s walking); %d of the 8 ticks landed on the way, %.2f Health lost to them; threat table filled in %d, combat flag up in %d, fights begun %d\n",
                walk_b.reset_at - lit_b, walk_b.arrived_at - lit_b, walk_b.arrived_at - walk_b.reset_at, b_ticks,
                walk_b.lost_after, walk_b.with_threat, walk_b.with_flag, walk_b.fights_begun);
    CHECK(env.world.effect_count(wolf_b) == 0 && near(env.value_of(wolf_b, env.ids.status_burning), 0.0, 1e-6),
          "the burn is over: nothing is left on it");
    const uint64_t fights_b = env.world.stats().aggros;
    std::vector<BurnResetEnv::Tick> rest;
    const double rest_from = env.now;
    while (env.now < rest_from + 3.0) env.step(wolf_b, rest);
    const std::optional<AiState> st_b = env.world.ai_state(wolf_b);
    CHECKF(st_b && *st_b == AiState::Idle && env.world.stats().aggros == fights_b && !env.in_combat_flag(wolf_b) && env.target_of(wolf_b) == 0 && rest.empty(),
           "it stands idle at home for the next 3 s: no tick, no fight, no combat flag (%zu ticks)", rest.size());
}

void check_burn_restarts_fight_at_home(TestHarness& h, BurnResetEnv& env, uint64_t wolf_a, double lit_at, const BurnResetEnv::Walk& walk_a) {
    using AiState = BurnResetEnv::AiState;
    std::vector<BurnResetEnv::Tick> after;
    const uint64_t fights_home = env.world.stats().aggros;
    double fight_at = -1;
    int idle_steps = 0;
    while (env.now < walk_a.arrived_at + 2.0 && fight_at < 0) {
        env.step(wolf_a, after);
        const std::optional<AiState> st = env.world.ai_state(wolf_a);
        if (st && (*st == AiState::Chase || *st == AiState::Attack)) {
            fight_at = env.now;
        } else if (st && *st == AiState::Idle) {
            ++idle_steps;
        }
    }
    const int a_ticks = BurnResetEnv::ticks_in_reset(walk_a);
    CHECKF(!after.empty() && fight_at > 0 && std::fabs(fight_at - after.front().at) <= 0.12,
           "the first tick after it is home starts the fight again: the tick at %.2f, the fight at %.2f (%d idle steps in between)",
           after.empty() ? -1.0 : after.front().at - lit_at, fight_at - lit_at, idle_steps);
    CHECKF(env.world.stats().aggros == fights_home + 1 && env.in_combat_flag(wolf_a) && env.target_of(wolf_a) == 77,
           "...against the player who burns it: one fight began (%llu), its combat flag is up and it names him",
           static_cast<unsigned long long>(env.world.stats().aggros - fights_home));
    std::printf("    2: burn of 24 s, dragged 32 m: its reset began %.2f s after the burn, it was home %.2f s after it (%.2f s walking); %d ticks landed on the way, %.2f Health lost to them; threat table filled in %d, combat flag up in %d, fights begun %d; the first tick after it was home came %.2f s after it arrived and the fight began in the same step\n",
                walk_a.reset_at - lit_at, walk_a.arrived_at - lit_at, walk_a.arrived_at - walk_a.reset_at, a_ticks,
                walk_a.lost_after, walk_a.with_threat, walk_a.with_flag, walk_a.fights_begun,
                after.empty() ? -1.0 : after.front().at - walk_a.arrived_at);
    const npc::ai::NpcAi* ai_a = env.world.ai_of(wolf_a);
    CHECK(ai_a && !ai_a->threat().empty() && ai_a->target() == 77, "...and the tick's damage is threat on its table, as a hit's would be");
    CHECK(env.rig.pawn.broken.empty(), "(the pawn took every change)");
}

void check_burn_outlasts_walk_home(TestHarness& h, BurnResetEnv& env) {
    using AiState = BurnResetEnv::AiState;
    const uint64_t wolf_a = env.w.spawn(env.wolf_path, 600, 3200, 45, 100);
    CHECK(wolf_a != 0, "a Wolf 32 m from the player (a level 45 Wolf kills the rig's 894 Health in two bites: it must not reach him before it is dragged away)");
    if (!wolf_a) return;
    bool lit = true;
    for (int i = 0; i < 5; ++i) lit = env.burn(wolf_a, env.now) && lit;
    CHECK(lit, "the Fire Empowerment's hit lands on the Wolf five times: a burn of 24 s");
    std::vector<BurnResetEnv::Tick> home_ticks;
    const double lit_at = env.now;
    while (env.now < lit_at + 1.5) env.step(wolf_a, home_ticks);
    const std::optional<AiState> fighting = env.world.ai_state(wolf_a);
    const world::WorldActor* wa_now = env.w.registry.find(wolf_a);
    CHECKF(fighting && (*fighting == AiState::Chase || *fighting == AiState::Attack) && env.in_combat_flag(wolf_a) && env.target_of(wolf_a) == 77,
           "burning, it fights the player who burns it: its AI is in a fight and its combat flag is up (its AI is %s, the flag %d, it names %llu; the player has %.0f Health; it stands at %.0f, %.0f; %zu effects)",
           fighting ? npc::ai::state_name(*fighting) : "not a combatant", env.in_combat_flag(wolf_a) ? 1 : 0,
           static_cast<unsigned long long>(env.target_of(wolf_a)), env.value_of_player(content::kGuidHealth),
           wa_now ? wa_now->x : -1.0, wa_now ? wa_now->y : -1.0, env.world.effect_count(wolf_a));
    CHECKF(home_ticks.size() >= 2, "...and its burn ticks every second (%zu ticks in 1.5 s; %.2f to burn over %.1f s with %zu effects)",
           home_ticks.size(), env.value_of(wolf_a, env.ids.status_burning), env.value_of(wolf_a, env.ids.status_burning_dur), env.world.effect_count(wolf_a));
    const double max_hp = env.world.max_health_of(wolf_a);
    BurnResetEnv::Walk walk_a;
    const uint64_t resets_before = env.world.stats().resets;
    env.drag_and_watch(wolf_a, 600, 6400, 25.0, walk_a);
    CHECK(walk_a.moved && walk_a.reset, "dragged 32 m from its home it breaks its leash: its AI is in its reset");
    CHECKF(env.world.stats().resets == resets_before + 1, "...once (%llu resets)", static_cast<unsigned long long>(env.world.stats().resets - resets_before));
    CHECKF(walk_a.arrived && walk_a.steps >= 100, "...and walks home: it arrives %.1f s after the reset began",
           walk_a.arrived ? walk_a.arrived_at - walk_a.reset_at : -1.0);
    const int a_ticks = BurnResetEnv::ticks_in_reset(walk_a);
    CHECKF(a_ticks >= 5, "the burn ticked %d times on the way home", a_ticks);
    CHECKF(walk_a.with_threat == 0 && walk_a.with_flag == 0 && walk_a.with_target == 0 && walk_a.fights_begun == 0,
           "...and those ticks added no threat and began no fight: the threat table was empty in %d of %d steps, the combat flag was up in %d, a target named in %d, fights begun %d",
           walk_a.with_threat, walk_a.steps, walk_a.with_flag, walk_a.with_target, walk_a.fights_begun);
    CHECKF(near(max_hp - env.world.health_of(wolf_a), walk_a.lost_after, 0.5) && walk_a.lost_after > 0.0,
           "...but they dealt their damage: it healed to full as it turned, then lost %.2f of %.2f Health to the %d ticks of the walk (%.2f gone)",
           walk_a.lost_after, max_hp, a_ticks, max_hp - env.world.health_of(wolf_a));
    CHECKF(walk_a.arrived && env.world.effect_count(wolf_a) >= 2 && env.value_of(wolf_a, env.ids.status_burning) > 0.0,
           "it is home and still burning: its effects and its burn are still on it (%zu effects, %.2f to burn over %.1f s; %zu ticks in all since the drag; %zu before it)",
           env.world.effect_count(wolf_a), env.value_of(wolf_a, env.ids.status_burning), env.value_of(wolf_a, env.ids.status_burning_dur),
           walk_a.ticks.size(), home_ticks.size());
    check_burn_restarts_fight_at_home(h, env, wolf_a, lit_at, walk_a);
}

void test_burning_creature_resets(TestHarness& h) {
    std::printf("a burning creature that resets: its ticks add no threat and start no fight on the way home; the first tick after it is home starts the fight again; a burn that ended on the way leaves it idle\n");
    BurnResetEnv env(h.f);
    CHECKF(env.rig.ok(), "the rig: %s", env.rig.problem.c_str());
    if (!env.rig.ok() || !env.w.problem.empty()) return;
    CHECK(env.application && env.tick_hit && env.ids.status_burning != 0 && env.ids.status_burning_dur != 0,
          "the burn's application hit, its tick and its two status stats are in the data");
    if (!env.application || !env.tick_hit || env.ids.status_burning == 0 || env.ids.status_burning_dur == 0) return;
    check_burn_ends_on_way_home(h, env);
    check_burn_outlasts_walk_home(h, env);
}

void test_session_dice_seed(TestHarness& h) {
    std::printf("the dice of a session\n");
    using combat::session_dice_seed;
    CHECK(session_dice_seed(0, 3) == 3 * 7919u + 1u, "a test's world (seed 0) keeps the plain function of the player id");
    CHECK(session_dice_seed(11, 3) == session_dice_seed(11, 3), "the same run seed and player repeat their rolls (--combat-seed)");
    CHECK(session_dice_seed(11, 3) != session_dice_seed(12, 3), "another start rolls another sequence for the same player");
    CHECK(session_dice_seed(11, 3) != session_dice_seed(11, 4), "two players of one run do not share a sequence");
    bool none_zero = true;
    for (uint64_t run = 1; run < 2000; ++run) none_zero = none_zero && session_dice_seed(run, run * 3) != 0;
    CHECK(none_zero, "a seed is never 0");
}

bool fire_matches_projectile_hit(const Fixture& f, const abilities::AbilityDef& def, int64_t key) {
    for (const content::AbilityHitRow& r : def.hits) {
        if (r.source == "projectile" && r.key == key && f.catalog->projectile(r.owner_guid) != nullptr) return true;
    }
    return false;
}

bool ability_flies_projectile(const Fixture& f, const combat::NpcAbilityData& tables, const abilities::AbilityDef& def, bool& notifies) {
    notifies = false;
    bool flies = false;
    for (const content::MontageRow& m : def.montages) {
        const combat::MontageData* montage = m.soft_path.empty() ? nullptr : tables.montage(m.soft_path);
        if (!montage) continue;
        for (const combat::MontageData::Fire& fire : montage->fires) {
            notifies = true;
            if (fire_matches_projectile_hit(f, def, fire.key)) flies = true;
        }
    }
    return flies;
}

void test_creature_shots_follow_the_fire_notify(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a creature ability counts as a shot where a fire notify of its montage names a projectile row of the ability\n");
    combat::NpcAbilityData tables;
    if (!load_creature_tables(tables)) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    world.set_ability_data(&tables);
    size_t checked = 0, shots = 0, other_notify = 0, differ = 0;
    std::set<int64_t> seen;
    for (const auto& [archetype, tasks] : tables.by_kind()) {
        for (const combat::CreatureAbility& task : tasks) {
            if (!seen.insert(task.ability).second) continue;
            const abilities::AbilityDef* def = f.catalog->ability(task.ability);
            if (!def) continue;
            bool notifies = false;
            const bool flies = ability_flies_projectile(f, tables, *def, notifies);
            ++checked;
            shots += flies ? 1 : 0;
            other_notify += (notifies && !flies) ? 1 : 0;
            differ += world.launches_projectiles(*def) != flies ? 1 : 0;
        }
    }
    std::printf("  %zu abilities of the creature tables: %zu fly a projectile, %zu have a fire notify whose key no projectile row has\n",
                checked, shots, other_notify);
    CHECK(checked > 0 && shots > 0, "the creature tables list abilities, and some of them fly a projectile");
    CHECKF(differ == 0, "the answer of the world agrees with the cast's rule for every one of them (%zu differ)", differ);
}

}  // namespace

void run_combat_burn_tests(TestHarness& h) {
    test_creature_burn(h);
    test_creature_burn_from_the_wand(h);
    test_burning_creature_resets(h);
    test_session_dice_seed(h);
    test_creature_shots_follow_the_fire_notify(h);
}

void run_combat_burn_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db) {
    (void)db;
    run_combat_burn_tests(h);
}

}  // namespace sandal::worldd::tests
