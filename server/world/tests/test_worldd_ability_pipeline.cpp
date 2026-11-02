// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_ability_pipeline_common.h"

using namespace ability_rig;

namespace {

int resonance_gain_for(const Rig &r, const char *name) {
  const abilities::AbilityDef *def = r.catalog->ability_named(name);
  if (!def)
    return 1;
  for (const content::AbilityHitRow &h : def->hits) {
    const abilities::HitDef *hd = r.catalog->hit(h.hit_guid);
    if (hd && hd->row.name == "Bard_AddResonance")
      return 2;
  }
  return 1;
}

bool cast_needs_self_target(const char *name) {
  const std::string s(name);
  return s == "Bard_Dance_Nimble" || s == "Bard_Misc_QuickWit_Jest" ||
         s == "Bard_Story_Joy";
}

void check_resonance_climb_and_expiry(Rig &r, int64_t resonance) {
  double t = 10.0;
  const char *casts[] = {"Bard_Dance_Maddening", "Bard_Misc_Misdirection",
                         "Bard_Dance_Nimble", "Bard_Dance_Shielding",
                         "Bard_Dance_Mesmerizing", "Bard_Misc_Flourish",
                         "Bard_Misc_Flourish", "Bard_Misc_QuickWit_Jest",
                         "Bard_Story_Joy", "Bard_Misc_GetOffTheStage"};
  int expect = 2;
  for (const char *name : casts) {
    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
    const Refusal rf = r.trigger(name, t, cast_needs_self_target(name));
    CHECKF(rf == Refusal::None, "%s accepted (%s)", name, refusal_name(rf));
    r.run_to(t + 3.0);
    expect = std::min(10, expect + resonance_gain_for(r, name));
    CHECKF(r.pawn.stat(kResonanceStat) == static_cast<float>(expect),
           "after %s Resonance is %d", name, expect);
    t += 4.0;
    r.run_to(t);
  }
  CHECK(r.pipe->stacks_of(resonance) == 10 &&
            r.pawn.stat(kResonanceStat) == 10.0f,
        "ten casts later Resonance is at its cap, 10");
  const std::optional<double> end = r.pipe->effect_end(resonance);
  CHECK(end && *end > t, "(its end is ahead)");
  if (!end)
    return;
  r.run_to(*end - 0.01);
  CHECK(r.pipe->stacks_of(resonance) == 10, "...still 10 a moment before it");
  r.run_to(*end + 0.01);
  CHECK(r.pipe->stacks_of(resonance) == 0 &&
            r.pawn.stat(kResonanceStat) == 0.0f &&
            r.pawn.effect(resonance) == nullptr,
        "...and all gone at its end: the stat is 0 again");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void test_resonance_and_fortissimo() {
  std::printf("Resonance and Fortissimo\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Misc_Fortissimo", "Bard_Dance_Maddening",
          "Bard_Misc_Misdirection", "Bard_Dance_Nimble", "Bard_Dance_Shielding",
          "Bard_Misc_Flourish", "Bard_Dance_Mesmerizing",
          "Bard_Misc_QuickWit_Jest", "Bard_Story_Joy",
          "Bard_Misc_GetOffTheStage"});
  const int64_t resonance = r.effect("Bard_Resonance");
  CHECK(r.trigger("Bard_Misc_Fortissimo", 2.0) == Refusal::Cost &&
            r.pawn.ends.size() == 1,
        "Fortissimo needs five stacks of Resonance: with none it is refused");
  CHECK(r.pipe->grant_effect(resonance, 7, 3.0) &&
            r.pipe->stacks_of(resonance) == 7,
        "(the Bard is given seven stacks)");
  r.take();
  CHECK(r.pawn.stat(kResonanceStat) == 7.0f, "(and the stat says 7)");
  std::vector<Seen> seen;
  CHECK(r.trigger("Bard_Misc_Fortissimo", 4.0, false, 0, &seen) ==
            Refusal::None,
        "with seven it is accepted");
  seen = r.run_to(4.5);
  CHECK(find_event(seen,
                   [](const Seen &s) {
                     return !s.change.abilities_added.empty();
                   }) == nullptr,
        "an ability with no montage has no ability item (retail: none)");
  const Seen *cost = find_event(
      seen, [&](const Seen &s) { return !s.change.cooldowns_set.empty(); });
  CHECK(cost && near(cost->at, 4.3, 1e-9) &&
            cost->change.cooldowns_set[0].initial_cooldown == 60.0f &&
            cost->change.cooldowns_set[0].charges == 0,
        "its Cooldowns item (60 s, no charge left) comes at 0.3 s");
  if (!cost)
    return;
  CHECK(r.pawn.stat(kResonanceStat) == 2.0f &&
            r.pipe->stacks_of(resonance) == 2 &&
            stat_of(*cost, kResonanceStat) == 2.0f,
        "it paid five stacks: Resonance 7 -> 2, and (a spend of no Mana) it "
        "gave none");
  const auto *fx = effect_changed(*cost, resonance);
  CHECK(fx && fx->stacks == 2, "the effect item changes in place to 2 stacks");
  CHECK(r.pawn.effect(r.effect("Bard_Fortissimo")) != nullptr &&
            r.pawn.effect(r.effect("Bard_Fortissimo"))->duration == 8.0f &&
            find_event(seen,
                       [&](const Seen &s) {
                         return effect_added(s, r.effect("Bard_Fortissimo")) &&
                                near(s.at, 4.3, 1e-9);
                       }),
        "Fortissimo's own effect lasts 8 s and comes in the same packet (no "
        "montage, no hit delay)");
  check_resonance_climb_and_expiry(r, resonance);
}

void check_glee_refresh_and_expiry(Rig &r, int64_t glee, double t0,
                                   double base_mit, double max_health) {
  r.run_to(t0 + 0.3 + 8.1);
  r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
  const double t2 = t0 + 10.0;
  r.run_to(t2 - 0.01);
  const double glee_end = *r.pipe->effect_end(glee);
  std::vector<Seen> seen;
  CHECK(r.trigger("Bard_Misc_QuickWit", t2, true, 0, &seen) == Refusal::None,
        "after its cooldown Quick Wit is cast again");
  seen = r.run_to(t2 + 1.0);
  const Seen *again = find_event(
      seen, [&](const Seen &s) { return effect_changed(s, glee) != nullptr; });
  const double remaining = glee_end - (t2 + 0.3 + 0.34);
  CHECK(again &&
            near(effect_changed(*again, glee)->duration, remaining + 15.0,
                 1e-3) &&
            effect_changed(*again, glee)->stacks == 1,
        "Glee, refreshed while it runs, is an in-place change of its item: the "
        "remaining time plus the full 15 s, still one stack (its in-place "
        "limit)");
  const double end = *r.pipe->effect_end(glee);
  r.run_to(end - 0.01);
  CHECK(r.pawn.effect(glee) != nullptr && r.pawn.stat(kMitigation) < base_mit,
        "Glee runs until its end");
  r.run_to(end + 0.01);
  CHECK(r.pawn.effect(glee) == nullptr &&
            near(r.pawn.stat(kMitigation), base_mit, 1e-6),
        "and at its end the item goes and Mitigation is back");
  for (double t = end + 1.0; t <= end + 200.0; t += 1.0)
    r.run_to(t);
  CHECKF(r.health() <= max_health + 1e-3 && near(r.health(), max_health, 1e-2),
         "Health never goes above the maximum and ends on it (%.3f of %.3f)",
         r.health(), max_health);
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void test_glee() {
  std::printf("Glee: heal, ticks, refresh, expiry\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Misc_QuickWit"});
  const double max_health = r.pawn.stat(content::kGuidMaxHealth);
  r.set_current(content::kGuidHealth, 200.0);
  const int64_t glee = r.effect("Bard_Glee");
  const double base_mit = r.pawn.stat(kMitigation);
  std::vector<Seen> seen;
  CHECK(r.trigger("Bard_Misc_QuickWit", 0.5, false) == Refusal::NeedsTarget &&
            r.pawn.ends.size() == 1,
        "Quick Wit needs a target: with none it is refused");
  r.run_to(0.6);
  const double t0 = 2.0;
  CHECK(r.trigger("Bard_Misc_QuickWit", t0, true, 0, &seen) == Refusal::None,
        "aimed at the caster it is accepted");
  seen = r.run_to(t0 + 0.7);
  const Seen *item = find_event(
      seen, [](const Seen &s) { return !s.change.abilities_added.empty(); });
  CHECK(item && item->change.abilities_added[0].targets_self &&
            item->change.abilities_added[0].montage_group == "DefaultGroup",
        "the ability item has the caster as its target");
  const Seen *heal = find_event(
      seen, [&](const Seen &s) { return effect_added(s, glee) != nullptr; });
  CHECK(heal && near(heal->at, t0 + 0.3 + 0.34, 1e-9),
        "Glee comes with the projectile's hit, 0.34 s after the cost step "
        "(retail: +343 ms)");
  if (!heal)
    return;
  CHECK(has_stat(*heal, content::kGuidHealth) &&
            stat_of(*heal, content::kGuidHealth) > 200.0f + 30.0f,
        "...and the same hit heals: Health rises at that instant, by the "
        "formula's amount");
  const auto *gi = effect_added(*heal, glee);
  CHECK(gi && gi->duration == 15.0f && gi->tick_time == 2.0f && gi->stacks == 1,
        "Glee: 15 s, a tick every 2 s, 1 stack");
  CHECK(stat_of(*heal, kMitigation) < base_mit &&
            near(stat_of(*heal, kMitigation), base_mit * (1.0 - 0.03), 1e-4),
        "Mitigation is multiplied by (1 - 0.03): the stat's form, the Bard "
        "holds Merriment");
  const double applied = heal->at;
  r.run_to(applied + 1.99);
  CHECK(r.pawn.effect(glee) != nullptr, "(Glee is on)");
  const double health_before_tick = r.health();
  r.run_to(applied + 2.001);
  CHECK(r.health() > health_before_tick + 8.0,
        "the first tick, 2 s after: Health rises (over and above the "
        "regeneration)");
  check_glee_refresh_and_expiry(r, glee, t0, base_mit, max_health);
}

void test_stat_forms() {
  std::printf("the engine's forms of a stat mod\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  const double healing = r.pawn.stat(kHealingMult);
  CHECK(r.pipe->grant_effect(r.effect("Bard_Note_Comedy"), 1, 1.0),
        "(Note_Comedy is given)");
  r.take();
  CHECK(near(r.pawn.stat(kHealingMult), healing * 1.05, 1e-5),
        "HealingMult is multiplied by 1.05");
  CHECK(r.pipe->grant_effect(r.effect("Bard_Pep"), 1, 1.5), "(Pep is given)");
  r.take();
  CHECK(near(r.pawn.stat(kMoveSpeedMult), 1.2, 1e-6),
        "MoveSpeedMult is 1 + 0.2");
  CHECK(r.pipe->remove_effect(r.effect("Bard_Note_Comedy"), 2.0),
        "(Note_Comedy is taken off)");
  r.take();
  CHECK(near(r.pawn.stat(kHealingMult), healing, 1e-6),
        "taking the effect off puts the stat back");
  CHECK(!r.pipe->remove_effect(r.effect("Bard_Note_Comedy"), 2.1),
        "(an effect that is not on is not taken off)");
  CHECK(!r.pipe->grant_effect(123456789, 1, 2.2),
        "(an effect the catalog does not have cannot be given)");
}

void check_song_stop_replace_and_air(Rig &r, int64_t self_effect, int64_t buff,
                                     double mana) {
  std::vector<Seen> seen;
  const double mana_now = r.pawn.stat(content::kGuidMana);
  CHECK(r.trigger("Bard_Music_Cheerful", 9.0, false, 3, &seen) == Refusal::None,
        "pressing the song again is accepted");
  seen = r.run_to(9.4);
  CHECK(r.pawn.effect(self_effect) == nullptr &&
            r.pawn.effect(buff) == nullptr && r.pawn.cooldowns.empty() &&
            r.pawn.stat(content::kGuidMana) >= mana_now,
        "...and ends it: its effects go, no cost, no cooldown");
  r.trigger("Bard_Music_Cheerful", 10.0, false, 2, &seen);
  r.run_to(12.5);
  CHECK(r.pawn.effect(self_effect) != nullptr, "(Cheerful plays again)");
  r.trigger("Bard_Music_Pensive", 13.0, false, 2, &seen);
  r.run_to(15.5);
  CHECK(r.pawn.effect(self_effect) == nullptr &&
            r.pawn.effect(r.effect("Bard_Music_Pensive_Self")) != nullptr,
        "a second song replaces the first: they share the Bard_Music tag");
  const double t1 = 30.0;
  CHECK(r.trigger("Bard_Dance_Maddening", t1, false, 0, &seen) == Refusal::None,
        "Maddening, cast at the air, is accepted");
  seen = r.run_to(t1 + 3.0);
  const Seen *cost = find_event(
      seen, [&](const Seen &s) { return !s.change.cooldowns_set.empty(); });
  CHECK(cost && cost->change.cooldowns_set[0].initial_cooldown == 25.0f &&
            mana - 20 * kManaMult25 < r.pawn.stat(content::kGuidMana) + 80.0,
        "it spends 113.75 Mana and starts its 25 s cooldown");
  CHECK(find_event(seen,
                   [&](const Seen &s) {
                     return !s.change.abilities_added.empty() &&
                            s.change.abilities_added[0].montage_guid ==
                                9016927598177880665ull;
                   }) != nullptr,
        "and plays its montage (the NetGUID of the retail capture)");
  TriggerInfo none;
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_QuickWit_Mock"), 99, 40.0, none) ==
            Refusal::None,
        "Mock, with no enemy to aim at, is cast at the air");
  seen = r.run_to(41.0);
  const Seen *mock = find_event(
      seen, [&](const Seen &s) { return !s.change.cooldowns_set.empty(); });
  CHECK(mock && mock->change.cooldowns_set[0].ability_guid ==
                    r.ability("Bard_Misc_QuickWit_Mock"),
        "it spends its cost and starts its cooldown, and nothing is hit");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

int count_effect_readds_in(const auto &effects, int64_t buff) {
  int n = 0;
  for (const auto &e : effects) {
    if (e.effect_guid == buff) {
      ++n;
    }
  }
  return n;
}

int count_effect_readds(const std::vector<Seen> &seen, int64_t buff) {
  int readds = 0;
  for (const Seen &s : seen) {
    readds += count_effect_readds_in(s.change.effects_added, buff) +
              count_effect_readds_in(s.change.effects_changed, buff);
  }
  return readds;
}

std::vector<float> collect_effect_hot_values(const std::vector<Seen> &log,
                                             int64_t buff) {
  std::vector<float> hot;
  for (const Seen &s : log)
    for (const auto &h : s.confirmations)
      if (h.source_type == content::kSourceTypeEffect &&
          h.source_guid == buff && !h.mods.empty())
        hot.push_back(h.mods[0].value);
  return hot;
}

void check_song_buff_readds_and_hot(Rig &r, int64_t self_effect, int64_t buff,
                                    double mana) {
  const double first_end = *r.pipe->effect_end(buff);
  std::vector<Seen> seen = r.run_to(8.4);
  const int readds = count_effect_readds(seen, buff);
  CHECKF(readds >= 2 && *r.pipe->effect_end(buff) > first_end,
         "the buff is added again every 2 s (%d times in 4 s)", readds);
  const std::vector<float> hot = collect_effect_hot_values(r.log, buff);
  CHECKF(hot.size() >= 2 && hot.back() > hot.front(),
         "the buff's heal ticks are confirmed, and the amount rises with the "
         "song's progress (%zu ticks)",
         hot.size());
  check_song_stop_replace_and_air(r, self_effect, buff, mana);
}

void test_songs_and_the_air() {
  std::printf("a song, and an ability cast at the air\n");
  std::vector<std::string> song_talents = kTalents;
  song_talents.push_back("Bard_Passive_Music_Crescendo");
  Rig r(2, 25, song_talents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Music_Cheerful", "Bard_Music_Pensive", "Bard_Dance_Maddening",
          "Bard_Misc_QuickWit_Mock"});
  const double mana = r.pawn.stat(content::kGuidMana);
  const int64_t self_effect = r.effect("Bard_Music_Cheerful_Self"),
                buff = r.effect("Bard_Music_Cheerful_Buff");
  std::vector<Seen> seen;
  CHECK(r.trigger("Bard_Music_Cheerful", 2.0, false, 2, &seen) == Refusal::None,
        "a song (the client's AnimationIndex 2) is accepted");
  seen = r.run_to(2.31);
  const Seen *item = find_event(
      seen, [](const Seen &s) { return !s.change.abilities_added.empty(); });
  CHECK(item && item->change.abilities_added[0].montage_group == "SongGroup" &&
            item->change.abilities_added[0].montage_guid ==
                5020857450605277129ull &&
            near(item->at, 2.3, 1e-9),
        "its item comes at 0.3 s: MontageGroup SongGroup and the Cheerful "
        "montage of the captures");
  CHECK(r.pawn.cooldowns.empty() &&
            near(r.pawn.stat(content::kGuidMana), mana, 1e-3) &&
            r.pawn.effect(self_effect) == nullptr,
        "no cooldown, no cost, and none of its effects yet");
  seen = r.run_to(4.4);
  const Seen *start = find_event(seen, [&](const Seen &s) {
    return effect_added(s, self_effect) != nullptr;
  });
  CHECK(start && near(start->at, 4.3, 1e-9) &&
            effect_added(*start, self_effect)->duration > 1e30f &&
            effect_added(*start, self_effect)->tick_time == 2.0f &&
            effect_added(*start, buff) &&
            effect_added(*start, buff)->tick_time == 2.0f,
        "2.3 s on: the song's own effect (no end, a tick every 2 s) and its "
        "Buff (a tick every 2 s) are on");
  check_song_buff_readds_and_hot(r, self_effect, buff, mana);
}

void check_confirmations_missing_health(Rig &r, double t0, float amount) {
  std::vector<Seen> seen;
  r.run_to(t0 + 9.0);
  r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
  const double max_health = r.pawn.stat(content::kGuidMaxHealth);
  r.set_current(content::kGuidHealth, max_health - 100.0);
  const double t1 = t0 + 10.0;
  CHECK(r.trigger("Bard_Misc_QuickWit", t1, true, 0, &seen) == Refusal::None,
        "(cast again with 100 Health missing)");
  seen = r.run_to(t1 + 0.7);
  const bootstrap::HitConfirm *part = nullptr;
  for (const Seen &s : seen)
    for (const auto &h : s.confirmations)
      if (h.source_type == content::kSourceTypeProjectile && !h.mods.empty())
        part = &h;
  CHECK(part && near(part->mods[0].value, amount, 0.01) &&
            part->mods[0].value > 10.0f &&
            near(part->mods[0].effective,
                 std::min<double>(100.0, part->mods[0].value), 0.5),
        "the amount is the same and the effective value is what fitted of the "
        "100 missing");
}

const bootstrap::HitConfirm *
find_projectile_heal_confirm(const std::vector<Seen> &seen) {
  const bootstrap::HitConfirm *heal = nullptr;
  for (const Seen &s : seen)
    for (const auto &h : s.confirmations)
      if (h.source_type == content::kSourceTypeProjectile && !h.mods.empty() &&
          h.mods[0].value > 0)
        heal = &h;
  return heal;
}

void test_confirmations() {
  std::printf(
      "hit confirmations: the amounts the client draws as floating numbers\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Misc_QuickWit"});
  std::vector<Seen> seen;
  const double t0 = 2.0;
  CHECK(r.trigger("Bard_Misc_QuickWit", t0, true, 0, &seen) == Refusal::None,
        "(Quick Wit is cast at the caster, at full Health)");
  seen = r.run_to(t0 + 0.7);
  size_t confirmed = 0;
  for (const Seen &s : seen)
    confirmed += s.confirmations.size();
  CHECKF(
      confirmed >= 1,
      "the projectile's hit is confirmed by the server (%zu confirmation(s))",
      confirmed);
  const bootstrap::HitConfirm *heal = find_projectile_heal_confirm(seen);
  CHECK(heal != nullptr, "one of them carries a heal's stat mod");
  if (!heal)
    return;
  CHECK(heal->source_type == content::kSourceTypeProjectile &&
            !heal->source_is_actor && !heal->response &&
            heal->predicted_autonomous && heal->predicted_simulated &&
            !heal->no_target,
        "its source is the projectile record, with no Source actor, answering "
        "no request [R: 3,529 projectile hits, source (0,0,0,0)]");
  CHECKF(heal->mods[0].value > 10.0f && heal->mods[0].effective == 0.0f,
         "at full Health the amount is the formula's (%.2f) and the effective "
         "value is 0: the overheal a retail client shows",
         heal->mods[0].value);
  check_confirmations_missing_health(r, t0, heal->mods[0].value);
}

