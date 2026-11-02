// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_ability_pipeline_common.h"

using namespace ability_rig;

namespace {

void test_time() {
  std::printf("the clock\n");
  auto run = [](bool jump) {
    Rig r(2, 25, kTalents);
    r.know({"Bard_Misc_QuickWit"});
    r.set_current(content::kGuidHealth, 300.0);
    r.trigger("Bard_Misc_QuickWit", 2.0, true);
    if (jump) {
      r.run_to(5.5);
    } else {
      for (int i = 1; i <= 550; ++i)
        r.run_to(i * 0.01);
    }
    return r;
  };
  Rig a = run(true), b = run(false);
  CHECKF(a.log.size() == b.log.size(),
         "one 5.5 s step gives as many events as 550 steps of 10 ms (%zu, %zu)",
         a.log.size(), b.log.size());
  bool same = a.log.size() == b.log.size();
  for (size_t i = 0; same && i < a.log.size(); ++i) {
    same = near(a.log[i].at, b.log[i].at, 1e-9) &&
           a.log[i].note == b.log[i].note &&
           a.log[i].change.stats.size() == b.log[i].change.stats.size();
  }
  CHECK(same, "...the same events at the same times");
  CHECK(a.pawn.stats == b.pawn.stats &&
            a.pawn.effects.size() == b.pawn.effects.size(),
        "and the same client state");
  Rig r(2, 25, kTalents);
  r.know({"Bard_Misc_Flourish"});
  r.run_to(10.0);
  CHECK(r.pipe->trigger(r.ability("Bard_Misc_Flourish"), 9, 5.0, {}) ==
                Refusal::None &&
            near(r.pipe->now(), 10.0, 1e-12),
        "a trigger with an earlier clock is handled at the later one");
  r.run_to(3.0);
  CHECK(near(r.pipe->now(), 10.0, 1e-12), "and the clock does not go back");
}

void check_regen_rest_and_stall(Rig &r, double regen_m) {
  r.set_current(content::kGuidMana, 100.0);
  r.a7->trigger(content::kGuidRest, 7, 8.5);
  r.take();
  r.run_to(8.5 + 2.1);
  const double boosted = r.pawn.stat(content::kGuidManaRegeneration);
  CHECK(boosted > regen_m + 1.0, "(the Rest's effect raised ManaRegeneration)");
  const double mana_then = r.mana();
  r.run_to(8.5 + 3.2);
  CHECK(near(r.mana() - mana_then,
             boosted * (r.mana() - mana_then > 0
                            ? std::round((r.mana() - mana_then) / boosted)
                            : 1),
             0.05) &&
            r.mana() > mana_then + boosted - 0.1,
        "the next tick adds the boosted amount");
  Rig s(2, 25, kTalents);
  s.set_current(content::kGuidMana, 100.0);
  const double regen = s.a7->stat(content::kGuidManaRegeneration);
  s.run_to(1000.0);
  CHECK(near(s.mana(), 100.0 + 5 * regen, 0.05),
        "a clock that jumps 1,000 s makes up for five ticks, no more");
  CHECK(r.pawn.broken.empty() && s.pawn.broken.empty(),
        "(the pawns took every change)");
}

void test_regeneration() {
  std::printf("regeneration\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  const double regen_h = r.a7->stat(content::kGuidHealthRegeneration),
               regen_m = r.a7->stat(content::kGuidManaRegeneration);
  CHECK(regen_h > 0 && regen_m > 0 && near(regen_m, 7.4599, 1e-3),
        "(the engine's out-of-combat Mana regeneration of a level-25 Bard with "
        "no gear: 5 x 1.492)");
  r.set_current(content::kGuidHealth, 100.0);
  r.set_current(content::kGuidMana, 100.0);
  std::vector<Seen> seen = r.run_to(1.0);
  const Seen *first = find_event(seen, [](const Seen &s) {
    return s.note.find("regenerate") != std::string::npos;
  });
  CHECK(first && near(first->at, 1.0, 1e-9) &&
            near(stat_of(*first, content::kGuidHealth),
                 static_cast<float>(100.0f + static_cast<float>(regen_h)),
                 1e-3) &&
            near(stat_of(*first, content::kGuidMana),
                 static_cast<float>(100.0f + static_cast<float>(regen_m)),
                 1e-3),
        "after one second Health and Mana gain their regeneration stat, in one "
        "change");
  seen = r.run_to(5.0);
  CHECK(near(r.health(), 100.0 + 5 * regen_h, 0.05) &&
            near(r.mana(), 100.0 + 5 * regen_m, 0.05),
        "five seconds: five steps");
  const double max_mana = r.pawn.stat(content::kGuidMaxMana);
  r.set_current(content::kGuidMana, max_mana - 3.0);
  r.run_to(6.0);
  CHECK(r.mana() == max_mana, "a step that would pass the maximum stops on it");
  const size_t events = r.log.size();
  r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
  r.run_to(8.0);
  CHECK(r.log.size() == events && r.pipe->ticks_regen() >= 6,
        "at the maximum nothing is sent");
  check_regen_rest_and_stall(r, regen_m);
}

void test_movement() {
  std::printf("movement abilities\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Misc_Flourish", "Mage_Blink"});
  CHECK(r.pipe->movement_allowance(1.0) == 0, "no allowance before a cast");
  CHECK(r.trigger("Bard_Misc_Flourish", 2.0) == Refusal::None,
        "(Flourish is cast)");
  CHECK(near(r.pipe->movement_allowance(2.0), 3659.0 * 1.5, 1e-6) &&
            near(r.pipe->movement_allowance(2.99), 3659.0 * 1.5, 1e-6),
        "Flourish's slide (3,659 cm/s in both captured casts) is allowed 1.5 x "
        "that for 1 s");
  CHECK(r.pipe->movement_allowance(3.01) == 0,
        "and the window is over after it");
  const abilities::AbilityDef *blink = r.catalog->ability_named("Mage_Blink");
  CHECK(blink && blink->row.networking_max_speed == 7000.0,
        "(Blink's data says 7,000 cm/s)");
  r.run_to(10.0);
  const Refusal rf = r.trigger("Mage_Blink", 11.0);
  if (rf == Refusal::None) {
    CHECK(near(r.pipe->movement_allowance(11.5), 7000.0 * 1.5, 1e-6),
          "Blink's window allows 1.5 x its NetworkingMaxSpeed");
  } else {
    std::printf(
        "  (Blink is %s for this character: its window is not tested)\n",
        refusal_name(rf));
  }
}

void test_a7_beside() {
  std::printf("Sprint, Dodge and the generic runtime\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Misc_Flourish", "Bard_Misc_QuickWit"});
  r.a7->set_moving(true, 0.5);
  r.a7->trigger(content::kGuidSprint, 100, 1.0);
  r.take();
  CHECK(near(r.pawn.stat(kMoveSpeedMult), 1.3, 1e-6),
        "a level-25 Bard sprints at MoveSpeedMult 1.3 (retail's), not 1.2");
  CHECK(r.trigger("Bard_Misc_Flourish", 2.0) == Refusal::None,
        "(Flourish is cast while sprinting)");
  r.run_to(2.4);
  CHECK(near(r.pawn.stat(kMoveSpeedMult), 1.5, 1e-6),
        "Pep adds its 0.2 to the Sprint's 0.3: MoveSpeedMult 1.5 (the retail "
        "capture, +940 ms)");
  r.run_to(2.34 + 4.01);
  CHECK(near(r.pawn.stat(kMoveSpeedMult), 1.3, 1e-6),
        "Pep ends: back to the Sprint's 1.3");
  r.a7->trigger(content::kGuidDodge, 101, 7.0);
  r.take();
  CHECK(r.pawn.cooldown(content::kGuidDodge) != nullptr &&
            r.pawn.cooldown(r.ability("Bard_Misc_Flourish")) != nullptr,
        "a Dodge's Cooldowns item is added beside Flourish's: both are on the "
        "pawn");
  CHECK(r.trigger("Bard_Misc_QuickWit", 8.0, true) == Refusal::None,
        "(Quick Wit is cast)");
  r.run_to(9.5);
  CHECK(r.pawn.cooldowns.size() == 3 || r.pawn.cooldowns.size() == 2,
        "the three cooldowns are listed (or two once the Dodge's 0.65 s is "
        "over)");
  CHECK(r.pawn.broken.empty(), "the ability items, effects and cooldowns of "
                               "both runtimes never share a key");
  r.a7->cancel(100, 10.0);
  r.run_to(14.0);
  CHECKF(r.pawn.stat(kMoveSpeedMult) == 1.0f && r.pawn.broken.empty(),
         "the Sprint ends: MoveSpeedMult is 1 again (%.4f)",
         r.pawn.stat(kMoveSpeedMult));
  Rig low(2, 1, {});
  CHECKF(low.ok(), "the level-1 rig: %s", low.problem.c_str());
  if (!low.ok())
    return;
  low.a7->set_moving(true, 0.5);
  low.a7->trigger(content::kGuidSprint, 1, 1.0);
  low.take();
  CHECK(near(low.pawn.stat(kMoveSpeedMult), 1.2, 1e-6),
        "a level-1 Bard sprints at 1.2");
}

