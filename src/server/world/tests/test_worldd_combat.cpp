// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat_test_fixture.h"

#include <bit>

namespace sandal::worldd::tests {
void run_combat_burn_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db);
void run_combat_dots_tests(TestHarness& h, const sandal::worldd::content::ContentDb& db);
}  // namespace sandal::worldd::tests

using namespace sandal::worldd::tests;

namespace {

void test_data(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the combat data\n");
    const combat::CombatData& d = *f.data;
    CHECK(d.profile().elements.size() == 11, "the damage profile has the eleven elements (four physical, seven magical)");
    CHECK(d.profile().physical.damage != 0 && d.profile().magical.damage != 0 && d.profile().physical.damage != d.profile().magical.damage, "the two aspects have their own Power stats");
    CHECK(d.profile().element("Element.Magical.Fire") && d.profile().element("Element.Physical.Slash") && !d.profile().element("Element.Healing"), "elements are found by tag");
    CHECK(near(d.in_combat_timer(), 6.0, 1e-9) && near(d.global_damage_multiplier(), 1.0, 1e-9) && near(d.activation_leeway_cm(), 1000.0, 1e-9),
          "DefaultCombat.ini's numbers: in combat for 6 s, damage multiplier 1, cast leeway 1,000 cm");
    const combat::CombatStatIds& id = d.ids();
    CHECK(id.health == content::kGuidHealth && id.max_health == content::kGuidMaxHealth && id.physical_crit_chance != 0 && id.physical_evasion != 0 && id.threat_generation != 0,
          "the stats the hit rules read are found by name in the stat program");
    const auto wolf = f.blueprint("Wolf_C");
    CHECK(wolf.has_value(), "the Blueprint table has the Wolf");
    if (wolf) {
        std::vector<int64_t> al(wolf->alignments.begin(), wolf->alignments.end());
        CHECK(d.relation(al, combat::CombatData::player_alignments()) == combat::Relation::Hostile, "a Wolf is hostile to a player");
    }
    const auto quest = f.blueprint("Quest_NPC_Master_C");
    if (quest) {
        std::vector<int64_t> al(quest->alignments.begin(), quest->alignments.end());
        CHECK(d.relation(al, combat::CombatData::player_alignments()) != combat::Relation::Hostile, "a quest master is not hostile to a player");
    }
}

struct Golden {
    const char* blueprint;
    int         level;
    double      max_health;
};

constexpr Golden kGolden[] = {
    {"Wolf_C", 5, 320},          {"Wolf_C", 6, 372},          {"Wolf_C", 7, 444},          {"Wolf_C", 11, 860},         {"Wolf_C", 12, 1011},
    {"Rabid_C", 8, 675},         {"Rabid_C", 9, 793},         {"Rabid_C", 11, 1089},       {"Rabid_C", 12, 1275},
    {"Raven_C", 5, 463},         {"Raven_C", 9, 885},         {"Ram_C", 5, 845},           {"Ram_C", 9, 1683},
    {"Deer_C", 6, 764},          {"Deer_C", 10, 1549},        {"Skeleton_C", 4, 309},      {"Skeleton_C", 5, 485},
    {"Bandit_Raider_Dunir_C", 6, 719}, {"Bandit_Raider_Dunir_C", 10, 1436}, {"Bandit_Healer_Dunir_C", 6, 666}, {"Bandit_Arsonist_Dunir_C", 9, 750},
    {"Minotaur_Berserker_C", 5, 1122}, {"Minotaur_Berserker_C", 12, 3312}, {"AlphaWolf_C", 6, 1105}, {"Bear_C", 11, 5997},
    {"RockElemental_C", 9, 8434}, {"Bandit_Raider3_Dunir_C", 13, 18327}, {"Guard_Soldier_C", 50, 13539725}, {"Guard_Captain_C", 50, 22629358},
};

void test_npc_stats(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("NPC stats\n");
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    size_t within1 = 0, within5 = 0, total = 0;
    for (const Golden& g : kGolden) {
        const auto bp = f.blueprint(g.blueprint);
        CHECKF(bp.has_value(), "the Blueprint table has %s", g.blueprint);
        if (!bp) continue;
        std::string err;
        auto cs = book.make(*bp, g.level, err);
        CHECKF(cs != nullptr, "%s at level %d: %s", g.blueprint, g.level, err.c_str());
        if (!cs) continue;
        const stats::StatValue* hp = cs->find(content::kGuidMaxHealth);
        const stats::StatValue* cur = cs->find(content::kGuidHealth);
        CHECKF(hp && cur && hp->value > 0 && cur->value == hp->value, "%s at level %d has full Health", g.blueprint, g.level);
        if (!hp) continue;
        ++total;
        const double rel = std::fabs(hp->value - g.max_health) / g.max_health;
        // GetStatInitRunningTotal folds Base into the running total before the elite scalar multiplies Constitution's MaxHealth contribution.
        if (cs->npc().elite_rank > 0 || g.level > 40) ++within5;
        CHECKF(rel <= 0.01, "%s at level %d: MaxHealth %.1f is within 1 %% of retail's %.0f (%.3f)", g.blueprint, g.level, hp->value, g.max_health, hp->value / g.max_health);
        if (rel <= 0.01) ++within1;
    }
    std::printf("  %zu of %zu within 1 %% of the retail MaxHealth (%zu elites and guards among them)\n", within1, total, within5);
    CHECK(within1 == total && total == 29 && within5 >= 7, "all 29 goldens, elites and guards among them, within 1 %");
}

struct CapturedOpenInfo {
    const int64_t*           found_level = nullptr;
    int64_t                  effective_level = 20;
    int64_t                  captured_rank = -1;
    std::map<int64_t, float> cap;
    bool                     valid = false;
};

CapturedOpenInfo parse_captured_open(const spawns::OpenTemplate& o) {
    CapturedOpenInfo out;
    if (o.category != "npc") return out;
    const spawns::ParsedBlock* info = nullptr;
    const spawns::ParsedBlock* st = nullptr;
    const spawns::ParsedBlock* ci = nullptr;
    for (const spawns::ParsedBlock& b : o.blocks) {
        if (b.cls == "UNPCCombatInfo") ci = &b;
        if (b.cls == "UNPCInfo") info = &b;
        if (b.cls == "UAoCStatsComponent") st = &b;
    }
    if (!info || !info->schema) return out;
    out.valid = true;
    const repl::Value* lv = info->state.state.get(*info->schema, "Level");
    out.found_level = lv ? lv->get_if<int64_t>() : nullptr;
    if (out.found_level) out.effective_level = *out.found_level;
    if (ci && ci->schema) {
        const repl::Value* ev = ci->state.state.get(*ci->schema, "EliteRank");
        if (ev && ev->get_if<int64_t>()) out.captured_rank = *ev->get_if<int64_t>();
    }
    const auto* arr = st && st->schema ? const_cast<repl::ObjectState&>(st->state).arrays.get(*st->schema, "StatRepInt32EveryoneProxy") : nullptr;
    if (!arr) return out;
    for (const repl::FastArrayItem& it : arr->items()) {
        const auto v = it.values.values.find(1);
        const auto* s = v == it.values.values.end() ? nullptr : v->second.get_if<repl::StatInt32Inst>();
        if (s) out.cap[s->record] = std::bit_cast<float>(s->value);
    }
    return out;
}

struct CapturedTally {
    size_t n = 0;
    size_t hp_ok = 0;
    size_t mp_ok = 0;
    size_t ms_ok = 0;
    size_t no_stats = 0;
    size_t no_stats_full = 0;
};

void check_captured_npc_open(TestHarness& h, combat::NpcStatBook& book, const spawns::OpenTemplate& o, CapturedTally& t) {
    Fixture& f = h.f;
    CapturedOpenInfo info = parse_captured_open(o);
    if (!info.valid) return;
    std::string err;
    const auto bp = f.motion.find(o.archetype_path);
    if (!info.cap.count(content::kGuidMaxHealth)) {
        ++t.no_stats;
        auto own = bp ? book.make(*bp, static_cast<int>(info.effective_level), err) : nullptr;
        const stats::StatValue* mh = own ? own->find(content::kGuidMaxHealth) : nullptr;
        const stats::StatValue* ch = own ? own->find(content::kGuidHealth) : nullptr;
        const bool full = mh && ch && mh->value > 0 && ch->value == mh->value;
        if (full) ++t.no_stats_full;
        CHECKF(full, "%s at level %lld: its open has no MaxHealth, and the engine gives it a Health equal to its MaxHealth, above 0",
               spawns::blueprint_leaf(o.archetype_path).c_str(), static_cast<long long>(info.effective_level));
        if (mh) {
            std::printf("    %-40s L%-3lld engine Health %.1f / MaxHealth %.1f (its open has none)\n",
                        spawns::blueprint_leaf(o.archetype_path).c_str(), static_cast<long long>(info.effective_level), ch ? ch->value : -1.0, mh->value);
        }
        return;
    }
    if (!bp || !info.found_level) return;
    auto cs = book.make(*bp, static_cast<int>(info.effective_level), err);
    if (!cs) return;
    ++t.n;
    const double hp = cs->find(content::kGuidMaxHealth)->value;
    const stats::StatValue* mp = cs->find(f.data->ids().max_mana);
    const stats::StatValue* ms = cs->find(f.data->ids().move_speed_mult);
    const double want_hp = info.cap[content::kGuidMaxHealth];
    const double want_mp = info.cap.count(f.data->ids().max_mana) ? info.cap[f.data->ids().max_mana] : -1;
    const double want_ms = info.cap.count(f.data->ids().move_speed_mult) ? info.cap[f.data->ids().move_speed_mult] : -1;
    const bool hp_within = std::fabs(hp - want_hp) <= 0.01 * want_hp;
    const bool mp_within = mp && want_mp >= 0 && std::fabs(mp->value - want_mp) <= 0.01 * std::max(1.0, want_mp);
    const bool guard = bp->list2 == combat::kGuidGuardOvertunedList;
    const double rule_ms = guard ? 1.0 : 1.05 + 0.1 * static_cast<double>(cs->npc().elite_rank);
    const bool ms_within = ms && want_ms >= 0 && std::fabs(ms->value - want_ms) <= 1e-4 && std::fabs(want_ms - rule_ms) <= 1e-4;
    if (hp_within) ++t.hp_ok;
    if (mp_within) ++t.mp_ok;
    if (ms_within) ++t.ms_ok;
    std::printf("    %-40s L%-3lld MaxHealth %9.1f / %9.1f (%.3f)  MaxMana %9.1f / %9.1f  MoveSpeedMult %.2f / %.2f  dist %d,%d,%d,%d,%d,%d,%d,%d elite %s rank %lld\n",
                spawns::blueprint_leaf(o.archetype_path).c_str(), static_cast<long long>(info.effective_level), hp, want_hp, hp / want_hp, mp ? mp->value : -1.0, want_mp,
                ms ? ms->value : -1.0, want_ms, bp->dist[0], bp->dist[1], bp->dist[2], bp->dist[3], bp->dist[4], bp->dist[5], bp->dist[6], bp->dist[7],
                bp->elite.empty() ? "-" : (std::to_string(bp->elite[0].first) + ":" + std::to_string(bp->elite[0].second)).c_str(), static_cast<long long>(info.captured_rank));
    CHECKF(hp_within && (want_mp < 0 || mp_within) && (want_ms < 0 || ms_within), "%s at level %lld: MaxHealth, MaxMana and MoveSpeedMult are the captured open's",
           spawns::blueprint_leaf(o.archetype_path).c_str(), static_cast<long long>(info.effective_level));
}

void test_captured_npcs(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the captured NPCs' stats\n");
    std::string err;
    const auto captured = spawns::CapturedData::load(err);
    CHECKF(captured.has_value(), "the captured data loads: %s", err.c_str());
    if (!captured) return;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    CapturedTally t;
    for (const spawns::OpenTemplate& o : captured->opens()) {
        check_captured_npc_open(h, book, o, t);
    }
    std::printf("  %zu captured NPCs: MaxHealth within 1 %% for %zu, MaxMana for %zu, MoveSpeedMult 1.05 + 0.1 x rank for %zu\n", t.n, t.hp_ok, t.mp_ok, t.ms_ok);
    CHECK(t.n >= 20 && t.hp_ok == t.n, "every captured NPC's MaxHealth is the engine's, within 1 %");
    std::printf("  %zu captured NPCs without stats in their open: the engine gives full Health to %zu\n", t.no_stats, t.no_stats_full);
    CHECK(t.no_stats > 0 && t.no_stats_full == t.no_stats, "every captured NPC whose open has no stats gets a Health equal to its MaxHealth, above 0, from the engine");
}

void test_aspect(TestHarness& h) {
    std::printf("the element and aspect of a hit\n");
    using combat::aspect_of;
    CHECK(aspect_of({"Element.Physical.Slash", "ability.weaponcombo"}).element == "Element.Physical.Slash" && aspect_of({"Element.Physical.Slash"}).physical, "a slash is physical");
    CHECK(!aspect_of({"Element.Magical.Fire"}).physical && aspect_of({"Element.Magical.Fire"}).element == "Element.Magical.Fire", "fire is magical");
    CHECK(aspect_of({"Element.Physical"}).element == "Element.Physical" && aspect_of({"Element.Physical"}).physical, "a bare Element.Physical is physical, with no element of the table");
    CHECK(aspect_of({"ability.Melee"}).physical && aspect_of({"ability.Melee"}).element.empty(), "a hit with no element is physical");
    CHECK(!aspect_of({"Element.Physical.Slash", "ability.MagicalScalingOverride"}).physical, "the magical scaling override wins");
    CHECK(aspect_of({"Element.Magical.Arcane", "ability.PhysicalScalingOverride"}).physical, "...and the physical one");
    CHECK(combat::tag_matches("Element.Physical.Slash", "Element.Physical") && !combat::tag_matches("Element.PhysicalX", "Element.Physical") && combat::tag_matches("a", "a"),
          "a tag matches itself and its descendants only");
    CHECK(combat::hit_flag_bit("Crit") == 1 && combat::hit_flag_bit("Lethal") == 2 && combat::hit_flag_bit("Block") == 4 && combat::hit_flag_bit("Miss") == 8 &&
              combat::hit_flag_bit("ActiveBlock") == 64 && combat::hit_flag_bit("Nope") == 0,
          "EHitFlag names are the bits");
}

struct LoneNpc final : combat::Fighter {
    std::unique_ptr<stats::CharacterStats> cs;
    std::vector<int64_t>                   aligns;
    combat::Vec3                           pos;
    int                                    lvl = 1;
    uint64_t                               ident = 99;
    uint64_t id() const override { return ident; }
    bool is_player() const override { return false; }
    int level() const override { return lvl; }
    combat::Vec3 position() const override { return pos; }
    bool alive() const override { return cs->find(content::kGuidHealth)->value > 0; }
    const std::vector<int64_t>& alignments() const override { return aligns; }
    stats::CharacterStats& stats() override { return *cs; }
};

std::unique_ptr<LoneNpc> make_lone(Fixture& f, const char* blueprint, int level) {
    const auto bp = f.blueprint(blueprint);
    if (!bp) return nullptr;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    std::string err;
    auto n = std::make_unique<LoneNpc>();
    n->cs = book.make(*bp, level, err);
    if (!n->cs) return nullptr;
    n->aligns.assign(bp->alignments.begin(), bp->alignments.end());
    n->lvl = level;
    return n;
}

void check_axe_combo_rolls(TestHarness& h, combat::HitResolver& resolver, combat::Fighter& player, combat::Fighter& wolf, const abilities::AbilityDef& axe) {
    const abilities::HitDef* hit1 = h.f.catalog->hit(axe.hits[0].hit_guid);
    CHECK(hit1 && !hit1->tags.empty() && hit1->tags[0] == "Element.Physical.Slash" && hit1->mods.size() == 1 && hit1->mods[0].stat_guid == content::kGuidHealth,
          "its first hit carries its event tags (Element.Physical.Slash ...) and one Health mod");
    if (!hit1) return;
    combat::HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    meta.source_guid = axe.row.guid;
    meta.ability_tags = axe.row.tags;
    double total = 0;
    int resolved = 0, misses = 0, blocks = 0, crits = 0;
    constexpr int kRolls = 400;
    for (int i = 0; i < kRolls; ++i) {
        combat::Resolved r;
        std::string why;
        CHECKF(resolver.resolve(player, wolf, *hit1, meta, r, why), "the formula runs: %s", why.c_str());
        if (r.miss()) {
            ++misses;
            CHECK(r.mods.empty(), "a miss has no stat mods");
            continue;
        }
        if (r.flags & stats::kHitBlock) ++blocks;
        if (r.flags & stats::kHitCrit) ++crits;
        if (r.mods.size() != 1) continue;
        ++resolved;
        CHECKF(r.mods[0].stat_guid == content::kGuidHealth && r.mods[0].value < 0 && std::isfinite(r.mods[0].value) && r.mods[0].stat_mod_guid == hit1->mods[0].stat_mod_guid,
               "a hit takes Health (%g)", r.mods[0].value);
        total += r.mods[0].value;
        CHECK(r.threat > 0, "a damaging hit has threat");
    }
    const double avg = resolved ? total / resolved : 0;
    CHECKF(resolved > 0 && avg < -1 && avg > -2000, "a level-20 Fighter's combo hit on a level-10 Wolf takes %.1f Health on average (the captured player's hits: 20 to 30)", -avg);
    CHECKF(misses + blocks + crits <= kRolls, "the rolls make flags (%d misses, %d blocks, %d crits of %d)", misses, blocks, crits, kRolls);
}

void check_forced_rolls_and_mitigation(TestHarness& h, combat::HitResolver& resolver, combat::Fighter& player, combat::Fighter& wolf, const abilities::AbilityDef& axe) {
    Fixture& f = h.f;
    const abilities::HitDef* hit1 = f.catalog->hit(axe.hits[0].hit_guid);
    if (!hit1) return;
    combat::HitMeta meta{content::kSourceTypeAbility, axe.row.guid, axe.row.tags};
    abilities::HitDef forced = *hit1;
    forced.force_roll_crit = true;
    forced.force_cant_miss = true;
    forced.force_cant_block = true;
    combat::Resolved crit;
    std::string why;
    CHECK(resolver.resolve(player, wolf, forced, meta, crit, why) && (crit.flags & stats::kHitCrit) && crit.mods.size() == 1, "a hit that always crits crits");
    abilities::HitDef plain = *hit1;
    plain.force_cant_crit = true;
    plain.force_cant_miss = true;
    plain.force_cant_block = true;
    combat::Resolved normal;
    CHECK(resolver.resolve(player, wolf, plain, meta, normal, why) && normal.flags == 0 && normal.mods.size() == 1, "a hit that cannot crit, miss or block has no flags");
    if (crit.mods.size() == 1 && normal.mods.size() == 1) {
        CHECKF(crit.mods[0].value < normal.mods[0].value, "a crit hits harder (%.2f against %.2f)", crit.mods[0].value, normal.mods[0].value);
    }
    abilities::HitDef blocked = plain;
    blocked.force_roll_block = true;
    blocked.force_cant_block = false;
    combat::Resolved bl;
    CHECK(resolver.resolve(player, wolf, blocked, meta, bl, why) && (bl.flags & stats::kHitBlock) && bl.mods.size() == 1, "a hit that is always blocked is");
    abilities::HitDef missing = plain;
    missing.force_roll_miss = true;
    missing.force_cant_miss = false;
    combat::Resolved ms;
    CHECK(resolver.resolve(player, wolf, missing, meta, ms, why) && ms.miss() && ms.mods.empty(), "a hit that always misses does nothing");

    auto tough = make_lone(f, "Bear_C", 40);
    auto weak = make_lone(f, "Wolf_C", 5);
    if (tough && weak) {
        combat::Resolved a, b;
        CHECK(resolver.resolve(player, *weak, plain, meta, a, why) && resolver.resolve(player, *tough, plain, meta, b, why) && a.mods.size() == 1 && b.mods.size() == 1, "the hit runs on both targets");
        if (a.mods.size() == 1 && b.mods.size() == 1) {
            CHECKF(-b.mods[0].value < -a.mods[0].value, "a level-40 Bear takes less than a level-5 Wolf (%.2f against %.2f): its Mitigation is read", -b.mods[0].value, -a.mods[0].value);
        }
    }
}

void check_wolf_bite_scaling(TestHarness& h, combat::HitResolver& resolver, combat::Fighter& player) {
    Fixture& f = h.f;
    const abilities::AbilityDef* bite = f.catalog->ability_named("Wolf_Bite");
    CHECK(bite && !bite->hits.empty(), "the Wolf's bite is in the tables");
    if (!bite || bite->hits.empty()) return;
    const abilities::HitDef* bite_hit = f.catalog->hit(bite->hits[0].hit_guid);
    CHECK(bite_hit && (bite_hit->row.applicable_targets & 2) && !bite_hit->mods.empty(), "...aimed at enemies, with a stat mod");
    if (!bite_hit) return;
    abilities::HitDef sure = *bite_hit;
    sure.force_cant_crit = sure.force_cant_miss = sure.force_cant_block = true;
    combat::HitMeta bite_meta{content::kSourceTypeAbility, bite->row.guid, bite->row.tags};
    auto low = make_lone(f, "Wolf_C", 5), high = make_lone(f, "Wolf_C", 25);
    combat::Resolved lo, hi;
    std::string why;
    CHECK(resolver.resolve(*low, player, sure, bite_meta, lo, why) && resolver.resolve(*high, player, sure, bite_meta, hi, why) && lo.mods.size() == 1 && hi.mods.size() == 1,
          "a Wolf's bite on the player runs for two levels");
    if (lo.mods.size() == 1 && hi.mods.size() == 1) {
        CHECKF(lo.mods[0].value < 0 && hi.mods[0].value < lo.mods[0].value, "the bite is level-scaled: %.2f at level 5, %.2f at level 25", lo.mods[0].value, hi.mods[0].value);
        const double health = player.stats().find(content::kGuidMaxHealth)->value;
        CHECKF(-hi.mods[0].value < health && -hi.mods[0].value > health * 0.001, "...and is a fraction of the player's %.0f Health (%.2f)", health, -hi.mods[0].value);
    }
}

void test_hit_rules(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the hit rules: a player's hit on an NPC and an NPC's on a player\n");
    Rig rig(4, 20, {});
    CHECKF(rig.ok(), "the rig: %s", rig.problem.c_str());
    if (!rig.ok()) return;
    combat_rig::RigFighter player(rig, 7, {0, 0, 1098});
    auto wolf = make_lone(f, "Wolf_C", 10);
    CHECK(wolf != nullptr, "a level 10 Wolf's stats run");
    if (!wolf) return;
    wolf->pos = {200, 0, 1088};
    combat::HitResolver resolver(*f.catalog, *f.data, 12345);
    const abilities::AbilityDef* axe = f.catalog->ability_named("WeaponCombo_Melee_1H_Axe");
    CHECK(axe && !axe->hits.empty(), "the 1H axe combo is in the tables");
    if (!axe || axe->hits.empty()) return;
    check_axe_combo_rolls(h, resolver, player, *wolf, *axe);
    check_forced_rolls_and_mitigation(h, resolver, player, *wolf, *axe);
    check_wolf_bite_scaling(h, resolver, player);
}

struct FightEnv {
    Rig                             rig{4, 20, {}};
    combat_rig::World               w;
    combat::NpcStatBook             book;
    combat::CombatWorld             world;
    combat_rig::FakePlayer          player;
    std::vector<combat::DeathEvent> deaths;
    uint64_t                        wolf = 0;
    double                          now = 0;