void check_shielding_damage_and_expiry(Rig &r, int64_t temp_hp, double ask,
                                       float shield) {
  const double health = r.health();
  const double through = r.pipe->take_damage(shield * 0.5, ask + 1.1);
  r.take();
  CHECK(near(through, 0.0, 1e-9) && near(r.health(), health, 1e-3) &&
            r.pawn.shields.size() == 1 &&
            near(r.pawn.shields[0].value, shield * 0.5, 0.01) &&
            r.pawn.shields[0].base == shield,
        "half the shield's amount in damage: Health is untouched, the shield "
        "holds the other half");
  const double through2 = r.pipe->take_damage(shield, ask + 1.2);
  r.take();
  CHECK(near(through2, shield * 0.5, 0.01) &&
            near(r.health(), health - shield * 0.5, 0.05) &&
            r.pawn.shields[0].value == 0.0f,
        "the rest of a larger blow reaches Health once the shield is spent");
  const double end = *r.pipe->effect_end(temp_hp);
  r.run_to(end - 0.01);
  CHECK(r.pawn.effect(temp_hp) != nullptr && r.pawn.shields.size() == 1,
        "the shield lasts until its effect ends");
  r.run_to(end + 0.01);
  CHECK(r.pawn.effect(temp_hp) == nullptr && r.pawn.shields.empty(),
        "when TempHP ends its element leaves the array");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void check_shielding_tick_and_absorb(Rig &r, int64_t temp_hp, double t0,
                                     float shield) {
  std::vector<Seen> seen;
  const double ask = t0 + 0.45;
  const auto verdict =
      r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick",
                    ask, true, &seen);
  CHECK(verdict == AbilityPipeline::HitVerdict::Scheduled,
        "the client's request for LingerTick (aimed by the Bard_Dance_AOE "
        "targeting) is taken");
  seen = r.run_to(ask + 0.5);
  const Seen *applied =
      find_event(seen, [](const Seen &s) { return !s.confirmations.empty(); });
  CHECK(applied != nullptr, "the hit is applied and confirmed");
  if (!applied)
    return;
  CHECKF(
      near(applied->at,
           std::max(ask + AbilityPipeline::kConfirmDelay,
                    t0 + 0.3 + AbilityPipeline::kHitDelay),
           1e-9),
      "...kConfirmDelay after the request, never before the cost step (%.3f)",
      applied->at - t0);
  const auto &conf = applied->confirmations[0];
  CHECK(conf.source_type == content::kSourceTypeAbility &&
            conf.source_is_actor && conf.response && conf.mods.size() == 1 &&
            conf.mods[0].value > 0 && !conf.no_target,
        "the confirmation names the ability as its source, answers the request "
        "and carries the hit's own Health mod (a heal)");
  CHECKF(conf.mods[0].effective == 0.0f &&
             near(conf.mods[0].value * 6.0, shield, 0.05),
         "at full Health: effective 0; the heal is 0.5 x the modifier and the "
         "shield 3.0 x (%.2f x 6 = %.2f)",
         conf.mods[0].value, shield);
  CHECK(!applied->change.shields && r.pawn.shields.size() == 1 &&
            r.pawn.shields[0].value == shield,
        "the tick adds no shield (the 4 s internal cooldown is on)");
  check_shielding_damage_and_expiry(r, temp_hp, ask, shield);
}