void test_class_resources() {
  std::printf(
      "class resources are held stats that move by their regeneration\n");
  constexpr int64_t kFocus = 6064633175093543338ll;
  Rig r(1, 25);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  const stats::StatValue *focus = r.cs->find(kFocus);
  CHECK(focus != nullptr && focus->resource(),
        "a Ranger's Focus is a held resource stat");
  if (!focus)
    return;
  r.set_current(kFocus, 10.0);
  r.run_to(1.0);
  const double after_1 = r.pawn.stat(kFocus);
  CHECK(near(after_1, 13.0, 0.01),
        "a second on, Focus has moved by its regeneration (+3)");
  r.run_to(4.0);
  CHECK(near(r.pawn.stat(kFocus), 22.0, 0.01),
        "...and keeps moving, one step a second");
  r.set_current(kFocus, r.pawn.stat(kFocus) > 0 ? 99.0 : 0.0);
  r.run_to(6.0);
  CHECK(r.pawn.stat(kFocus) <= 100.0 && r.pawn.stat(kFocus) >= 99.0,
        "a step stops on the stat's maximum");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
  Rig c(3, 25);
  CHECKF(c.ok(), "the Cleric rig: %s", c.problem.c_str());
  if (!c.ok())
    return;
  constexpr int64_t kPower = 6064629623747772676ll;
  const stats::StatValue *power = c.cs->find(kPower);
  CHECK(power != nullptr && power->resource(),
        "a Cleric's DivinePower is a held resource stat");
  c.set_current(kPower, 40.0);
  c.run_to(3.0);
  const double kept = c.pawn.stat(kPower);
  CHECK(kept >= 0.0 && kept <= 100.0 && c.pawn.broken.empty(),
        "the value that was set survives the engine's recomputation (it is "
        "held, not derived)");
}

