// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat_test_fixture.h"

namespace sandal::worldd::tests {
namespace {

namespace dots {

class BookEffects final : public combat::TargetEffects {
public:
    BookEffects(const combat::CombatWorld& w, uint64_t wolf, double now) : w_(w), wolf_(wolf), now_(now) {}
    double remaining(int64_t effect_guid) const override { return w_.effect_remaining(wolf_, effect_guid, now_); }

private:
    const combat::CombatWorld& w_;
    uint64_t                   wolf_;
    double                     now_;
};

struct Landing {
    bool                          ok = false;
    std::vector<effects::Applied> applied;
    bool has(const char* name) const {
        for (const effects::Applied& a : applied) {
            if (a.def->row.name == name) return true;
        }
        return false;
    }
    const effects::Applied* find(const char* name) const {
        for (const effects::Applied& a : applied) {
            if (a.def->row.name == name) return &a;
        }
        return nullptr;
    }
};

struct Tick {
    double   at = 0, value = 0;
    uint8_t  flags = 0;
    bool     predicted = false;
    int64_t  effect = 0;
    uint64_t by = 0;
    bool plain() const { return (flags & (stats::kHitCrit | stats::kHitMiss)) == 0; }
    bool missed() const { return (flags & stats::kHitMiss) != 0; }
};

struct Bed {
    Fixture&               f;
    Rig                    rig;
    combat_rig::World      w;
    combat::NpcStatBook    book;
    combat::CombatWorld    world;
    combat_rig::FakePlayer player, other;
    std::string            wolf_path;

    explicit Bed(Fixture& fixture)
        : f(fixture),
          rig(8, 25, {}),
          book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          player(rig, 77, {0, 0, 1098}),
          other(rig, 78, {0, 100, 1098}) {
        if (!rig.ok() || !w.problem.empty()) return;
        world.add_player(&player);
        world.add_player(&other);
        wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    }
    bool ok() const { return rig.ok() && w.problem.empty(); }
    std::string problem() const { return !rig.ok() ? rig.problem : w.problem; }

    uint64_t wolf(double y) { return w.spawn(wolf_path, 3000, y, 45, 100); }

    Landing land(uint64_t wolf, const char* name, double t, combat_rig::FakePlayer& by, bool damage = false) {
        Landing out;
        const abilities::HitDef* hit = f.catalog->hit_named(name);
        combat::Fighter* target = world.fighter_of(wolf);
        if (!hit || !target) return out;
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeAbility;
        std::string why;
        if (damage) {
            combat::Resolved r;
            if (!world.resolver().resolve(by.fighter(), *target, *hit, meta, r, why)) return out;
            world.apply_hit_to_npc(wolf, by.actor_id(), r, t);
        }
        const BookEffects view(world, wolf, t);
        meta.target_effects = &view;
        combat::Resolved gate;
        out.ok = world.resolver().resolve_effects(by.fighter(), *target, *hit, meta, gate, why);
        out.applied = gate.effects;
        world.apply_effects(wolf, by.actor_id(), gate.effects, t);
        return out;
    }

    void run(double from, double to) {
        for (double t = from; t <= to + 1e-9; t += 0.05) world.tick(t);
    }

    std::vector<Tick> ticks(uint64_t wolf, const char* hit_name) {
        std::vector<Tick> out;
        const abilities::HitDef* hit = f.catalog->hit_named(hit_name);
        for (combat_rig::FakePlayer* p : {&player, &other}) {
            for (const combat::EffectHitNote& n : p->effect_notes) {
                if (!hit || n.holder != wolf || n.hit_guid != hit->row.guid) continue;
                out.push_back({n.at, n.mods.empty() ? 0.0 : n.mods[0].value, n.flags, n.predicted, n.effect_guid, p->actor_id()});
            }
            p->effect_notes.clear();
        }
        std::sort(out.begin(), out.end(), [](const Tick& a, const Tick& b) { return a.at < b.at; });
        return out;
    }

    std::vector<std::pair<std::string, combat::EffectItemWrite>> items(uint64_t wolf) const {
        std::vector<std::pair<std::string, combat::EffectItemWrite>> out;
        if (const world::WorldActor* a = w.registry.find(wolf)) {
            for (const combat::EffectItemWrite& it : combat::read_effect_items(*a)) {
                const abilities::EffectDef* d = f.catalog->effect(it.item.effect_guid);
                out.emplace_back(d ? d->row.name : std::string("?"), it);
            }
        }
        return out;
    }

    size_t items_named(uint64_t wolf, const char* name) const {
        size_t n = 0;
        for (const auto& [nm, it] : items(wolf)) n += nm == name ? 1 : 0;
        return n;
    }

    int64_t effect_guid(const char* name) const {
        const abilities::EffectDef* d = f.catalog->effect_named(name);
        return d ? d->row.guid : 0;
    }

    double stat_of(uint64_t wolf, int64_t stat) {
        combat::Fighter* g = world.fighter_of(wolf);
        std::string err;
        auto ev = g ? g->stats().evaluation(g->worn(), err, true) : nullptr;
        return ev ? ev->stat(stat) : -1.0e18;
    }