void test_shielding_dance() {
  std::printf("Shielding Dance: the shield, the request, the confirmation, "
              "absorb, expiry\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.asks = false;
  r.know({"Bard_Dance_Shielding"});
  const int64_t temp_hp = r.effect("Bard_Dance_Shielding_TempHP"),
                icd = r.effect("Bard_Dance_Shielding_ICD");
  std::vector<Seen> seen;
  const double t0 = 2.0;
  CHECK(r.trigger("Bard_Dance_Shielding", t0, false, 0, &seen) == Refusal::None,
        "Shielding is accepted");
  seen = r.run_to(t0 + 0.4);
  const Seen *cost = find_event(
      seen, [&](const Seen &s) { return effect_added(s, temp_hp) != nullptr; });
  CHECK(cost && effect_added(*cost, temp_hp)->duration == 8.0f &&
            effect_added(*cost, icd) &&
            effect_added(*cost, icd)->duration == 4.0f,
        "the TempHP effect (8 s) and the internal cooldown (4 s) are on the "
        "pawn");
  CHECK(cost && cost->change.shields && cost->change.shields->size() == 1,
        "the TempHealth array's item lists one shield");
  const float shield =
      cost && cost->change.shields && !cost->change.shields->empty()
          ? (*cost->change.shields)[0].value
          : 0.0f;
  CHECK(shield > 30.0f && (*cost->change.shields)[0].base == shield,
        "its amount is the formula's (3.0 x the healing modifier at level 25): "
        "Base = Value");
  CHECKF(r.pawn.shields.size() == 1 && r.pawn.shields[0].value == shield,
         "the client's array holds it (%.2f)", shield);
  CHECK(r.pawn.confirmations.empty(),
        "(no confirmation yet: the cast's Default_Self hits and the overlap's "
        "effects are never confirmed)");
  check_shielding_tick_and_absorb(r, temp_hp, t0, shield);
}

void check_hit_requests_no_zone_and_self() {
  using V = AbilityPipeline::HitVerdict;
  {
    Rig rf(2, 25, kTalents);
    rf.asks = false;
    rf.know({"Bard_Misc_Flourish"});
    rf.trigger("Bard_Misc_Flourish", 2.0);
    rf.run_to(2.4);
    size_t flourish_taken = 0;
    for (int i = 0; i < 30; ++i)
      flourish_taken +=
          rf.hit_request("Bard_Misc_Flourish", "Bard_Flourish_Self",
                         2.5 + 0.2 * i) == V::Scheduled
              ? 1
              : 0;
    CHECKF(flourish_taken ==
               static_cast<size_t>(AbilityPipeline::kMaxRequestsPerHit),
           "a cast with no zone takes %d requests of one hit (%zu)",
           AbilityPipeline::kMaxRequestsPerHit, flourish_taken);
  }
  Rig r3(2, 25, kTalents);
  r3.asks = false;
  r3.know({"Bard_Dance_Shielding"});
  r3.trigger("Bard_Dance_Shielding", 2.0);
  r3.run_to(2.4);
  CHECK(r3.hit_request("Bard_Dance_Shielding",
                       "Bard_Dance_Shielding_LingerTick", 2.5,
                       false) == V::Empty,
        "a request that names none of the caster's components found nobody");
  r3.run_to(3.0);
  CHECK(r3.pawn.confirmations.size() == 1 && r3.pawn.confirmations[0].no_target,
        "it is answered by a confirmation with no target");
  Rig r2(2, 25, kTalents);
  r2.asks = false;
  r2.know({"Bard_Misc_Flourish"});
  r2.trigger("Bard_Misc_Flourish", 2.0);
  r2.run_to(2.1);
  CHECK(r2.pawn.effect(r2.effect("Bard_Pep")) == nullptr,
        "(no request, no hit: the client picks the hits its state needs)");
  CHECK(r2.hit_request("Bard_Misc_Flourish", "Bard_Flourish_Self", 2.15) ==
            V::Scheduled,
        "the request for the Default_Self hit is taken");
  r2.run_to(4.0);
  CHECK(r2.pawn.confirmations.empty() &&
            r2.pawn.effect(r2.effect("Bard_Pep")) != nullptr,
        "...Pep is on, and nothing is confirmed");
}

static void check_hit_requests_zone_cap_and_shields(
    Rig &r, const abilities::AbilityDef *shielding_def) {
  using V = AbilityPipeline::HitVerdict;
  size_t taken = 1;
  for (int i = 0; i < 40; ++i)
    taken +=
        r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick",
                      3.2 + 0.2 * i) == V::Scheduled
            ? 1
            : 0;
  const content::ZoneRow *zone = r.catalog->zone(
      shielding_def->zones.empty() ? 0 : shielding_def->zones[0]);
  CHECK(zone != nullptr && zone->life_time == 30.0,
        "the Shielding Dance has its zone (30 s)");
  const size_t cap =
      static_cast<size_t>(
          zone ? static_cast<int>(zone->life_time / zone->tick_rate) : 0) +
      AbilityPipeline::kZoneExtraRequests;
  CHECKF(taken <= cap && taken >= cap - 1,
         "a cast with a zone takes the zone's ticks and a few more requests of "
         "one hit, and no more (%zu of at most %zu)",
         taken, cap);
  r.run_to(40.0);
  size_t shields_added = 0;
  for (const Seen &s : r.log)
    shields_added += s.change.shields && !s.change.shields->empty() ? 1 : 0;
  CHECKF(shields_added <= 3,
         "the internal cooldown keeps the repeated requests from stacking "
         "shields (%zu shield updates)",
         shields_added);
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void test_hit_requests_hostile() {
  std::printf("hit requests: forged, repeated, for nothing\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.asks = false;
  r.know({"Bard_Dance_Shielding", "Bard_Misc_Flourish"});
  using V = AbilityPipeline::HitVerdict;
  CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick",
                      1.0) == V::NoCast,
        "a request for an ability that was not cast is dropped");
  r.trigger("Bard_Dance_Shielding", 2.0);
  r.run_to(2.5);
  const abilities::AbilityDef *shielding_def =
      r.catalog->ability_named("Bard_Dance_Shielding");
  const abilities::AbilityDef *flourish =
      r.catalog->ability_named("Bard_Misc_Flourish");
  const int64_t shielding = shielding_def->row.guid;
  int64_t tick = 0;
  for (const auto &h : shielding_def->hits)
    if (r.catalog->hit(h.hit_guid)->row.name ==
        "Bard_Dance_Shielding_LingerTick")
      tick = h.hit_guid;
  (void)tick;
  CHECK(r.pipe->hit_request(shielding, content::kTargetingDefaultSelf, 12345, 1,
                            1, 2.6) == V::NotItsHit,
        "a hit that is none of the ability's is dropped");
  CHECK(r.pipe->hit_request(flourish->row.guid, content::kTargetingDefaultSelf,
                            flourish->hits[0].hit_guid, 1, 1, 2.8) == V::NoCast,
        "the cast of another ability is not open");
  CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick",
                      3.0) == V::Scheduled,
        "the real request is taken");
  CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick",
                      3.01) == V::TooMany,
        "a second one 10 ms later is too fast");
  check_hit_requests_zone_cap_and_shields(r, shielding_def);
  check_hit_requests_no_zone_and_self();
}