void test_mend_chain() {
  std::printf("Mend: one projectile per cast, chosen by the charges used\n");
  Rig r(3, 25);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Cleric_Mend"});
  std::vector<Seen> seen;
  size_t confirmed_first = 0;
  CHECK(r.trigger("Cleric_Mend", 2.0, true, 0, &seen) == Refusal::None,
        "Mend, cast at an ally, is accepted");
  seen = r.run_to(4.0);
  for (const Seen &s : seen)
    confirmed_first += s.confirmations.size();
  const Seen *cost = find_event(
      seen, [](const Seen &s) { return !s.change.cooldowns_set.empty(); });
  CHECK(cost != nullptr, "it pays its cost and starts its cooldown");
  CHECK(confirmed_first >= 1 && r.pawn.broken.empty(),
        "its hit is confirmed, and the pawn took every change");
  CHECK(r.trigger("Cleric_Mend", 4.5, true, 0, &seen) == Refusal::None,
        "a second Mend, with a charge left, is accepted");
  seen = r.run_to(6.5);
  size_t confirmed_second = 0;
  for (const Seen &s : seen)
    confirmed_second += s.confirmations.size();
  CHECK(confirmed_second >= 1, "the second cast's hit is confirmed as well "
                               "(the chain goes on with the charges used)");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void test_end_request() {
  std::printf("the client's ServerEndAbility\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Music_Cheerful"});
  std::vector<Seen> seen;
  CHECK(r.trigger("Bard_Music_Cheerful", 2.0, false, 2, &seen) == Refusal::None,
        "(a song is cast)");
  seen = r.run_to(2.5);
  int64_t instance = -1;
  for (const auto &[k, v] : r.pawn.abilities)
    if (v.ability_guid == r.ability("Bard_Music_Cheerful"))
      instance = static_cast<int64_t>(k);
  CHECK(instance >= 0, "(its item is on the pawn)");
  CHECK(!r.pipe->end_request(static_cast<int32_t>(instance) + 1000, 3.0),
        "an instance that is not ours is not ended");
  CHECK(r.pipe->end_request(static_cast<int32_t>(instance), 3.0),
        "the instance the client names is ended...");
  CHECK(!r.pipe->end_request(static_cast<int32_t>(instance), 3.01), "...once");
  seen = r.run_to(3.2);
  CHECK(find_event(seen, [](const Seen &s) { return s.end.has_value(); }) ==
            nullptr,
        "nothing is sent before 259 ms");
  seen = r.run_to(3.4);
  const Seen *end =
      find_event(seen, [](const Seen &s) { return s.end.has_value(); });
  CHECK(end && near(end->at, 3.259, 1e-6),
        "259 ms after the request the client's instance is ended "
        "(ClientEndAbility)");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

int64_t expr_by_text(const std::string &path, const char *text) {
  sqlite3 *db = nullptr;
  int64_t id = 0;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK)
    return 0;
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT id FROM ab_expr WHERE text = ?1", -1, &st,
                         nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, text, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
      id = sqlite3_column_int64(st, 0);
  }
  if (st)
    sqlite3_finalize(st);
  sqlite3_close(db);
  return id;
}

