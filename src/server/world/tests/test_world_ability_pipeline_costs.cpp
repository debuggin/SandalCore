// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_world_ability_pipeline_common.h"

#include "session/summoner_pets.h"

namespace {

void check_catalog_jest_and_reload(const Rig &r) {
  const abilities::AbilityDef *jest =
      r.catalog->ability_named("Bard_Misc_QuickWit_Jest");
  bool pep_hit_gated = false;
  for (const auto &h : jest->hits) {
    const abilities::HitDef *hd = r.catalog->hit(h.hit_guid);
    if (hd->row.name == "Bard_QuickWit_Jest" && hd->row.requirement_expr != 0 &&
        r.catalog->program(hd->row.requirement_expr) >= 0)
      pep_hit_gated = true;
  }
  CHECK(pep_hit_gated,
        "Jest's Pep hit has its requirement compiled (its $SkillTree:Bard$ and "
        "$Skill:...$ were written as numbers by contentc)");
  std::string err;
  const auto again = abilities::Catalog::load(r.path, *r.ctx->bundle, err);
  CHECK(again && again->abilities() == r.catalog->abilities(),
        "the catalog loads again to the same size");
  stats::StatBundle other = *r.ctx->bundle;
  other.crc32 ^= 1;
  CHECK(!abilities::Catalog::load(r.path, other, err) &&
            err.find("not the one world runs") != std::string::npos,
        "a stat bundle of another build is refused (the programs would index "
        "the wrong table)");
}

void test_catalog() {
  std::printf("the catalog\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  CHECK(r.catalog->abilities() >= 200 && r.catalog->effects() >= 800 &&
            r.catalog->hits() >= 500 && r.catalog->exprs() >= 700,
        "every ability, effect, hit and expression of the tables is in memory");
  const abilities::AbilityDef *flourish =
      r.catalog->ability_named("Bard_Misc_Flourish");
  CHECK(flourish && flourish->row.cooldown_expr != 0 &&
            !flourish->costs.empty() && flourish->montages.size() == 1 &&
            flourish->montages[0].net_guid.value_or(0) ==
                6743654538719732531ull,
        "Flourish has its cost, cooldown and the montage whose retail NetGUID "
        "the captures name");
  CHECK(r.catalog->effect_named("Bard_Resonance") &&
            r.catalog->effect_named("Bard_Glee") &&
            r.catalog->hit(r.catalog->ability_named("Bard_Misc_Fortissimo")
                               ->hits[0]
                               .hit_guid),
        "the effects and hits are found by name and guid");
  CHECK(r.catalog->expr(0) == nullptr && r.catalog->program(0) == -1 &&
            r.catalog->program(flourish->row.cooldown_expr) >= 0,
        "an expression id of 0 is none, a compiled one has its program");
  check_catalog_jest_and_reload(r);
}

void check_cast_timeline_hits_and_charges(Rig &r, double t0) {
  std::vector<Seen> seen = r.run_to(t0 + 0.34);
  const Seen *hits = find_event(seen, [&](const Seen &s) {
    return effect_added(s, r.effect("Bard_Pep")) != nullptr;
  });
  CHECK(hits && near(hits->at, t0 + 0.335, 1e-9) &&
            effect_added(*hits, r.effect("Bard_Pep"))->duration == 4.0f &&
            near(stat_of(*hits, kMoveSpeedMult), 1.2, 1e-6),
        "0.035 s after the cost the self hit's effects: Pep for 4 s and "
        "MoveSpeedMult 1.2");
  CHECK(hits && effect_added(*hits, r.effect("Bard_Flourish_Power")) &&
            effect_added(*hits, r.effect("Bard_Flourish_Power"))->duration ==
                6.0f,
        "...and Flourish_Power for 6 s (the Bard holds the talent)");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
  seen = r.run_to(t0 + 0.335 + 3.99);
  CHECK(r.pawn.effect(r.effect("Bard_Pep")) != nullptr &&
            r.pawn.stat(kMoveSpeedMult) > 1.19f,
        "Pep is on at 3.99 s");
  seen = r.run_to(t0 + 0.335 + 4.01);
  CHECK(r.pawn.effect(r.effect("Bard_Pep")) == nullptr &&
            r.pawn.stat(kMoveSpeedMult) == 1.0f,
        "and gone at 4 s with MoveSpeedMult back to 1");
  const double t1 = t0 + 5.0;
  CHECK(r.trigger("Bard_Misc_Flourish", t1, false, 0) == Refusal::None,
        "a second Flourish 5 s later: accepted");
  r.run_to(t1 + 0.4);
  CHECK(r.pawn.cooldown(r.ability("Bard_Misc_Flourish")) &&
            r.pawn.cooldown(r.ability("Bard_Misc_Flourish"))->charges == 0,
        "the Cooldowns item says 0 charges left");
  CHECK(r.trigger("Bard_Misc_Flourish", t1 + 2.0, false, 0) ==
                Refusal::Charges &&
            !r.pawn.ends.empty() && r.pawn.ends.back() == r.next_client_id - 1,
        "a third is refused: out of charges, ClientEndAbility(its id)");
  r.run_to(t0 + 0.3 + 20.01);
  CHECK(r.pawn.cooldown(r.ability("Bard_Misc_Flourish")) &&
            r.pawn.cooldown(r.ability("Bard_Misc_Flourish"))->charges == 1,
        "20 s after the first cast a charge is back: the item says 1");
  r.run_to(t0 + 0.3 + 40.01);
  CHECK(r.pawn.cooldown(r.ability("Bard_Misc_Flourish")) == nullptr &&
            r.pipe->cooldowns_running() == 0,
        "20 s after that both are back and the item is gone");
  CHECK(r.trigger("Bard_Misc_Flourish", t0 + 42.0, false, 0) == Refusal::None,
        "(and Flourish can be cast again)");
  r.run_to(t0 + 43.0);
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void check_cast_timeline_cost_step(Rig &r, const Seen &cost, double max_mana) {
  CHECK(near(max_mana - stat_of(cost, content::kGuidMana), 10 * kManaMult25,
             1e-3) &&
            near(10 * kManaMult25, 56.875, 1e-9),
        "Mana falls by 10 x 5.6875 = 56.875");
  const auto &cd = cost.change.cooldowns_set[0];
  CHECK(cd.ability_guid == r.ability("Bard_Misc_Flourish") && cd.triggered &&
            cd.initial_cooldown == 20.0f && cd.charges == 1 &&
            near(cd.end_time - cd.start_time, 20.0, 1e-6),
        "the Cooldowns item: 20 s, 1 charge left of the 2 (the first use)");
  const bootstrap::EffectItem *res =
      effect_added(cost, r.effect("Bard_Resonance"));
  CHECK(stat_of(cost, kResonanceStat) == 1.0f && res &&
            res->duration == 30.0f && res->stacks == 1,
        "Bard_Resonance: the stat is 1 and its effect lasts 30 s with 1 stack");
}

void check_cast_timeline_first_step(Rig &r, double t0, double max_mana) {
  std::vector<Seen> seen = r.run_to(t0 + 0.301);
  const Seen *item = find_event(
      seen, [](const Seen &s) { return !s.change.abilities_added.empty(); });
  const Seen *cost = find_event(
      seen, [&](const Seen &s) { return has_stat(s, content::kGuidMana); });
  CHECK(item && near(item->at, t0 + 0.3, 1e-9) &&
            item->change.abilities_added[0].ability_guid ==
                r.ability("Bard_Misc_Flourish") &&
            item->change.abilities_added[0].montage_group == "EmoteGroup" &&
            item->change.abilities_added[0].montage_guid ==
                6743654538719732531ull &&
            !item->change.abilities_added[0].targets_self &&
            item->change.abilities_removed.empty(),
        "at 0.3 s the ability item: Flourish, MontageGroup EmoteGroup, the "
        "montage's static NetGUID, no target, nothing before it to remove");
  CHECK(
      cost && near(cost->at, t0 + 0.3, 1e-9) &&
          !cost->change.cooldowns_set.empty() &&
          has_stat(*cost, kResonanceStat),
      "the cost step comes with it: the Cooldowns item and the class resource");
  if (!cost)
    return;
  check_cast_timeline_cost_step(r, *cost, max_mana);
  check_cast_timeline_hits_and_charges(r, t0);
}

void test_cast_timeline() {
  std::printf("a cast: Flourish\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Misc_Flourish"});
  const double max_mana = r.pawn.stat(content::kGuidMana);
  CHECK(near(max_mana, r.pawn.stat(content::kGuidMaxMana), 1e-3) &&
            r.pawn.stat(kMoveSpeedMult) == 1.0f,
        "(the Bard starts with full Mana and MoveSpeedMult 1)");
  std::vector<Seen> seen;
  const double t0 = 2.0;
  CHECK(r.trigger("Bard_Misc_Flourish", t0, false, 0, &seen) == Refusal::None &&
            seen.empty(),
        "the trigger is accepted and changes nothing at once: the montage's "
        "first notify is 0.3 s on");
  seen = r.run_to(t0 + 0.299);
  CHECK(find_event(seen,
                   [](const Seen &s) {
                     return !s.change.abilities_added.empty();
                   }) == nullptr,
        "nothing before 0.3 s");
  check_cast_timeline_first_step(r, t0, max_mana);
}

void test_mana_costs() {
  std::printf("the Mana costs, k x ManaCostMult(25)\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  struct Row {
    const char *ability;
    double k;
    bool target;
  };
  const Row rows[] = {{"Bard_Misc_Flourish", 10, false},
                      {"Bard_Misc_QuickWit_Jest", 10, true},
                      {"Bard_Dance_Mesmerizing", 15, false},
                      {"Bard_Story_Joy", 15, true},
                      {"Bard_Dance_Nimble", 20, true},
                      {"Bard_Dance_Maddening", 20, false},
                      {"Bard_Misc_Misdirection", 20, false},
                      {"Bard_Dance_Shielding", 20, false}};
  r.know({"Bard_Misc_Flourish", "Bard_Misc_QuickWit_Jest",
          "Bard_Dance_Mesmerizing", "Bard_Story_Joy", "Bard_Dance_Nimble",
          "Bard_Dance_Maddening", "Bard_Misc_Misdirection",
          "Bard_Dance_Shielding"});
  double t = 5.0;
  for (const Row &row : rows) {
    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
    const double before = r.pawn.stat(content::kGuidMana);
    std::vector<Seen> seen;
    const Refusal rf = r.trigger(row.ability, t + 0.017, row.target, 0, &seen);
    CHECKF(rf == Refusal::None, "%s is accepted (%s)", row.ability,
           refusal_name(rf));
    seen = r.run_to(t + 3.0);
    const Seen *cost = find_event(seen, [&](const Seen &s) {
      return has_stat(s, content::kGuidMana) &&
             stat_of(s, content::kGuidMana) < before;
    });
    CHECKF(cost && near(before - stat_of(*cost, content::kGuidMana),
                        row.k * kManaMult25, 1e-3),
           "%s costs %.4f Mana (%.4f)", row.ability, row.k * kManaMult25,
           cost ? before - stat_of(*cost, content::kGuidMana) : -1.0);
    t += 100.0;
    r.run_to(t - 1.0);
  }
  CHECK(near(15 * kManaMult25, 85.3125, 1e-9) &&
            near(20 * kManaMult25, 113.75, 1e-9) &&
            near(35 * kManaMult25, 199.0625, 1e-9),
        "(85.3125, 113.75 and 199.0625: the research's other deltas)");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void test_cast_flood() {
  std::printf("a flood of casts\n");
  Rig flood(2, 25, kTalents);
  CHECKF(flood.ok(), "the rig: %s", flood.problem.c_str());
  if (!flood.ok())
    return;
  flood.know({"Bard_Misc_Flourish"});
  for (int id = 60; id < 60 + static_cast<int>(AbilityPipeline::kMaxCasts);
       ++id)
    flood.pipe->trigger(flood.ability("Bard_Misc_Flourish"), id, 2.15, {});
  CHECK(flood.pipe->trigger(flood.ability("Bard_Misc_Flourish"), 99, 2.16,
                            {}) == Refusal::Busy,
        "a flood of waiting casts is refused: Busy");
  flood.run_to(5.0);
  CHECK(flood.pawn.broken.empty() &&
            flood.pipe->charges_left(flood.ability("Bard_Misc_Flourish")) == 0,
        "(the casts that found no charge left at their cost step are over; the "
        "pawn took every change)");
}

void test_unaffordable_at_the_cost_step() {
  std::printf("a cast that cannot be paid for when its cost comes\n");
  Rig q(2, 25, kTalents);
  CHECKF(q.ok(), "the rig: %s", q.problem.c_str());
  if (!q.ok())
    return;
  q.know({"Bard_Dance_Maddening"});
  q.run_to(1.0);
  CHECK(q.trigger("Bard_Dance_Maddening", 2.0) == Refusal::None,
        "(Maddening is cast)");
  q.run_to(2.299);
  q.set_current(content::kGuidMana, 5.0);
  q.run_to(2.6);
  CHECK(q.pawn.abilities.empty() && q.pawn.cooldowns.empty() &&
            !q.pawn.ends.empty() && q.pawn.stat(content::kGuidMana) < 6.0f,
        "the cast is over at its cost step: the item is taken back, no "
        "cooldown, ClientEndAbility, nothing paid");
  CHECK(q.pawn.broken.empty(), "(the pawn took every change)");
}

void test_guard_stamina_recovery() {
  std::printf("Guard releases restore stamina recovery, including quick taps and end requests\n");
  for (int release = 0; release < 4; ++release) {
    Rig r(5, 1);
    CHECKF(r.ok(), "the Rogue rig: %s", r.problem.c_str());
    if (!r.ok())
      return;
    r.know({"General_Guard"});
    CHECK(r.trigger("General_Guard", 1.0) == Refusal::None, "Guard starts");
    const int32_t client_id = r.next_client_id - 1;
    if (release < 2) {
      CHECK(r.pipe->cancel(client_id, release == 0 ? 1.01 : 1.31), "the quick release is accepted");
    }
    r.run_to(1.5);
    CHECK(r.a7->trigger(content::kGuidDodge, 100, 1.5) == Refusal::None, "the first Dodge starts");
    r.take();
    r.run_to(2.3);
    CHECK(r.a7->trigger(content::kGuidDodge, 101, 2.3) == Refusal::None, "the second Dodge starts after its cooldown");
    r.take();
    CHECK(r.a7->stamina() == 0 && r.pawn.stat(content::kGuidStamina) == 0, "two Dodges exhaust stamina on the server and client");
    if (release == 2) {
      CHECK(r.pipe->cancel(client_id, 2.3), "the active Guard release is accepted");
    } else if (release == 3) {
      int64_t instance = -1;
      for (const auto &[id, item] : r.pawn.abilities)
        if (item.ability_guid == r.ability("General_Guard"))
          instance = item.server_instance_id;
      CHECK(instance >= 0 && r.pipe->end_request(static_cast<int32_t>(instance), 2.3), "the active Guard end request is accepted");
    }
    r.run_to(5.0);
    r.run_to(6.0);
    CHECKF(r.pawn.effect(r.effect("General_Guard")) == nullptr, "release %d leaves no Guard effect", release);
    CHECKF(near(r.pawn.stat(content::kGuidStaminaRegeneration), 5.0, 1e-6), "release %d restores the replicated recovery rate", release);
    CHECKF(r.a7->stamina() > 0 && r.pawn.stat(content::kGuidStamina) > 0, "release %d recovers from zero on the server and client", release);
    CHECK(r.pawn.broken.empty(), "every ability update applies");
  }
}

void test_summoner_completion() {
  std::printf("Summoner: completion, cancellation, and one server-owned summon hit\n");
  const char *names[] = {"Summoner_Summon_SpiritGuardian", "Summoner_Summon_SpiritHunter", "Summoner_Summon_SpiritMystic"};
  for (const char *name : names) {
    Rig r(6, 25);
    CHECK(r.ok(), "the Summoner rig loads");
    if (!r.ok())
      return;
    r.know({name});
    r.asks = false;
    const auto *d = aoc::worldd::pets::for_ability(r.ability(name));
    CHECK(d != nullptr, "the summon has a pet definition");
    if (!d)
      continue;
    CHECK(r.trigger(name, 1.0, false) == Refusal::None, "the summon cast is accepted");
    const auto *ability = r.catalog->ability(d->ability);
    CHECK(ability != nullptr && !ability->hits.empty(), "the summon has a record with a hit");
    if (!ability || ability->hits.empty())
      continue;
    const int64_t hit = ability->hits.front().hit_guid;
    CHECK(r.pipe->hit_request(d->ability, content::kTargetingDefaultSelf, hit, 1, 1, 1.2) == AbilityPipeline::HitVerdict::Quiet,
          "a premature client hit cannot complete a summon");
    r.run_to(8.9);
    CHECK(!r.pawn.effect(d->effect), "the pet effect is absent before the cast completes");
    r.run_to(9.1);
    const auto *effect = r.pawn.effect(d->effect);
    CHECK(effect != nullptr, "completion applies the summon even without a client hit request");
    const int64_t effect_id = effect ? effect->effect_id : -1;
    r.pipe->hit_request(d->ability, content::kTargetingDefaultSelf, hit, 1, 1, 9.2);
    r.run_to(10.0);
    effect = r.pawn.effect(d->effect);
    CHECK(effect && effect->effect_id == effect_id && r.pawn.broken.empty(), "a late hit cannot summon a second pet");
  }
  for (const bool move : {false, true}) {
    Rig r(6, 25);
    r.know({names[0]});
    r.asks = false;
    r.trigger(names[0], 1.0, false);
    r.run_to(1.5);
    CHECK(!r.pawn.abilities.empty(), "the summon montage is present before cancellation");
    if (r.pawn.abilities.empty())
      continue;
    const int64_t instance = r.pawn.abilities.begin()->first;
    if (move)
      r.pipe->set_moving(true);
    else
      r.pipe->end_request(static_cast<int32_t>(instance), 1.5);
    r.run_to(12.0);
    CHECK(!r.pawn.effect(aoc::worldd::pets::kDefinitions[0].effect), "a cancelled summon creates no pet effect");
    CHECK(r.pawn.broken.empty(), "cancelling keeps the replicated arrays valid");
  }
}

} // namespace

void run_ability_pipeline_costs_tests() {
  test_catalog();
  test_cast_timeline();
  test_mana_costs();
  test_cast_flood();
  test_unaffordable_at_the_cost_step();
  test_guard_stamina_recovery();
  test_summoner_completion();
}