    explicit FightEnv(Fixture& f)
        : book(*f.ctx->bundle, *f.ctx->gear),
          world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, [] {
              combat::CombatWorldConfig cfg;
              cfg.seed = 5;
              return cfg;
          }()),
          player(rig, 77, {5000, 0, 1098}) {
        world.add_player(&player);
        world.set_death_hook([this](const combat::DeathEvent& e) { deaths.push_back(e); });
    }

    void run(double seconds) {
        const double step = 1.0 / 15.0;
        const double end = now + seconds;
        while (now < end) {
            now += step;
            w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
            w.npcs->tick(now);
            world.tick(now);
        }
    }
};

void log_wolf_stat_arrays(const world::WorldActor& wa, const combat_rig::World& w, const std::string& wolf_path) {
    for (const repl::Subobject& sub : wa.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UAoCStatsComponent") continue;
        const auto* arr = const_cast<repl::FastArrays&>(sub.obj.arrays).get(*sub.obj.schema, "StatRepInt32EveryoneProxy");
        for (const char* an : {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy", "StatRepFloatArrayEveryoneProxy"}) {
            const auto* a2 = const_cast<repl::FastArrays&>(sub.obj.arrays).get(*sub.obj.schema, an);
            std::printf("    %s: %zd items\n", an, a2 ? static_cast<ptrdiff_t>(a2->items().size()) : -1);
        }
        std::printf("    the template: %s, %zu blocks\n", w.captured->template_for(wolf_path) ? "found" : "missing", w.captured->opens().size());
        std::printf("    the stat items:");
        if (arr) {
            for (const repl::FastArrayItem& it : arr->items()) {
                const auto v = it.values.values.find(1);
                const auto* si = v == it.values.values.end() ? nullptr : v->second.get_if<repl::StatInt32Inst>();
                if (si) std::printf(" %lld=%g", static_cast<long long>(si->record), std::bit_cast<float>(si->value));
            }
        }
        std::printf("\n");
    }
}

void check_fight_aggro_and_attack(TestHarness& h, FightEnv& env, const std::string& wolf_path) {
    env.run(2.0);
    CHECK(!env.world.is_combatant(env.wolf) || env.world.ai_state(env.wolf) == npc::ai::State::Idle, "a player 50 m away: the Wolf stays idle");
    const world::WorldActor* wa = env.w.registry.find(env.wolf);
    CHECK(wa && !combat_rig::combat_flag(*wa, "bInCombat"), "its CombatInfo says it is not in combat");

    env.player.rig_fighter().move_to({600, 0, 1098});
    env.run(0.6);
    CHECK(env.world.is_combatant(env.wolf), "a player 6 m away makes it a combatant");
    const auto st = env.world.ai_state(env.wolf);
    CHECK(st && (*st == npc::ai::State::Chase || *st == npc::ai::State::Attack), "the Wolf aggros: chase");
    wa = env.w.registry.find(env.wolf);
    CHECK(wa && combat_rig::combat_flag(*wa, "bInCombat"), "its bInCombat goes up");
    CHECK(wa && combat_rig::active_target(*wa) == 77, "its ActiveTargets names the player's pawn");
    float max_health = 0;
    if (wa) log_wolf_stat_arrays(*wa, env.w, wolf_path);
    CHECK(wa && combat::read_stat(*wa, content::kGuidMaxHealth, max_health), "its MaxHealth item exists");
    CHECKF(near(max_health, env.world.max_health_of(env.wolf), 0.5) && max_health > 100, "the item holds the engine's MaxHealth for level 10 (%.1f)", max_health);

    env.run(6.0);
    CHECK(!env.player.hits.empty(), "the Wolf bites the player");
    CHECKF(env.world.stats().npc_attacks >= 1, "%llu attacks started", static_cast<unsigned long long>(env.world.stats().npc_attacks));
    CHECK(env.player.in_combat, "the player's combat flag is up");
    const world::WorldActor* wolf_now = env.w.registry.find(env.wolf);
    const double dist = std::hypot(wolf_now->x - env.player.rig_fighter().position().x, wolf_now->y - env.player.rig_fighter().position().y);
    CHECKF(dist < 450, "the Wolf stands at the player (%.0f cm)", dist);
    const stats::StatValue* hp = env.player.rig_fighter().stats().find(content::kGuidHealth);
    const stats::StatValue* hp_max = env.player.rig_fighter().stats().find(content::kGuidMaxHealth);
    CHECK(hp && hp_max && hp->value < hp_max->value, "the player lost Health");
    if (!env.player.hits.empty()) {
        const combat::IncomingHit& ih = env.player.hits.front();
        CHECK(ih.instigator == env.wolf && ih.hit_guid != 0 && ih.source_type == content::kSourceTypeAbility && !ih.mods.empty(),
              "the hit names the Wolf, the hit record and the ability");
    }
}

void check_fight_retaliation_and_leash(TestHarness& h, FightEnv& env) {
    combat::Resolved hit;
    hit.mods.push_back({1, content::kGuidHealth, -30.0});
    hit.threat = 30.0;
    const double before = env.world.health_of(env.wolf);
    const combat::Delivery d = env.world.apply_hit_to_npc(env.wolf, 77, hit, env.now);
    // Collision cylinder sits at actor + 4 inside the actor's GUID block, avoiding NetGuidMismatch against adjacent actors.
    CHECK(d.applied && !d.lethal && d.effective.size() == 1 && near(d.effective[0], -30.0, 1e-3) && d.target_object == env.wolf + 4 &&
              d.target_object == env.w.registry.find(env.wolf)->npc.collision_cylinder,
          "a hit applies: Health -30, confirmed against the Wolf's collision cylinder (the actor's + 4, the registry's)");
    CHECKF(near(env.world.health_of(env.wolf), before - 30.0, 0.01), "its Health is %.2f", env.world.health_of(env.wolf));
    float item = 0;
    CHECK(combat::read_stat(*env.w.registry.find(env.wolf), content::kGuidHealth, item) && near(item, static_cast<float>(before - 30.0), 0.01),
          "the replicated Health item moved with it");
    CHECK(env.world.actor_of_object(env.wolf + 4) == env.wolf && env.world.actor_of_object(env.wolf) == env.wolf &&
              env.world.actor_of_object(env.wolf + 24) != env.wolf && env.world.actor_of_object(env.wolf + 28) != env.wolf && env.world.actor_of_object(1) == 0,
          "the collision cylinder and the actor name the NPC; the GUIDs outside its block (+ 24, + 28: the next actor's components) and a stranger's object do not");

    env.player.rig_fighter().move_to({12000, 0, 1098});
    env.run(0.8);
    CHECK(env.world.ai_state(env.wolf) == npc::ai::State::Reset || env.world.ai_state(env.wolf) == npc::ai::State::Idle, "the player left: the Wolf resets");
    env.run(4.0);
    const world::WorldActor* wa = env.w.registry.find(env.wolf);
    CHECK(wa && !combat_rig::combat_flag(*wa, "bInCombat") && combat_rig::active_target(*wa) == 0, "its combat flags are down");
    CHECKF(near(env.world.health_of(env.wolf), env.world.max_health_of(env.wolf), 0.01), "it healed to full (%.1f of %.1f)",
           env.world.health_of(env.wolf), env.world.max_health_of(env.wolf));
    CHECK(env.world.stats().resets >= 1, "a reset was counted");
    env.run(10.0);
    wa = env.w.registry.find(env.wolf);
    CHECKF(wa && std::hypot(wa->x, wa->y) < 400, "it walked home (%.0f cm away)", wa ? std::hypot(wa->x, wa->y) : -1.0);
    CHECK(env.world.ai_state(env.wolf) == npc::ai::State::Idle, "and is idle again");
}

void check_fight_lethal_kill(TestHarness& h, FightEnv& env) {
    env.world.add_threat(env.wolf, 77, 10.0, env.now);
    combat::Resolved kill;
    kill.mods.push_back({1, content::kGuidHealth, -1.0e9});
    const combat::Delivery k = env.world.apply_hit_to_npc(env.wolf, 77, kill, env.now);
    CHECK(k.applied && k.lethal && (k.flags & stats::kHitLethal), "an overwhelming hit is lethal and carries the Lethal flag");
    CHECKF((k.effective.size() == 1 && near(k.effective[0], -env.world.max_health_of(env.wolf), 0.01)) || (!k.effective.empty() && k.effective[0] < 0),
           "its effective loss is what the Wolf had (%.1f)", k.effective.empty() ? 0.0 : k.effective[0]);
    CHECK(env.world.dead(env.wolf) && env.world.ai_state(env.wolf) == npc::ai::State::Dead, "the Wolf is dead and its AI is off");
    const world::WorldActor* wa = env.w.registry.find(env.wolf);
    CHECK(wa && combat_rig::combat_flag(*wa, "bIsDead") && !combat_rig::combat_flag(*wa, "bInCombat") && combat_rig::active_target(*wa) == 0,
          "bIsDead is 1, the combat flags are down, ActiveTargets is obj:0");
    float item = 0;
    CHECK(combat::read_stat(*wa, content::kGuidHealth, item) && item == 0.0f, "its Health item is 0");
    CHECK(env.deaths.size() == 1 && env.deaths[0].npc == env.wolf && env.deaths[0].killer == 77 && env.deaths[0].level == 10 && env.deaths[0].blueprint == "Wolf" &&
              env.deaths[0].attackers.count(77) && env.deaths[0].info && env.deaths[0].motion,
          "the death hook ran once: who killed it, what it was, who fought it");
    const combat::Delivery again = env.world.apply_hit_to_npc(env.wolf, 77, kill, env.now);
    CHECK(!again.applied && env.deaths.size() == 1, "a dead NPC takes no more hits and dies once");
    const double dead_x = wa->x;
    env.run(3.0);
    CHECK(env.w.registry.find(env.wolf)->x == dead_x, "the corpse does not move");
}

void test_fight(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("a fight: aggro, chase, attack, leash, reset, death\n");
    FightEnv env(f);
    CHECKF(env.rig.ok(), "the rig: %s", env.rig.problem.c_str());
    if (!env.rig.ok()) return;
    CHECKF(env.w.problem.empty() && env.w.captured, "the world: %s", env.w.problem.c_str());
    if (!env.w.problem.empty()) return;
    const auto wolf_bp = f.blueprint("Wolf_C");
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    CHECK(!wolf_path.empty() && wolf_bp, "the Blueprint table has the Wolf");
    if (wolf_path.empty()) return;
    env.wolf = env.w.spawn(wolf_path, 0, 0, 10, 900);
    CHECK(env.wolf != 0 && !env.world.is_combatant(env.wolf), "a Wolf at level 10 stands at the origin; it is no combatant yet");
    if (env.wolf == 0) return;
    check_fight_aggro_and_attack(h, env, wolf_path);
    check_fight_retaliation_and_leash(h, env);
    check_fight_lethal_kill(h, env);
}

void test_hostile(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("hostile cases\n");
    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    combat_rig::World w;
    if (!w.problem.empty()) return;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion);
    const uint64_t wolf = w.spawn(wolf_path, 0, 0, 10);
    combat_rig::FakePlayer player(rig, 77, {300, 0, 1098});
    world.add_player(&player);
    combat::Resolved r;
    r.mods.push_back({1, content::kGuidHealth, -5.0});
    CHECK(!world.apply_hit_to_npc(12345, 77, r, 1.0).applied, "a hit on an actor that is not there applies to nothing");
    CHECK(world.fighter_of(12345) == nullptr && world.fighter_of(1) == nullptr, "no fighter for a stranger");
    combat::Resolved nan;
    nan.mods.push_back({1, content::kGuidHealth, std::nan("")});
    const combat::Delivery dn = world.apply_hit_to_npc(wolf, 77, nan, 1.0);
    CHECK(dn.applied && std::isfinite(world.health_of(wolf)) && world.health_of(wolf) > 0, "a NaN hit leaves Health a number");
    combat::Resolved heal;
    heal.mods.push_back({1, content::kGuidHealth, 1.0e9});
    world.apply_hit_to_npc(wolf, 77, heal, 1.0);
    CHECK(near(world.health_of(wolf), world.max_health_of(wolf), 1e-3), "a heal stops at the maximum");
    combat::Resolved miss;
    miss.flags = stats::kHitMiss;
    const double hp = world.health_of(wolf);
    const combat::Delivery dm = world.apply_hit_to_npc(wolf, 77, miss, 2.0);
    CHECK(dm.applied && !dm.lethal && world.health_of(wolf) == hp && (dm.flags & stats::kHitMiss), "a miss costs no Health");
    const combat::TargetInfo ti = world.inspect(wolf, player.fighter());
    CHECK(ti.exists && ti.alive && ti.relation == combat::Relation::Hostile && ti.surface_distance_cm > 100 && ti.surface_distance_cm < 300 && ti.in_line_of_sight,
          "inspect: a living enemy 3 m away in sight");
    CHECK(!world.inspect(999, player.fighter()).exists, "inspect: no such actor");
    combat::CombatWorldConfig off;
    off.enabled = false;
    combat::CombatWorld quiet(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, off);
    quiet.add_player(&player);
    for (double t = 0; t < 5; t += 0.1) quiet.tick(t);
    CHECK(quiet.combatants() == 0, "with combat off nothing thinks");
}