int64_t jest_charges_expr(Rig &r) {
  const abilities::AbilityDef *a =
      r.catalog->ability_named("Bard_Misc_QuickWit_Jest");
  return a ? a->row.charges_expr : 0;
}

double eval_expr(Rig &r, int64_t expr) {
  std::string err;
  auto ev = r.cs->evaluation({}, err);
  if (!ev)
    return -1e9;
  stats::ExprHost host;
  double v = -1e9;
  if (!ev->eval(r.catalog->program(expr), host, v, err))
    return -1e9;
  return v;
}

bool holds(const std::vector<stats::SkillRank> &held, uint64_t tree_type,
           int64_t tree, uint64_t type, int64_t guid) {
  for (const stats::SkillRank &s : held)
    if (s.cls.type_id == tree_type && s.cls.guid == tree &&
        s.skill.type_id == type && s.skill.guid == guid)
      return true;
  return false;
}

void check_held_ranks_passives_and_mage(Rig &bard, Rig &plain) {
  bool endurance = false;
  for (const stats::ActiveEffect &e : bard.book->passives(0)) {
    for (const stats::EffectMod &m : e.mods)
      endurance = endurance ||
                  (m.stat == content::kGuidMaxStamina && m.value == 10.0 &&
                   e.effect.type_id == stats::kRefTypeEffect && e.stacks == 1);
  }
  CHECK(endurance, "passives() carries Endurance Training: MaxStamina +10 as "
                   "an Effect-typed reference");
  CHECK(bard.a7->max_stamina() == 110.0 && plain.a7->max_stamina() == 110.0,
        "(so a level-25 Bard has 110 Stamina, as the join gives it)");
  {
    Rig mage_plain(8, 25);
    CHECKF(mage_plain.ok(), "the Mage rig: %s", mage_plain.problem.c_str());
    if (mage_plain.ok())
      CHECK(eval_expr(
                mage_plain,
                expr_by_text(mage_plain.path,
                             "GetSkillPurchased(GetInstigator(), "
                             "$#297887639323616934:107281647359609$, "
                             "$#4847957796014348780:6064631986549818361$)")) ==
                0.0,
            "a Mage without the Blink upgrade: GetSkillPurchased(tree, <that "
            "rank>) is false");
  }
  {
    Rig mage(8, 25, {"Mage_Passive_Blink_Unyielding"});
    CHECKF(mage.ok(), "the Mage rig with the upgrade: %s",
           mage.problem.c_str());
    if (mage.ok()) {
      CHECK(eval_expr(
                mage,
                expr_by_text(mage.path,
                             "GetSkillPurchased(GetInstigator(), "
                             "$#297887639323616934:107281647359609$, "
                             "$#4847957796014348780:6064631986549818361$)")) ==
                1.0,
            "...and true once the rank is bought (it could never be true: the "
            "join held skills only)");
      CHECK(holds(mage.book->held_ranks(), stats::kRefTypeSkillTree,
                  107281647359609ll, stats::kRefTypeSkillRank,
                  6064631986549818361ll),
            "held_ranks() lists the rank-typed reference of the bought rank");
    }
  }
}