    double plain_damage(combat::Fighter& from, combat::Fighter& to, const char* hit_name) {
        const abilities::HitDef* hit = f.catalog->hit_named(hit_name);
        if (!hit) return 0;
        combat::HitMeta meta;
        meta.source_type = content::kSourceTypeAbility;
        for (int tries = 0; tries < 200; ++tries) {
            combat::Resolved r;
            std::string why;
            if (!world.resolver().resolve(from, to, *hit, meta, r, why) || r.flags != 0) continue;
            for (const combat::ResolvedMod& m : r.mods) {
                if (m.stat_guid == content::kGuidHealth) return m.value;
            }
        }
        return 0;
    }
};

}  // namespace dots

void check_single_bleed(TestHarness& h, dots::Bed& bed, int64_t bleed, double t0) {
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    const dots::Landing l = bed.land(wolf, "Gryphon_Rake_Hit", t0, bed.player);
    CHECK(l.ok && l.has("Status_Bleeding"), "the Rake's hit puts a bleed on the Wolf");
    CHECKF(bed.world.effect_count(wolf) >= 1 && bed.world.effect_stacks(wolf, bleed) == 1 && bed.items_named(wolf, "Status_Bleeding") == 1,
           "...one stack of it, shown as one item (%d stacks)", bed.world.effect_stacks(wolf, bleed));
    std::optional<combat::EffectItemWrite> first;
    for (const auto& [n, it] : bed.items(wolf)) {
        if (n == "Status_Bleeding") first = it;
    }
    CHECKF(first && near(first->item.duration, 6.0, 1e-4) && near(first->item.tick_time, 1.0, 1e-4) && first->item.stacks == 1,
           "...for 6 s, ticking every second (%.2f s, %.2f)",
           first ? static_cast<double>(first->item.duration) : -1.0, first ? static_cast<double>(first->item.tick_time) : -1.0);
    bed.run(t0 + 0.05, t0 + 7.0);
    const std::vector<dots::Tick> ticks = bed.ticks(wolf, "Status_Bleeding_Tick");
    CHECKF(ticks.size() == 5, "it ticks 5 times in its 6 s, at 1 s to 5 s: the tick that would fall on its last moment does not happen (%zu ticks)", ticks.size());
    double worst = 0;
    for (size_t i = 0; i < ticks.size(); ++i) worst = std::max(worst, std::fabs(ticks[i].at - (t0 + 1.0 + static_cast<double>(i))));
    CHECKF(worst <= 0.05, "...each on its second (the worst is off by %.4f s)", worst);
    bool predicted = !ticks.empty(), takes = !ticks.empty(), same = true;
    double plain_value = 0;
    for (const dots::Tick& t : ticks) {
        predicted = predicted && t.predicted && t.effect == bleed;
        takes = takes && (t.missed() || t.value < 0);
        if (!t.plain()) continue;
        if (plain_value == 0) plain_value = t.value;
        same = same && near(t.value, plain_value, std::fabs(plain_value) * 0.01);
    }
    CHECK(predicted && takes, "...every tick is the bleed's, takes Health (unless it missed) and is one the clients predicted");
    CHECKF(same && plain_value < 0, "...the ticks without a critical hit are all the same size (%.2f)", plain_value);
    CHECK(bed.world.effect_count(wolf) == 0 && bed.items(wolf).empty(), "when the 6 s are over the bleed is gone from the Wolf and its list");
}

void check_multi_caster_bleed(TestHarness& h, dots::Bed& bed, int64_t bleed, double t0) {
    const uint64_t wolf2 = bed.wolf(300);
    bed.land(wolf2, "Gryphon_Rake_Hit", t0, bed.player);
    bed.run(t0 + 0.05, t0 + 2.5);
    bed.land(wolf2, "Gryphon_Rake_Hit", t0 + 2.5, bed.player);
    CHECKF(bed.world.effect_stacks(wolf2, bleed) == 1 && bed.items_named(wolf2, "Status_Bleeding") == 1 &&
               near(bed.world.effect_remaining(wolf2, bleed, t0 + 2.5), 6.0, 1e-4),
           "the same caster's second bleed adds no stack and runs the 6 s again from then (%d stacks, %.2f s left)",
           bed.world.effect_stacks(wolf2, bleed), bed.world.effect_remaining(wolf2, bleed, t0 + 2.5));
    bed.run(t0 + 2.55, t0 + 9.5);
    const std::vector<dots::Tick> again = bed.ticks(wolf2, "Status_Bleeding_Tick");
    CHECKF(again.size() == 8 && bed.world.effect_count(wolf2) == 0, "...so it ticked 8 times, up to 8 s, and then ended at 8.5 s (%zu ticks)", again.size());

    const uint64_t wolf3 = bed.wolf(600);
    bed.land(wolf3, "Gryphon_Rake_Hit", t0, bed.player);
    bed.run(t0 + 0.05, t0 + 2.5);
    bed.land(wolf3, "Gryphon_Rake_Hit", t0 + 2.5, bed.other);
    CHECKF(bed.items_named(wolf3, "Status_Bleeding") == 2 && bed.world.effect_stacks(wolf3, bleed) == 2,
           "a second caster's bleed is a second instance, the first one left as it was (%zu items)", bed.items_named(wolf3, "Status_Bleeding"));
    bed.run(t0 + 2.55, t0 + 9.0);
    const std::vector<dots::Tick> both = bed.ticks(wolf3, "Status_Bleeding_Tick");
    size_t by_first = 0, by_second = 0;
    int overlapping = 0;
    for (const dots::Tick& t : both) {
        by_first += t.by == bed.player.actor_id() ? 1 : 0;
        by_second += t.by == bed.other.actor_id() ? 1 : 0;
        overlapping += (t.at > t0 + 3.4 && t.at < t0 + 5.6) ? 1 : 0;
    }
    CHECKF(by_first == 5 && by_second == 5, "...each caster is told of his own bleed's ticks: 5 and 5 (%zu, %zu)", by_first, by_second);
    CHECKF(overlapping == 5, "...two ticks a second where they overlap (%d ticks between 3.5 s and 5.5 s)", overlapping);

    const uint64_t wolf4 = bed.wolf(900);
    bed.land(wolf4, "Gryphon_Rake_Hit", t0, bed.player);
    bed.land(wolf4, "Gryphon_Rake_Hit", t0, bed.other);
    CHECK(bed.items_named(wolf4, "Status_Bleeding") == 2, "(two casters bleed a fourth Wolf)");
    bed.world.remove_player(&bed.other);
    CHECKF(bed.items_named(wolf4, "Status_Bleeding") == 1 && bed.world.effect_stacks(wolf4, bleed) == 1,
           "a caster who leaves takes his bleed with him and the other caster's goes on (%zu left)", bed.items_named(wolf4, "Status_Bleeding"));
    bed.world.add_player(&bed.other);
}

void test_creature_bleed(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a creature bleeds: a tick a second for 6 s, one stack per caster, two casters' bleeds tick apart\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* bleeding = f.catalog->effect_named("Status_Bleeding");
    const abilities::HitDef* tick_hit = f.catalog->hit_named("Status_Bleeding_Tick");
    CHECK(bleeding && tick_hit && f.catalog->hit_named("Gryphon_Rake_Hit"), "the bleed, its tick and a hit that applies it are in the data");
    if (!bleeding || !tick_hit) return;
    check_single_bleed(h, bed, bleeding->row.guid, 100.0);
    check_multi_caster_bleed(h, bed, bleeding->row.guid, 100.0);
}

void test_creature_hemorrhage(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a creature takes a hemorrhage: a tick every 0.5 s for 5 s, separate stacks that never refresh, and only while it bleeds\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* hemorrhage = f.catalog->effect_named("Status_Hemorrhage");
    CHECK(hemorrhage && f.catalog->hit_named("Snipe_Hemorrhage") && f.catalog->hit_named("Ranger_Snipe_V2") &&
              f.catalog->hit_named("Rogue_Lacerate") && f.catalog->hit_named("Gryphon_Rake_Hit"),
          "the hemorrhage, a hit that applies it unconditionally and the two that need a bleeding creature are in the data");
    if (!hemorrhage) return;
    const int64_t hem = hemorrhage->row.guid;
    constexpr double t0 = 100.0;

    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    const dots::Landing first = bed.land(wolf, "Snipe_Hemorrhage", t0, bed.player);
    CHECK(first.ok && first.has("Status_Hemorrhage"), "a hemorrhage lands");
    bed.run(t0 + 0.05, t0 + 1.0);
    bed.land(wolf, "Snipe_Hemorrhage", t0 + 1.0, bed.player);
    CHECKF(bed.world.effect_count(wolf) == 2 && bed.items_named(wolf, "Status_Hemorrhage") == 2,
           "a second one a second later is a second instance: each application is a stack of its own (%zu items)", bed.items_named(wolf, "Status_Hemorrhage"));
    const auto items = bed.items(wolf);
    bool durations = items.size() == 2;
    for (const auto& [n, it] : items) durations = durations && near(it.item.duration, 5.0, 1e-4) && near(it.item.tick_time, 0.5, 1e-4) && it.item.stacks == 1;
    CHECK(durations, "...each lasts 5 s and ticks every half second, the first one not lengthened by the second");
    bed.run(t0 + 1.05, t0 + 7.0);
    const std::vector<dots::Tick> ticks = bed.ticks(wolf, "Status_Hemorrhage_Tick");
    CHECKF(ticks.size() == 20, "10 ticks of each, 20 in all: the first at the moment it lands, the last half a second before its end (%zu)", ticks.size());
    size_t at_first = 0, at_second = 0;
    bool predicted = true;
    for (const dots::Tick& t : ticks) {
        at_first += (t.at >= t0 - 0.01 && t.at <= t0 + 4.51) ? 1 : 0;
        at_second += (t.at >= t0 + 0.99 && t.at <= t0 + 5.51) ? 1 : 0;
        predicted = predicted && t.predicted && t.effect == hem && (t.missed() || t.value < 0);
    }
    CHECKF(at_first >= 10 && at_second >= 10 && ticks.front().at < t0 + 0.06 && ticks.back().at > t0 + 5.4 && ticks.back().at < t0 + 5.6,
           "...ticking from 0 s to 4.5 s and from 1 s to 5.5 s (first %.3f s, last %.3f s)",
           ticks.empty() ? 0.0 : ticks.front().at - t0, ticks.empty() ? 0.0 : ticks.back().at - t0);
    CHECK(predicted, "...each takes Health (unless it missed) and is a tick both clients predicted");
    CHECK(bed.world.effect_count(wolf) == 0, "both are gone after their 5 s");

    const uint64_t clean = bed.wolf(300);
    CHECK(!bed.land(clean, "Ranger_Snipe_V2", t0, bed.player).has("Status_Hemorrhage"),
          "a Snipe on a creature that does not bleed puts no hemorrhage on it (the stack count works out to 0)");
    CHECK(!bed.land(clean, "Rogue_Lacerate", t0, bed.player).has("Status_Hemorrhage"), "...nor does a Lacerate (the row requires a bleeding target)");
    CHECK(bed.land(clean, "Gryphon_Rake_Hit", t0 + 0.1, bed.player).has("Status_Bleeding") &&
              bed.world.effect_stacks(clean, bed.effect_guid("Status_Bleeding")) == 1,
          "a bleed lands on it");
    CHECK(bed.land(clean, "Ranger_Snipe_V2", t0 + 0.2, bed.player).has("Status_Hemorrhage"), "now a Snipe puts a hemorrhage on it");
    const uint64_t bleeding_wolf = bed.wolf(600);
    bed.land(bleeding_wolf, "Gryphon_Rake_Hit", t0, bed.player);
    CHECK(bed.land(bleeding_wolf, "Rogue_Lacerate", t0 + 0.1, bed.player).has("Status_Hemorrhage"),
          "...and so does a Lacerate on a bleeding creature (the tag Debuff.Bleeding is on it)");
}

double check_single_poison_stack(TestHarness& h, dots::Bed& bed, int64_t pg, double t0) {
    const uint64_t wolf1 = bed.wolf(0);
    CHECK(wolf1 != 0, "a Wolf 30 m away");
    if (!wolf1) return 0.0;
    CHECK(bed.land(wolf1, "Scorpion_TailSting_Hit", t0, bed.player).has("Status_Poisoned"), "a sting poisons the Wolf");
    const auto one = bed.items(wolf1);
    CHECKF(one.size() == 1 && near(one[0].second.item.duration, 20.0, 1e-4) && near(one[0].second.item.tick_time, 2.0, 1e-4) &&
               one[0].second.item.stacks == 1,
           "...for 20 s, a tick every 2 s, one stack");
    bed.run(t0 + 0.05, t0 + 21.0);
    const std::vector<dots::Tick> t1 = bed.ticks(wolf1, "Status_Poisoned_Tick");
    CHECKF(t1.size() == 9, "9 ticks at 2 s, 4 s ... 18 s: the one that would fall on its last moment does not happen (%zu)", t1.size());
    double v1 = 0;
    bool predicted = !t1.empty();
    for (const dots::Tick& t : t1) {
        if (t.plain() && v1 == 0) v1 = t.value;
        predicted = predicted && t.predicted && t.effect == pg && (t.missed() || t.value < 0);
    }
    CHECKF(predicted && v1 < 0, "...each takes Health (unless it missed) and is predicted by both clients (a tick of one stack: %.4f)", v1);
    return v1;
}

void check_poison_stacking_and_chill(TestHarness& h, dots::Bed& bed, int64_t pg, double t0, double v1) {
    const uint64_t wolf3 = bed.wolf(300);
    for (int i = 0; i < 3; ++i) bed.land(wolf3, "Scorpion_TailSting_Hit", t0 + 0.1 * i, bed.player);
    CHECKF(bed.world.effect_stacks(wolf3, pg) == 3 && bed.items_named(wolf3, "Status_Poisoned") == 1,
           "three stings are one poison of three stacks (%d)", bed.world.effect_stacks(wolf3, pg));
    bed.run(t0 + 0.25, t0 + 12.1);
    const std::vector<dots::Tick> t3 = bed.ticks(wolf3, "Status_Poisoned_Tick");
    double v3 = 0;
    for (const dots::Tick& t : t3) {
        if (t.plain() && v3 == 0) v3 = t.value;
    }
    CHECKF(v1 < 0 && v3 < 0 && near(v3 / v1, 3.0, 0.05), "...and each of its ticks is three times a tick of one stack: %.4f against %.4f", v3, v1);

    const uint64_t wolf12 = bed.wolf(600);
    for (int i = 0; i < 12; ++i) bed.land(wolf12, "Scorpion_TailSting_Hit", t0 + 0.05 * i, bed.player);
    CHECKF(bed.world.effect_stacks(wolf12, pg) == 10, "twelve stings stack to ten and no more (%d)", bed.world.effect_stacks(wolf12, pg));
    bed.run(t0 + 0.7, t0 + 12.1);
    const std::vector<dots::Tick> t10 = bed.ticks(wolf12, "Status_Poisoned_Tick");
    double v10 = 0;
    for (const dots::Tick& t : t10) {
        if (t.plain() && v10 == 0) v10 = t.value;
    }
    CHECKF(v10 < 0 && near(v10 / v1, 10.0, 0.15), "...and a tick of ten stacks is ten times a tick of one: %.4f against %.4f", v10, v1);

    const uint64_t wolf2 = bed.wolf(900);
    bed.land(wolf2, "Scorpion_TailSting_Hit", t0, bed.player);
    bed.land(wolf2, "Scorpion_TailSting_Hit", t0 + 0.1, bed.other);
    CHECKF(bed.items_named(wolf2, "Status_Poisoned") == 2, "a second caster's poison is a poison of its own (%zu items)", bed.items_named(wolf2, "Status_Poisoned"));

    const uint64_t wolf5 = bed.wolf(1200);
    bed.land(wolf5, "Rogue_Poison_Chilling", t0, bed.player);
    CHECKF(bed.items_named(wolf5, "Status_Poisoned") == 1 && bed.items_named(wolf5, "Status_Chilled") == 0 &&
               bed.world.effect_stacks(wolf5, bed.effect_guid("Status_Chilled")) == 0,
           "the Chilling poison puts its poison on the Wolf and leaves its Chilled off (%zu poisons, %zu chills)",
           bed.items_named(wolf5, "Status_Poisoned"), bed.items_named(wolf5, "Status_Chilled"));
}

void test_creature_poison(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a creature is poisoned: a tick every 2 s for 20 s, stacks in place up to ten that grow the tick, another caster's poison apart\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* poison = f.catalog->effect_named("Status_Poisoned");
    CHECK(poison && f.catalog->hit_named("Status_Poisoned_Tick") && f.catalog->hit_named("Scorpion_TailSting_Hit"),
          "the poison, its tick and a hit that applies it are in the data");
    if (!poison) return;
    const double v1 = check_single_poison_stack(h, bed, poison->row.guid, 100.0);
    if (v1 != 0.0) check_poison_stacking_and_chill(h, bed, poison->row.guid, 100.0, v1);
}

void check_debuff_damage_and_accuracy(TestHarness& h, dots::Bed& bed, const abilities::EffectDef& demoralized, const abilities::EffectDef& blinded, double t0) {
    const uint64_t wolf = bed.wolf(0);
    CHECK(wolf != 0, "a Wolf 30 m away");
    if (!wolf) return;
    combat::Fighter* wolf_fighter = bed.world.fighter_of(wolf);
    combat::Fighter& mage = bed.player.fighter();
    const double physical_before = bed.plain_damage(mage, *wolf_fighter, "Ranger_Snipe_V2");
    const double magical_before = bed.plain_damage(mage, *wolf_fighter, "Mage_BallLightning");
    const double dealt_before = bed.plain_damage(*wolf_fighter, mage, "Ranger_Snipe_V2");
    const int64_t accuracy = blinded.mods[0].stat_guid, damage_mult = demoralized.mods[0].stat_guid;
    const double accuracy_before = bed.stat_of(wolf, accuracy), mult_before = bed.stat_of(wolf, damage_mult);
    CHECKF(physical_before < 0 && magical_before < 0 && dealt_before < 0,
           "(a Snipe takes %.1f from the Wolf, a Ball Lightning %.1f, and the Wolf deals %.1f with the Snipe's formula)",
           physical_before, magical_before, dealt_before);

    const dots::Landing s = bed.land(wolf, "Beetle_Subsonics_Hit", t0, bed.player);
    CHECK(s.has("Status_Shaken") && s.has("Status_Blinded"), "a hit puts Shaken and Blinded on the Wolf");
    const double physical_shaken = bed.plain_damage(mage, *wolf_fighter, "Ranger_Snipe_V2");
    const double magical_shaken = bed.plain_damage(mage, *wolf_fighter, "Mage_BallLightning");
    CHECKF(physical_shaken < physical_before * 1.01 && physical_shaken < 0, "Shaken: the Wolf takes more physical damage (%.1f, it was %.1f)", physical_shaken, physical_before);
    CHECKF(near(magical_shaken, magical_before, std::fabs(magical_before) * 1e-6), "...and no more magical damage (%.1f, it was %.1f)", magical_shaken, magical_before);
    CHECKF(bed.stat_of(wolf, accuracy) < accuracy_before - 1e-9, "Blinded: its accuracy is lower (%.3f, it was %.3f)", bed.stat_of(wolf, accuracy), accuracy_before);
    const dots::Landing v = bed.land(wolf, "Mage_BallLightning", t0 + 0.1, bed.player);
    CHECK(v.has("Status_Volatile"), "a Ball Lightning puts Volatile on it");
    const double magical_volatile = bed.plain_damage(mage, *wolf_fighter, "Mage_BallLightning");
    CHECKF(magical_volatile < magical_before * 1.01 && magical_volatile < 0, "Volatile: the Wolf takes more magical damage (%.1f, it was %.1f)", magical_volatile, magical_before);
    const dots::Landing d = bed.land(wolf, "Fighter_BattleCry_Enemy", t0 + 0.2, bed.player);
    CHECK(d.has("Status_Demoralized"), "a Battle Cry puts Demoralized on it");
    const double dealt_demoralized = bed.plain_damage(*wolf_fighter, mage, "Ranger_Snipe_V2");
    CHECKF(dealt_demoralized > dealt_before - 1e-9 && dealt_demoralized < 0 && !near(bed.stat_of(wolf, damage_mult), mult_before, 1e-12),
           "Demoralized: the Wolf deals less damage (%.1f, it was %.1f; its damage multiplier %.3f, it was %.3f)",
           dealt_demoralized, dealt_before, bed.stat_of(wolf, damage_mult), mult_before);

    std::printf("    (Shaken: a physical hit x%.3f, Volatile: a magical hit x%.3f, Demoralized: the Wolf's hits x%.3f, its damage multiplier %.3f -> %.3f, Blinded: accuracy %.3f -> %.3f)\n",
                physical_before != 0 ? physical_shaken / physical_before : 0.0,
                magical_before != 0 ? magical_volatile / magical_before : 0.0,
                dealt_before != 0 ? dealt_demoralized / dealt_before : 0.0,
                mult_before, bed.stat_of(wolf, damage_mult), accuracy_before, bed.stat_of(wolf, accuracy));

    bed.run(t0 + 0.25, t0 + 40.0);
    CHECK(bed.world.effect_count(wolf) == 0, "after their time nothing is left on the Wolf");
    const double physical_after = bed.plain_damage(mage, *wolf_fighter, "Ranger_Snipe_V2");
    const double dealt_after = bed.plain_damage(*wolf_fighter, mage, "Ranger_Snipe_V2");
    CHECKF(near(physical_after, physical_before, std::fabs(physical_before) * 1e-6) &&
               near(dealt_after, dealt_before, std::fabs(dealt_before) * 1e-6) && near(bed.stat_of(wolf, accuracy), accuracy_before, 1e-9),
           "...and the damage it takes, the damage it deals and its accuracy are what they were (%.1f, %.1f)", physical_after, dealt_after);
}

void check_shaken_extension_and_bard_melodies(TestHarness& h, dots::Bed& bed, int64_t sg, double t0) {
    const uint64_t wolf2 = bed.wolf(300);
    bed.land(wolf2, "Beetle_Subsonics_Hit", t0, bed.player);
    const uint64_t wolf3 = bed.wolf(600);
    const dots::Landing a = bed.land(wolf3, "Weapon_Proc_Concussion_2", t0, bed.player);
    const effects::Applied* sa = a.find("Status_Shaken");
    CHECK(sa != nullptr, "a Concussion hit puts Shaken on the Wolf");
    if (!sa) return;
    CHECKF(sa->duration > 5.99 && sa->extension >= 3.0 && near(sa->extended_max, 15.0, 1e-9),
           "...for at least the record's 6 s (%.2f s), with an extension of at least 3 s (%.2f) and no more than 15 s ever (%.1f)",
           sa->duration, sa->extension, sa->extended_max);
    const double first = bed.world.effect_remaining(wolf3, sg, t0);
    bed.land(wolf3, "Weapon_Proc_Concussion_2", t0 + 1.0, bed.player);
    const double second = bed.world.effect_remaining(wolf3, sg, t0 + 1.0);
    CHECKF(near(second, (first - 1.0) + 3.0, 0.6) || second > first - 1.0,
           "a second application a second later adds to what is left (%.2f s left of %.2f s)", second, first);
    for (int i = 0; i < 12; ++i) bed.land(wolf3, "Weapon_Proc_Concussion_2", t0 + 1.0 + 0.01 * i, bed.player);
    const double most = bed.world.effect_remaining(wolf3, sg, t0 + 1.2);
    CHECKF(most <= 15.0 + 1e-6 && most >= 14.0, "...and however often it is applied, never more than 15 s are left (%.2f)", most);
    CHECKF(bed.world.effect_stacks(wolf3, sg) == 1, "...and Shaken stays one stack (%d)", bed.world.effect_stacks(wolf3, sg));

    const uint64_t wolf4 = bed.wolf(900);
    const dots::Landing menacing = bed.land(wolf4, "Bard_ResonantWeapon_End_Menacing", t0, bed.player);
    const dots::Landing cathartic = bed.land(wolf4, "Bard_ResonantWeapon_End_Cathartic", t0, bed.player);
    CHECK(menacing.has("Status_Shocked") && cathartic.has("Status_Wounded") && bed.items_named(wolf4, "Status_Shocked") == 1 &&
              bed.items_named(wolf4, "Status_Wounded") == 1,
          "the Bard's Menacing melody puts Shocked on the Wolf and the Cathartic one Wounded");
}

void test_creature_stat_debuffs(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the stat debuffs: Shaken and Volatile raise the damage a creature takes, Demoralized lowers what it deals, Blinded its accuracy; the extension of Shaken\n");
    dots::Bed bed(f);
    CHECKF(bed.ok(), "the bed: %s", bed.problem().c_str());
    if (!bed.ok()) return;
    const abilities::EffectDef* shaken = f.catalog->effect_named("Status_Shaken");
    const abilities::EffectDef* volatile_ = f.catalog->effect_named("Status_Volatile");
    const abilities::EffectDef* demoralized = f.catalog->effect_named("Status_Demoralized");
    const abilities::EffectDef* blinded = f.catalog->effect_named("Status_Blinded");
    CHECK(shaken && volatile_ && demoralized && blinded && !shaken->mods.empty() && !volatile_->mods.empty() &&
              !demoralized->mods.empty() && !blinded->mods.empty(),
          "the four debuffs and their stat mods are in the data");
    if (!shaken || !volatile_ || !demoralized || !blinded) return;
    check_debuff_damage_and_accuracy(h, bed, *demoralized, *blinded, 100.0);
    check_shaken_extension_and_bard_melodies(h, bed, shaken->row.guid, 100.0);
}

void test_effect_host_functions(TestHarness& h) {
    std::printf("EffectHasTag, GetEffect() and CalcEffectDuration are answered for the effect whose numbers are worked out\n");
    const std::vector<std::string> tags = {"Element.Physical.Bleed", "Debuff.Bleeding"};
    const std::map<int64_t, double> durations = {{111, 6.0}, {222, 0.0}};
    combat::HitContext ctx;
    double out = -1;
    CHECK(!ctx.host_fn(stats::kHostEffectHasTag, "Debuff.Bleeding", nullptr, 0, out) &&
              !ctx.host_fn(stats::kHostEffectIs, "5", nullptr, 0, out) &&
              !ctx.host_fn(stats::kHostEffectDuration, "111", nullptr, 0, out),
          "with no effect in progress none of the three is answered: the cast's own host does");
    ctx.effect_guid = 5;
    ctx.effect_tags = &tags;
    ctx.effect_durations = &durations;
    const auto ask = [&](int fn, const char* name) {
        out = -1;
        return ctx.host_fn(fn, name, nullptr, 0, out) ? out : -999.0;
    };
    CHECK(ask(stats::kHostEffectHasTag, "Debuff.Bleeding") == 1.0 && ask(stats::kHostEffectHasTag, "Element.Physical") == 1.0 &&
              ask(stats::kHostEffectHasTag, "Debuff") == 1.0,
          "EffectHasTag: the effect has its own tags and the ones above them (Element.Physical.Bleed is Element.Physical)");
    CHECK(ask(stats::kHostEffectHasTag, "Element.Magical") == 0.0 && ask(stats::kHostEffectHasTag, "Debuff.Snared") == 0.0,
          "...and not the tags it does not have");
    CHECK(ask(stats::kHostEffectIs, "5") == 1.0 && ask(stats::kHostEffectIs, "6") == 0.0,
          "GetEffect() == <effect> is true for the effect in progress and for no other");
    CHECK(ask(stats::kHostEffectDuration, "111") == 6.0 && ask(stats::kHostEffectDuration, "222") == 0.0 &&
              ask(stats::kHostEffectDuration, "333") == 0.0,
          "CalcEffectDuration reads the length worked out for the effect named (0 for one that was not)");
}

struct Forge {
    Rig                    rig;
    combat_rig::World      w;
    combat::NpcStatBook    book;
    combat::CombatWorld    world;
    combat_rig::FakePlayer player;
    combat::WorldHitPort   port;
    std::string            wolf_path;