double wolf_first_hit_health(Fixture& f, double mult) {
    const double none = std::nan("");
    Rig rig(4, 20, {});
    if (!rig.ok()) return none;
    combat_rig::World w;
    if (!w.problem.empty()) return none;
    const std::string wolf_path = creature_path(f, "/Beast/Canine/Wolf.Default__Wolf_C");
    if (wolf_path.empty()) return none;
    combat::NpcStatBook book(*f.ctx->bundle, *f.ctx->gear);
    combat::CombatWorldConfig cfg;
    cfg.seed = 5;
    cfg.npc_damage_mult = mult;
    combat::CombatWorld world(w.registry, w.npcs.get(), &navfix::shared_nav(), *f.data, *f.catalog, book, f.motion, cfg);
    if (w.spawn(wolf_path, 0, 0, 10, 900) == 0) return none;
    combat_rig::FakePlayer player(rig, 77, {600, 0, 1098});
    world.add_player(&player);
    double now = 0;
    while (now < 10.0 && player.hits.empty()) {
        now += 1.0 / 15.0;
        w.npcs->note_player(77, player.rig_fighter().position().x, player.rig_fighter().position().y, now);
        w.npcs->tick(now);
        world.tick(now);
    }
    if (player.hits.empty()) return none;
    for (const combat::ResolvedMod& m : player.hits.front().mods) {
        if (m.stat_guid == content::kGuidHealth) return m.value;
    }
    return none;
}