void test_held_ranks() {
  std::printf(
      "the ranks the join holds: the talents::Book's held_ranks() and "
      "passives(), the one source of both the session and these tests\n");
  Rig bard(2, 25, kTalents);
  CHECKF(bard.ok(), "the rig: %s", bard.problem.c_str());
  if (!bard.ok())
    return;
  const std::vector<stats::SkillRank> held = bard.book->held_ranks();
  const int64_t tree = bard.book->class_tree()->guid;
  size_t skills = 0, ranks = 0;
  for (const stats::SkillRank &s : held) {
    skills += s.skill.type_id == stats::kRefTypeSkill ? 1 : 0;
    ranks += s.skill.type_id == stats::kRefTypeSkillRank ? 1 : 0;
  }
  CHECKF(skills > 5 && skills == ranks,
         "every listed rank is held twice: as its skill and as itself (%zu and "
         "%zu)",
         skills, ranks);
  const int64_t jest_skill =
      record_guid(bard.path, "SkillRecordBase", "Bard_QuickWit_Jest");
  CHECK(jest_skill != 0 && holds(held, stats::kRefTypeSkillTree, tree,
                                 stats::kRefTypeSkill, jest_skill),
        "the Bard's bought Jest talent is held as a skill of the class tree");
  CHECK(eval_expr(bard, jest_charges_expr(bard)) == 2.0,
        "...and Jest's charges formula (GetSkillPurchased of that skill) gives "
        "2");
  Rig plain(2, 25);
  CHECK(plain.ok() && eval_expr(plain, jest_charges_expr(plain)) == 1.0,
        "a Bard without the talent has 1");
  std::string err;
  auto ac = content::AbilityContent::open(bard.path, err);
  std::vector<stats::SkillRank> auto25;
  CHECK(ac && ac->auto_ranks_at(25, auto25, err) && !auto25.empty(),
        "the reader of the auto-granted ranks of a level-25 character");
  std::set<int64_t> own_trees;
  for (const talents::Tree *t : bard.book->listed_trees())
    own_trees.insert(t->guid);
  size_t missing = 0, expected = 0;
  for (const stats::SkillRank &a : auto25) {
    if (!own_trees.count(a.cls.guid))
      continue;
    ++expected;
    missing +=
        holds(held, a.cls.type_id, a.cls.guid, a.skill.type_id, a.skill.guid)
            ? 0
            : 1;
  }
  CHECKF(expected > 5 && missing == 0,
         "every auto-granted rank of level 25 in the character's own trees is "
         "held by the Book (%zu of %zu missing)",
         missing, expected);
  check_held_ranks_passives_and_mage(bard, plain);
}

const bootstrap::CooldownItem *cooldown_of(const std::vector<Seen> &seen,
                                           int64_t ability) {
  for (const Seen &s : seen)
    for (const auto &cd : s.change.cooldowns_set)
      if (cd.ability_guid == ability)
        return &cd;
  return nullptr;
}