    Forge(Fixture& f, int class_enum, const std::vector<std::string>& skills, uint64_t seed = 7)
        : rig(class_enum, 25, skills, true, seed),
          book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion),
          player(rig, 77, {0, 0, 1098}),
          port(world, w.registry, player.fighter()),
          wolf_path(creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C")) {
        if (!rig.ok()) return;
        world.add_player(&player);
        rig.pipe->set_combat(&port);
    }
    bool ok() const { return rig.ok() && w.problem.empty(); }
    uint64_t wolf(double x, double y) { return w.spawn(wolf_path, x, y, 25, 100); }
    double lost(const std::vector<uint64_t>& wolves) const {
        double sum = 0;
        for (const uint64_t id : wolves) sum += world.max_health_of(id) - world.health_of(id);
        return sum;
    }
};

size_t forged_zone_ops(const Rig& rig, bootstrap::ZoneOp::Kind kind) {
    size_t n = 0;
    for (const Seen& s : rig.log) {
        for (const bootstrap::ZoneOp& z : s.zones) n += z.kind == kind ? 1 : 0;
    }
    return n;
}

std::vector<const bootstrap::HitConfirm*> forged_confirmations(const Rig& rig, int64_t hit_guid) {
    std::vector<const bootstrap::HitConfirm*> out;
    for (const Seen& s : rig.log) {
        for (const bootstrap::HitConfirm& c : s.confirmations) {
            if (c.record_guid == hit_guid && !c.targets.empty()) out.push_back(&c);
        }
    }
    return out;
}

struct ScatterAsk {
    int64_t charged = 0;
    double  request = 0;
    bool    drain = false;
    double  early = 0;
    double  late = 0;
    bool    tap = false;
};

struct ScatterResult {
    bool   ok = false, accepted = false, unpaid = false;
    double mana_before = 0, mana_early = 0, mana_late = 0, lost_early = 0, lost_late = 0;
};

ScatterResult cast_forged_scatter(TestHarness& h, const ScatterAsk& ask) {
    constexpr double t0 = 1.0;
    ScatterResult out;
    Forge g(h.f, 1, {"Ranger_Passive_ScatterShot_Concentrated"});
    CHECKF(g.ok(), "the world for the Scatter Shot (%s%s)", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return out;
    const std::vector<uint64_t> wolves = {g.wolf(150, 0), g.wolf(400, 0), g.wolf(700, 0), g.wolf(1000, 0)};
    g.rig.know({"Ranger_ScatterShot"});
    const int32_t id = g.rig.next_client_id;
    const Refusal refusal = g.rig.trigger("Ranger_ScatterShot", t0, false);
    out.accepted = refusal == Refusal::None;
    if (!out.accepted) std::printf("    (the Scatter Shot was refused: %s)\n", refusal_name(refusal));
    out.mana_before = g.rig.mana();
    g.rig.run_to(t0 + ask.request);
    if (ask.tap) {
        g.rig.pipe->cancel(id, t0 + ask.request, 0.2);
    } else {
        g.rig.pipe->branch_request(id, ask.charged, t0 + ask.request);
    }
    if (ask.drain) g.rig.set_current(content::kGuidMana, 0.0);
    g.rig.run_to(t0 + ask.early);
    out.lost_early = g.lost(wolves);
    out.mana_early = g.rig.mana();
    g.rig.run_to(t0 + ask.late);
    out.lost_late = g.lost(wolves);
    out.mana_late = g.rig.mana();
    for (const Seen& s : g.rig.log) {
        out.unpaid = out.unpaid || (s.refusal && s.note.find("cannot be paid for any more") != std::string::npos);
    }
    out.ok = g.rig.pawn.broken.empty();
    return out;
}

void test_forged_scatter_shot(TestHarness& h) {
    std::printf("a held Scatter Shot: the charged shot waits for a charge to fill, and a cast that cannot pay fires nothing\n");
    const int64_t charged = ability_rig::record_guid(real_db_path(), "AnimBranchRecordBase", "Ranger_Passive_ScatterShot_Concentrated");
    CHECK(charged != 0, "the animation branch of the talent's charged shot is in the data");
    if (charged == 0) return;

    const ScatterResult soon = cast_forged_scatter(h, {charged, 0.3, false, 0.9, 1.8, false});
    CHECK(soon.accepted && soon.ok, "(the Scatter Shot is cast, and the pawn takes every change)");
    CHECKF(soon.lost_early == 0.0 && soon.mana_early == soon.mana_before,
           "a request for the charged shot 0.3 s after the trigger fires nothing at 0.9 s: no Wolf is hurt and no Mana is paid (%.1f Health lost, Mana %.2f of %.2f)",
           soon.lost_early, soon.mana_early, soon.mana_before);
    CHECKF(soon.lost_late > 0.0 && soon.mana_late < soon.mana_before,
           "...the shot goes out when the charge could be full (1.0 s plus the montage's jump) and is paid for: %.1f Health lost, Mana %.2f of %.2f",
           soon.lost_late, soon.mana_late, soon.mana_before);

    const ScatterResult broke = cast_forged_scatter(h, {charged, 1.2, true, 1.5, 2.0, false});
    CHECK(broke.accepted && broke.ok, "(a second Scatter Shot is cast)");
    CHECKF(broke.unpaid, "a charged shot that cannot be paid for ends the cast (\"cannot be paid for any more\")");
    CHECKF(broke.lost_early == 0.0 && broke.lost_late == 0.0,
           "...and fires nothing: no Wolf is hurt (%.1f at 1.5 s, %.1f at 2.0 s)", broke.lost_early, broke.lost_late);

    const ScatterResult tapped = cast_forged_scatter(h, {charged, 0.5, false, 1.2, 1.8, true});
    CHECK(tapped.accepted && tapped.ok, "(a third Scatter Shot is cast, its key let go at 0.5 s)");
    CHECKF(tapped.lost_late > 0.0 && tapped.mana_late < tapped.mana_before,
           "a tap sends the scatter and pays for it: %.1f Health lost, Mana %.2f of %.2f",
           tapped.lost_late, tapped.mana_late, tapped.mana_before);

    const ScatterResult tap_broke = cast_forged_scatter(h, {charged, 0.5, true, 1.2, 1.8, true});
    CHECK(tap_broke.accepted && tap_broke.ok && tap_broke.unpaid,
          "(a fourth Scatter Shot is cast, its key let go at 0.5 s with no Mana left: it ends at its cost step)");
    CHECKF(tap_broke.lost_early == 0.0 && tap_broke.lost_late == 0.0,
           "a tap that cannot pay sends nothing: no Wolf is hurt (%.1f at 1.2 s, %.1f at 1.8 s)",
           tap_broke.lost_early, tap_broke.lost_late);
}

struct ForgedOwnZones {
    abilities::AbilityDef* def = nullptr;
    std::vector<int64_t>   before;
    ForgedOwnZones(const abilities::AbilityDef* ability, std::vector<int64_t> zones) : def(const_cast<abilities::AbilityDef*>(ability)) {
        if (!def) return;
        before = def->zones;
        def->zones = std::move(zones);
    }
    ~ForgedOwnZones() {
        if (def) def->zones = before;
    }
};

void test_forged_cast_bar_zone(TestHarness& h) {
    std::printf("a cast bar the client ends takes its zone with it\n");
    Rig rig(8, 25, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    rig.know({"Mage_MagmaField"});
    const ForgedOwnZones zones(rig.catalog->ability_named("Mage_MagmaField"), {6064631404175951226ll, 6064631522804306548ll});
    constexpr double t0 = 1.0;
    const Refusal refusal = rig.trigger("Mage_MagmaField", t0, false);
    CHECKF(refusal == Refusal::None, "Magma Field is cast (%s)", refusal_name(refusal));
    rig.run_to(t0 + 0.5);
    int64_t instance = -1;
    for (const auto& [key, item] : rig.pawn.abilities) {
        if (item.ability_guid == rig.ability("Mage_MagmaField")) instance = key;
    }
    CHECKF(instance >= 0 && forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Open) == 2,
           "the cast bar is under way: its item is on the pawn and its two zones have opened (%zu opened)",
           forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Open));
    if (instance < 0) return;
    CHECK(rig.pipe->end_request(static_cast<int32_t>(instance), t0 + 0.6), "the client ends the instance before its bar is over");
    rig.run_to(t0 + 3.0);
    CHECKF(forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Fade) == 2 && forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Close) == 2,
           "both zones fade out and close with the cancelled cast, 2.4 s on (%zu faded, %zu closed)",
           forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Fade), forged_zone_ops(rig, bootstrap::ZoneOp::Kind::Close));
    CHECK(rig.pawn.broken.empty(), "(the pawn took every change)");
}