void test_npc_damage_mult(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the NPC damage multiplier: 0.2 takes 80 %% off the Health a creature's hit does to a player\n");
    const double full = wolf_first_hit_health(f, 1.0);
    const double fifth = wolf_first_hit_health(f, 0.2);
    CHECK(std::isfinite(full) && full < 0, "a level 10 Wolf's first hit takes Health at the data's own damage");
    CHECK(std::isfinite(fifth) && std::isfinite(full) && near(fifth, full * 0.2, std::fabs(full) * 1e-6 + 1e-6), "with the multiplier at 0.2 the same hit takes a fifth of it");
    const double none = wolf_first_hit_health(f, 0.0);
    CHECK(std::isfinite(none) && none == 0.0, "with the multiplier at 0 the hit takes nothing");
}

std::vector<std::string> health_exprs_of(Fixture& f, const abilities::AbilityDef& d) {
    std::vector<std::string> out;
    for (const content::AbilityHitRow& hr : d.hits) {
        const abilities::HitDef* hit = f.catalog->hit(hr.hit_guid);
        if (!hit) continue;
        for (const content::ModRow& m : hit->mods) {
            if (m.stat_guid != content::kGuidHealth) continue;
            const content::ExprRow* e = f.catalog->expr(m.value_expr);
            out.push_back(e ? e->text : std::string());
        }
    }
    return out;
}