void check_flourish_charge_chain(Rig &r) {
  const int64_t flourish = r.ability("Bard_Misc_Flourish");
  const double f0 = 30.0;
  r.trigger("Bard_Misc_Flourish", f0);
  std::vector<Seen> seen = r.run_to(f0 + 0.5);
  const bootstrap::CooldownItem *f1p = cooldown_of(seen, flourish);
  const bootstrap::CooldownItem f1 = f1p ? *f1p : bootstrap::CooldownItem{};
  CHECK(f1p && f1.charges == 1 && f1.initial_cooldown == 20.0f,
        "Flourish's first use: Charges 1, InitialCooldown 20 [R: the retail "
        "capture]");
  r.trigger("Bard_Misc_Flourish", f0 + 2.0);
  seen = r.run_to(f0 + 2.5);
  const bootstrap::CooldownItem *f2 = cooldown_of(seen, flourish);
  CHECK(f1p && f2 && f2->charges == 0 && f2->start_time == f1.start_time &&
            near(f2->end_time - f2->start_time, 20.0, 1e-6),
        "the second: Charges 0, the same start and end");
  seen = r.run_to(f0 + 0.3 + 20.0 + 0.05);
  const bootstrap::CooldownItem *f3 = cooldown_of(seen, flourish);
  CHECK(f1p && f3 && f3->charges == 1 && f3->start_time == f1.start_time &&
            near(f3->end_time - f3->start_time, 40.0, 1e-6),
        "the return: Charges 1, start S, end S + 40 [R: the retail capture]");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void check_jest_charge_return(Rig &r, int64_t jest, double start) {
  std::vector<Seen> seen = r.run_to(2.3 + 8.0 + 0.05);
  const bootstrap::CooldownItem *back = cooldown_of(seen, jest);
  CHECK(back && back->charges == 1 && back->initial_cooldown == 8.0f,
        "a charge is back: Charges 1");
  CHECKF(back && back->start_time == start,
         "its start is the first use's (%.4f, not 8 s later)",
         back ? back->start_time - start : -1.0);
  CHECKF(
      back && near(back->end_time - start, 16.0, 1e-6),
      "its end is 16 s after the start: the next charge returns 8 s on (%.4f)",
      back ? back->end_time - start : -1.0);
  CHECK(r.pawn.cooldown(jest) != nullptr,
        "(the item stays while a charge is missing)");
  seen = r.run_to(2.3 + 16.0 + 0.05);
  bool removed = false;
  for (const Seen &s : seen)
    for (const int64_t a : s.change.cooldowns_removed)
      removed = removed || a == jest;
  CHECK(removed && r.pawn.cooldown(jest) == nullptr,
        "the last charge returns 16 s after the first use and the item goes");
  check_flourish_charge_chain(r);
}

void test_charge_chain() {
  std::printf("charges: the Cooldowns item of a chain keeps the first use's "
              "start [R: the retail captures]\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.know({"Bard_Misc_QuickWit_Jest", "Bard_Misc_Flourish"});
  const int64_t jest = r.ability("Bard_Misc_QuickWit_Jest");
  std::vector<Seen> seen;
  CHECK(r.trigger("Bard_Misc_QuickWit_Jest", 2.0, true, 0, &seen) ==
            Refusal::None,
        "the first Jest is accepted");
  seen = r.run_to(2.5);
  const bootstrap::CooldownItem *first = cooldown_of(seen, jest);
  CHECK(first && first->charges == 1 && first->initial_cooldown == 8.0f &&
            first->triggered,
        "the first use: Charges 1 left, InitialCooldown 8 [R: 8.0, 1]");
  if (!first)
    return;
  const double start = first->start_time;
  CHECKF(near(first->end_time - start, 8.0, 1e-6),
         "the first item ends 8 s after it starts (%.4f)",
         first->end_time - start);
  r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
  CHECK(r.trigger("Bard_Misc_QuickWit_Jest", 5.0, true, 0, &seen) ==
            Refusal::None,
        "the second Jest is accepted with one charge left");
  seen = r.run_to(5.5);
  const bootstrap::CooldownItem *second = cooldown_of(seen, jest);
  CHECK(second && second->charges == 0 && second->initial_cooldown == 8.0f,
        "the second use: Charges 0 [R: 8.0, 0]");
  CHECKF(second && second->start_time == start &&
             near(second->end_time - start, 8.0, 1e-6),
         "...with the same start and end as the first (the second use does not "
         "restart the 8 s: %.4f)",
         second ? second->end_time - second->start_time : -1.0);
  check_jest_charge_return(r, jest, start);
}

struct ZoneSeen {
  double at;
  bootstrap::ZoneOp op;
};

std::vector<ZoneSeen> zone_events(const Rig &r) {
  std::vector<ZoneSeen> out;
  for (const Seen &s : r.log)
    for (const bootstrap::ZoneOp &z : s.zones)
      out.push_back({s.at, z});
  return out;
}

const ZoneSeen *zone_event(const std::vector<ZoneSeen> &zs,
                           bootstrap::ZoneOp::Kind kind, int64_t ability,
                           size_t nth = 0) {
  for (const ZoneSeen &z : zs) {
    if (z.op.kind == kind && z.op.ability == ability && nth-- == 0)
      return &z;
  }
  return nullptr;
}

void check_dance_zone_fade_and_close(Rig &r, int64_t shielding, int64_t aura,
                                     const content::ZoneRow *zone,
                                     double open_at) {
  using K = bootstrap::ZoneOp::Kind;
  const int64_t aura_key =
      r.pawn.effect(aura) ? r.pawn.effect(aura)->effect_id : -1;
  r.run_to(open_at + 29.0);
  CHECK(zone_events(r).size() == 1 && r.pawn.effect(aura) != nullptr,
        "29 s on: no close, the Aura is still on");
  CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick",
                      open_at + 28.0) == AbilityPipeline::HitVerdict::Scheduled,
        "a LingerTick request 28 s in is taken (the zone's actor is still "
        "there)");
  r.run_to(open_at + 32.0);
  std::vector<ZoneSeen> zs = zone_events(r);
  double aura_gone = -1;
  for (const Seen &s : r.log)
    for (const int64_t key : s.change.effects_removed)
      if (key == aura_key && aura_gone < 0)
        aura_gone = s.at;
  const ZoneSeen *fade = zone_event(zs, K::Fade, shielding);
  const ZoneSeen *close = zone_event(zs, K::Close, shielding);
  CHECK(r.pawn.effect(aura) == nullptr,
        "the Aura is gone from the Bard (it had no end)");
  CHECKF(aura_gone > 0 && near(aura_gone, open_at + zone->life_time, 1e-9),
         "...when the zone's 30 s are over (%.3f)", aura_gone - open_at);
  CHECK(fade &&
            near(fade->at, aura_gone + AbilityPipeline::kZoneFadeDelay, 1e-9),
        "the actor fades out 60 ms later (bFadingOut)");
  CHECK(
      close &&
          near(close->at, aura_gone + AbilityPipeline::kZoneCloseDelay, 1e-9) &&
          fade && fade->at < close->at,
      "and is closed 0.7 s after the Aura went");
  CHECK(zs.size() == 3 && r.pipe->zones() == 0,
        "(the actor opens once, fades once and closes once: nothing is left)");
  CHECK(r.hit_request("Bard_Dance_Shielding", "Bard_Dance_Shielding_LingerTick",
                      open_at + 40.0) == AbilityPipeline::HitVerdict::NoCast,
        "a LingerTick request after the close is dropped");
  CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
}