void check_refusals_in_progress_and_cost(Rig &r) {
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 50, 2.0, {}) ==
            Refusal::None,
        "(Flourish is cast)");
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Fortissimo"), 51, 2.1, {}) ==
            Refusal::Cost,
        "another ability during its cast is taken (retail's combos re-trigger "
        "within a montage): here it lacks the stacks");
  r.take();
  r.run_to(2.4);
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Fortissimo"), 52, 2.5, {}) ==
            Refusal::Cost,
        "(after the cost step it can be tried: Fortissimo then lacks the "
        "stacks)");
  r.take();
  r.run_to(3.0);
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 53, 3.0, {}) ==
            Refusal::None,
        "(Flourish again: its second charge)");
  r.run_to(3.5);
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 54, 4.0, {}) ==
            Refusal::Charges,
        "a third: no charge left");
  r.run_to(60.0);
  r.set_current(content::kGuidMana, 10.0);
  const Pawn poor = r.pawn;
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 55, 60.5, {}) ==
            Refusal::Cost,
        "with 10 Mana Flourish cannot be paid for");
  r.take();
  CHECK(near(r.pawn.stat(content::kGuidMana), poor.stat(content::kGuidMana),
             1e-3) &&
            r.pawn.cooldowns.size() == poor.cooldowns.size(),
        "nothing was spent and no cooldown was started");
  CHECK(r.pipe->refused() >= 9 && r.pawn.broken.empty(),
        "(the refusals are counted, the pawn took every change)");
}