void test_fallback_attack(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the attack of a creature with no kit of its own: a factor times the shared damage equation, not a flat number\n");
    const combat::CombatWorldConfig cfg;
    const abilities::AbilityDef* fallback = f.catalog->ability_named(cfg.fallback_ability);
    CHECK(fallback != nullptr, "the fallback ability is in the data");
    if (fallback) {
        const std::vector<std::string> now = health_exprs_of(f, *fallback);
        bool formula = !now.empty();
        for (const std::string& t : now) formula = formula && t.find("EvalFormula") != std::string::npos;
        CHECK(formula, "its Health damage is a factor times the shared damage equation, so the creature's level and stats, the player's mitigation and the level difference all count");
    }
    const abilities::AbilityDef* flat = f.catalog->ability_named("NPC_Bandit_Melee_Slash_ABL_1");
    if (flat) {
        const std::vector<std::string> placeholder = health_exprs_of(f, *flat);
        CHECK(placeholder.size() == 1 && placeholder[0] == "-50", "the data's NPC_Bandit_Melee_Slash_ABL_1 takes a flat 50 Health whatever the creature and the player are");
    }
}

void test_combat_flag(TestHarness& h) {
    Fixture& f = h.f;
    std::printf("the player's in-combat flag ends the out-of-combat regeneration\n");
    Rig rig(4, 20, {});
    if (!rig.ok()) return;
    const int64_t regen = f.data->ids().health_regeneration;
    const stats::StatValue* v = rig.cs->find(regen);
    CHECK(v != nullptr, "the character has a HealthRegeneration stat");
    if (!v) return;
    const float out_of_combat = v->value;
    rig.cs->set_in_combat(true);
    rig.pipe->refresh(1.0, "combat starts", true);
    rig.take();
    const float in_combat = rig.cs->find(regen)->value;
    CHECKF(out_of_combat > 0 && std::fabs(out_of_combat / in_combat - 5.0) < 0.01, "out of combat it is five times faster (%.3f against %.3f)", out_of_combat, in_combat);
    CHECK(rig.pawn.stat(regen) == in_combat || rig.pawn.stat(regen) == -12345.0f, "the change goes to the pawn's arrays when the stat is replicated");
    rig.cs->set_in_combat(false);
    rig.pipe->refresh(7.0, "combat ends", true);
    rig.take();
    CHECK(rig.cs->find(regen)->value == out_of_combat, "and returns when combat ends");
}

}  // namespace

int main() {
    if (!real_db_present() || !std::filesystem::exists(NPC_MOTION_PATH)) {
        std::printf("[skip] test_worldd_combat: no content.db or npc_motion.tsv at %s: skipped\n", real_db_path().c_str());
        return 0;
    }
    Fixture f;
    if (!f.ok()) {
        std::printf("  [FAIL] fixture: %s\n", f.problem.c_str());
        return 1;
    }
    TestHarness h{f};
    test_data(h);
    test_npc_stats(h);
    test_captured_npcs(h);
    test_aspect(h);
    test_hit_rules(h);
    test_fight(h);
    test_hostile(h);
    test_npc_damage_mult(h);
    test_fallback_attack(h);
    test_combat_flag(h);
    run_combat_cast_grem_tests(h, *f.db);
    run_combat_gates_tests(h, *f.db);
    run_combat_lunge_chase_tests(h, *f.db);
    run_combat_projectile_tests(h, *f.db);
    run_combat_generic_tests(h, *f.db);
    run_combat_burn_tests(h, *f.db);
    run_combat_dots_tests(h, *f.db);
    std::printf("\n%d passed, %d failed\n", h.pass, h.fail);
    return h.fail == 0 ? 0 : 1;
}