void check_dance_zone_30s_lifecycle(Rig &r, int64_t shielding, int64_t aura,
                                    const content::ZoneRow *zone) {
  using K = bootstrap::ZoneOp::Kind;
  const double t0 = 2.0;
  CHECK(r.trigger("Bard_Dance_Shielding", t0) == Refusal::None,
        "Shielding is accepted");
  r.run_to(t0 + 1.0);
  std::vector<ZoneSeen> zs = zone_events(r);
  const ZoneSeen *open = zone_event(zs, K::Open, shielding);
  CHECK(zs.size() == 1 && open != nullptr,
        "one zone event so far: its actor opens");
  if (!open)
    return;
  CHECKF(near(open->at - t0,
              AbilityPipeline::kItemDelay + AbilityPipeline::kZoneOpenDelay,
              1e-9) &&
             open->at - t0 >= 0.3 && open->at - t0 <= 0.5,
         "within 300 to 500 ms of the trigger (%.0f ms)",
         (open->at - t0) * 1000);
  CHECK(open->op.zone == zone->guid && open->op.id > 0,
        "it names the zone record and has an instance number");
  CHECK(r.pawn.effect(aura) != nullptr && r.pawn.effect(aura)->duration > 1e30f,
        "the Aura, the zone's infinite effect, is on the Bard");
  check_dance_zone_fade_and_close(r, shielding, aura, zone, open->at);
}

void check_dance_zone_retail_mesmerizing_and_aura(Rig &m, const ZoneSeen *sc) {
  using K = bootstrap::ZoneOp::Kind;
  const int64_t mesmerizing = m.ability("Bard_Dance_Mesmerizing");
  m.set_current(content::kGuidMana, m.pawn.stat(content::kGuidMaxMana));
  m.trigger("Bard_Dance_Mesmerizing", 50.0);
  m.run_to(60.0);
  std::vector<ZoneSeen> mz = zone_events(m);
  const ZoneSeen *eo = zone_event(mz, K::Open, mesmerizing);
  const ZoneSeen *ec = zone_event(mz, K::Close, mesmerizing);
  CHECK(eo && ec && near(eo->at - 50.0, 0.389, 1e-9) &&
            near(ec->at - 50.0, 2.334, 1e-9),
        "Mesmerizing Dance: its actor opens at +389 ms and is destroyed at "
        "+2,334 ms [R]");
  const int64_t shielding_aura = m.effect("Bard_Dance_Shielding_Aura");
  int64_t aura_id = -1;
  for (const Seen &s : m.log)
    for (const auto &e : s.change.effects_added)
      if (e.effect_guid == shielding_aura)
        aura_id = e.effect_id;
  double gone = -1;
  for (const Seen &s : m.log)
    for (const int64_t k : s.change.effects_removed)
      if (k == aura_id && gone < 0)
        gone = s.at;
  CHECK(sc && gone > 0 &&
            near(gone, sc->at - AbilityPipeline::kZoneCloseDelay, 1e-9),
        "Shielding's Aura goes 0.7 s before its actor does [R: 702 ms]");
  CHECK(m.pawn.broken.empty() && m.pawn.effect(shielding_aura) == nullptr,
        "(the pawn took every change; no Aura is left)");
}