void test_refusals() {
  std::printf("refusals\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Misc_Flourish", "Bard_Misc_QuickWit_Jest",
          "Bard_Misc_QuickWit_Mock", "Bard_Misc_Fortissimo", "Ranger_Dash"});
  const Pawn before = r.pawn;
  auto unchanged = [&](const char *what) {
    CHECKF(r.pawn.stats == before.stats &&
               r.pawn.effects.size() == before.effects.size() &&
               r.pawn.abilities.size() == before.abilities.size() &&
               r.pawn.cooldowns.size() == before.cooldowns.size(),
           "%s: nothing changed on the pawn", what);
  };
  TriggerInfo info;
  CHECK(r.pipe->trigger(987654321, 40, 1.0, info) == Refusal::NotEnabled,
        "an ability that is no row: not enabled");
  CHECK(r.pipe->trigger(r.ability("Bard_Dance_Maddening"), 41, 1.1, info) ==
            Refusal::NotKnown,
        "a real ability that is not known");
  info.item = true;
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 42, 1.2, info) ==
            Refusal::Unsupported,
        "an ability named with an item is not run here");
  info = {};
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_QuickWit_Jest"), 43, 1.3, info) ==
            Refusal::NeedsTarget,
        "an ability that needs a target and can be aimed at a friend is "
        "refused when the call names none");
  const Refusal dart = r.pipe->trigger(r.ability("Ranger_Dash"), 44, 1.4, info);
  CHECKF(
      dart == Refusal::NoFormula,
      "an ability whose activation requirement does not compile (Ranger Dash: "
      "GetLastProjectileFireTime) is refused (logged), never run half-way (%s)",
      refusal_name(dart));
  r.take();
  CHECK(r.pawn.ends == (std::vector<int32_t>{40, 41, 42, 43, 44}),
        "each of them is answered with ClientEndAbility(its ClientInstanceId), "
        "in order");
  unchanged("the refusals");
  const size_t ends = r.pawn.ends.size();
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 40, 1.5, {}) ==
            Refusal::Duplicate,
        "a ClientInstanceId the session has used is a replay: dropped");
  r.take();
  CHECK(r.pawn.ends.size() == ends, "...with no ClientEndAbility (it would end "
                                    "the real instance of that id)");
  check_refusals_in_progress_and_cost(r);
}

} // namespace

int main() {
  if (!real_db_present()) {
    std::printf("no content.db at %s (cd services && go run ./cmd/contentc): "
                "the generic ability runtime's tests are skipped\n",
                real_db_path().c_str());
    return 0;
  }
  test_catalog();
  test_cast_timeline();
  test_mana_costs();
  test_resonance_and_fortissimo();
  test_glee();
  test_stat_forms();
  test_songs_and_the_air();
  test_confirmations();
  test_shielding_dance();
  test_hit_requests_hostile();
  test_refusals();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