struct ForgedStrike {
    bool    ok = false;
    double  value = 0;
    uint8_t flags = 0;
};

int64_t find_hit_targeting(const abilities::AbilityDef& def, int64_t hit_guid) {
    int64_t targeting = content::kTargetingDefaultSelf;
    for (const content::AbilityHitRow& hr : def.hits) {
        if (hr.hit_guid != hit_guid || hr.source != "hit") continue;
        for (const content::TargetingRow& tg : def.targetings) {
            if (tg.key == hr.key) targeting = tg.guid;
        }
    }
    return targeting;
}

ForgedStrike forged_strike(Fixture& f, uint64_t seed, int how) {
    ForgedStrike out;
    Forge g(f, 8, {}, seed);
    if (!g.ok()) return out;
    const uint64_t wolf = g.wolf(600, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Mage_LightningStrike");
    const abilities::HitDef* hit = f.catalog->hit_named("Mage_LightningStrike_Hit");
    if (!wolf || !def || !hit) return out;
    g.rig.know({"Mage_LightningStrike"});
    const int64_t targeting = find_hit_targeting(*def, hit->row.guid);
    constexpr int64_t kCastComplete = 6064628894971461633ll;
    constexpr double t0 = 1.0;
    constexpr int32_t id = 41;
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    if (g.rig.pipe->trigger(def->row.guid, id, t0, info) != Refusal::None) return out;
    double ask = t0 + 1.2;
    if (how == 0) {
        g.rig.run_to(t0 + 2.416);
        g.rig.pipe->branch_request(id, kCastComplete, t0 + 2.416);
        ask = t0 + 3.2;
    } else if (how == 1) {
        g.rig.run_to(t0 + 0.171);
        g.rig.pipe->branch_request(id, kCastComplete, t0 + 0.171);
    } else {
        g.rig.run_to(t0 + 0.3);
        g.rig.pipe->cancel(id, t0 + 0.3, 1.0e6);
    }
    g.rig.run_to(ask);
    g.rig.pipe->hit_request(def->row.guid, targeting, hit->row.guid, 1, 0, ask, {wolf});
    g.rig.run_to(ask + 1.0);
    for (const bootstrap::HitConfirm* c : forged_confirmations(g.rig, hit->row.guid)) {
        if (c->targets[0].mods.empty()) continue;
        out.ok = true;
        out.value = c->targets[0].mods[0].value;
        out.flags = c->targets[0].flags;
    }
    return out;
}

void test_forged_held_fill(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a held ability's fill: a tap asks like a full bar, a release claims a hold longer than the cast has run\n");
    bool done = false;
    for (uint64_t seed = 1; seed <= 12 && !done; ++seed) {
        const ForgedStrike full = forged_strike(f, seed, 0);
        const ForgedStrike tap = forged_strike(f, seed, 1);
        const ForgedStrike forged = forged_strike(f, seed, 2);
        const bool all_plain = full.ok && tap.ok && forged.ok && full.flags == 0 && tap.flags == 0 && forged.flags == 0 && full.value != 0.0;
        if (!all_plain) continue;
        done = true;
        const double tap_share = tap.value / full.value, forged_share = forged.value / full.value;
        CHECK(full.value < 0.0, "a full bar hits the Wolf for damage");
        CHECKF(tap_share > 0.25 && tap_share < 0.42,
               "a tap that asked 171 ms after the trigger does a third of a full bar's damage (100 %% against 300 %%): %.3f of it", tap_share);
        CHECKF(forged_share > 0.30 && forged_share < 0.55,
               "a release 0.3 s after the trigger that claims a hold of a million seconds is cut to the 0.3 s it ran (about 127 %% against 300 %%): %.3f of a full bar",
               forged_share);
    }
    CHECK(done, "a seed where the full bar, the tap and the forged release are all plain hits (no critical hit, block or miss)");
}

void test_forged_launch_request(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a launch asked for long before the montage gets there is not believed\n");
    Forge g(f, 1, {});
    CHECKF(g.ok(), "the world for the Snipe (%s%s)", g.rig.problem.c_str(), g.w.problem.c_str());
    if (!g.ok()) return;
    const uint64_t wolf = g.wolf(1200, 0);
    const abilities::AbilityDef* def = f.catalog->ability_named("Ranger_Snipe");
    CHECK(wolf != 0 && def != nullptr, "a Wolf 12 m ahead and the Snipe");
    if (!wolf || !def) return;
    g.rig.know({"Ranger_Snipe"});
    constexpr double t0 = 1.0;
    TriggerInfo info;
    info.target_count = 1;
    info.target_actors = {wolf};
    const Refusal refusal = g.rig.pipe->trigger(def->row.guid, 50, t0, info);
    CHECKF(refusal == Refusal::None, "the Snipe is cast at the Wolf (%s)", refusal_name(refusal));
    g.rig.take();
    g.rig.hit_request("Ranger_Snipe", "Ranger_Snipe_Hasting", t0 + 0.3);
    g.rig.run_to(t0 + 2.0);
    const std::vector<uint64_t> wolves = {wolf};
    CHECKF(g.lost(wolves) == 0.0, "the arrow has not left 2.0 s after the trigger, however early the request came (%.1f Health lost)", g.lost(wolves));
    g.rig.run_to(t0 + 6.0);
    CHECKF(g.lost(wolves) > 0.0, "...and it lands when the montage launches it (%.1f Health lost at 6.0 s)", g.lost(wolves));
    CHECK(g.rig.pawn.broken.empty(), "(the pawn took every change)");
}

void test_forged_slicing_requests(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a hit that fires a projectile fires it once a cast, and never after the caster died\n");
    const abilities::HitDef* slicing = f.catalog->hit_named("Fighter_Maim_Slicing");
    CHECK(slicing != nullptr, "the Maim's projectile hit is in the data");
    if (!slicing) return;
    for (const bool dies : {false, true}) {
        Forge g(f, 4, {"Fighter_Passive_Maim_Slicing"});
        CHECKF(g.ok(), "the world for the Maim (%s%s)", g.rig.problem.c_str(), g.w.problem.c_str());
        if (!g.ok()) return;
        const uint64_t wolf = g.wolf(800, 0);
        g.rig.know({"Fighter_Maim"});
        constexpr double t0 = 1.0;
        g.rig.asks = false;
        const Refusal refusal = wolf != 0 ? g.rig.trigger("Fighter_Maim", t0, false) : Refusal::Busy;
        CHECKF(wolf != 0 && refusal == Refusal::None, "the Maim is cast with a Wolf in the line ahead (%s)", refusal_name(refusal));
        const int asks = dies ? 1 : 6;
        for (int i = 0; i < asks; ++i) g.rig.hit_request("Fighter_Maim", "Fighter_Maim_Slicing_Fire", t0 + 0.6 + 0.2 * i);
        if (dies) g.rig.pipe->set_dead(true);
        g.rig.run_to(t0 + 4.0);
        const size_t waves = forged_confirmations(g.rig, slicing->row.guid).size();
        if (dies) {
            CHECKF(waves == 0, "a request accepted just before the death fires nothing after it (%zu hits of the wave)", waves);
        } else {
            CHECKF(waves == 1, "six requests for the Slicing hit throw one wave: one hit on the Wolf (%zu)", waves);
        }
    }
}

}  // namespace

void run_combat_dots_tests(TestHarness& h) {
    test_creature_bleed(h);
    test_creature_hemorrhage(h);
    test_creature_poison(h);
    test_creature_stat_debuffs(h);
    test_effect_host_functions(h);
    test_forged_scatter_shot(h);
    test_forged_cast_bar_zone(h);
    test_forged_held_fill(h);
    test_forged_launch_request(h);
    test_forged_slicing_requests(h);
}

void run_combat_dots_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db) {
    (void)db;
    run_combat_dots_tests(h);
}

}  // namespace sandal::worldd::tests