void check_dance_zone_retail_timings(int64_t shielding) {
  using K = bootstrap::ZoneOp::Kind;
  Rig m(2, 25, kTalents);
  m.asks = false;
  m.pipe->set_data_timings(true);
  m.know({"Bard_Dance_Maddening", "Bard_Dance_Mesmerizing",
          "Bard_Dance_Shielding", "Bard_Misc_Flourish"});
  const int64_t maddening = m.ability("Bard_Dance_Maddening");
  m.trigger("Bard_Dance_Maddening", 2.0);
  m.run_to(10.0);
  std::vector<ZoneSeen> mz = zone_events(m);
  const ZoneSeen *mo = zone_event(mz, K::Open, maddening);
  const ZoneSeen *mf = zone_event(mz, K::Fade, maddening);
  const ZoneSeen *mc = zone_event(mz, K::Close, maddening);
  CHECKF(mo && near(mo->at - 2.0, 0.362, 1e-9),
         "Maddening Dance: its actor opens 362 ms after the trigger, as "
         "retail's [R: the retail capture] (%.0f ms)",
         mo ? (mo->at - 2.0) * 1000 : -1.0);
  CHECKF(mc && near(mc->at - 2.0, 5.383, 1e-9),
         "and is destroyed 5,383 ms after it [R] (%.0f ms)",
         mc ? (mc->at - 2.0) * 1000 : -1.0);
  CHECK(mf && mc &&
            near(mc->at - mf->at,
                 AbilityPipeline::kZoneCloseDelay -
                     AbilityPipeline::kZoneFadeDelay,
                 1e-9),
        "with the fade-out 0.64 s before [R: 641 ms]");
  m.set_current(content::kGuidMana, m.pawn.stat(content::kGuidMaxMana));
  m.trigger("Bard_Dance_Shielding", 20.0);
  m.run_to(21.0);
  m.set_current(content::kGuidMana, m.pawn.stat(content::kGuidMaxMana));
  m.trigger("Bard_Misc_Flourish", 21.0);
  m.run_to(30.0);
  mz = zone_events(m);
  const ZoneSeen *so = zone_event(mz, K::Open, shielding);
  const ZoneSeen *sc = zone_event(mz, K::Close, shielding);
  CHECKF(so && near(so->at - 20.0, 0.287, 1e-9) && sc &&
             near(sc->at - 20.0, 5.247, 1e-9),
         "Shielding Dance (zone +287 ms, close +5,247 ms: the middle of the "
         "two captures) is not ended by Flourish [R]");
  check_dance_zone_retail_mesmerizing_and_aura(m, sc);
}

void test_dance_zones() {
  std::printf("dances: the zone's actor opens near the cast, ends with the "
              "dance and takes the Aura with it\n");
  Rig r(2, 25, kTalents);
  CHECKF(r.ok(), "the rig: %s", r.problem.c_str());
  if (!r.ok())
    return;
  r.asks = false;
  r.know({"Bard_Dance_Shielding", "Bard_Dance_Maddening", "Bard_Dance_Nimble",
          "Bard_Dance_Mesmerizing", "Bard_Misc_Flourish", "Bard_Saga",
          "Bard_Misc_QuickWit_Jest"});
  const int64_t shielding = r.ability("Bard_Dance_Shielding");
  const int64_t aura = r.effect("Bard_Dance_Shielding_Aura");
  for (const char *name : {"Bard_Dance_Shielding", "Bard_Dance_Maddening",
                           "Bard_Dance_Nimble", "Bard_Dance_Mesmerizing"}) {
    const abilities::AbilityDef *d = r.catalog->ability_named(name);
    CHECKF(d && d->zones.size() == 1 && r.catalog->zone(d->zones[0]) != nullptr,
           "%s spawns one zone with its cast", name);
  }
  CHECK(r.catalog->ability_named("Bard_Saga")->zones.empty() &&
            r.catalog->ability_named("Bard_Misc_Flourish")->zones.empty() &&
            r.catalog->ability_named("Bard_Misc_QuickWit_Jest")->zones.empty(),
        "the Saga (its zone comes with a hit), Flourish and Jest spawn none");
  const content::ZoneRow *zone = r.catalog->zone(
      r.catalog->ability_named("Bard_Dance_Shielding")->zones[0]);
  check_dance_zone_30s_lifecycle(r, shielding, aura, zone);
  check_dance_zone_retail_timings(shielding);
}

} // namespace

int main() {
  if (!real_db_present()) {
    std::printf("no content.db at %s (cd services && go run ./cmd/contentc): "
                "the generic ability runtime's tests are skipped\n",
                real_db_path().c_str());
    return 0;
  }
  test_held_ranks();
  test_charge_chain();
  test_dance_zones();
  test_cast_flood();
  test_unaffordable_at_the_cost_step();
  test_time();
  test_regeneration();
  test_movement();
  test_a7_beside();
  test_class_resources();
  test_mend_chain();
  test_end_request();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
