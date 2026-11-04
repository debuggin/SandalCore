// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_abilities_common.h"

using namespace aoc::worldd;
using namespace aoc::protocol;
using namespace aoc::protocol::repl;

namespace {

bool near_d(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

bool evasion_effect_ok(const content::EffectRule *ev, const content::EffectRule *ender) {
  return ev && !ev->infinite && ev->duration == 0.5 && ev->stops_regen_standing && ev->stops_regen_moving && ender && ev->end_effect_duration == 1.0 && ender->stops_regen_standing && ender->stops_regen_moving;
}

void check_ability_table_rules(const content::AbilityRules &a) {
  CHECK(a.sprint.guid == content::kGuidSprint && a.sprint.held == content::Held::Smart && a.sprint.min_stamina == 1.0 && a.sprint.cost_amount == 1.25 && a.sprint.charge_interval == 0.25 && a.sprint.cost_requires_moving &&
            a.sprint.cooldown == 0,
        "Sprint: held, needs 1 Stamina, takes 1.25 every 0.25 s while moving (5 a second), no cooldown");
  CHECK(a.dodge.guid == content::kGuidDodge && a.dodge.held == content::Held::Naive && a.dodge.cost_amount == 50 && a.dodge.charge_interval == 0 && a.dodge.cooldown == 0.65 && a.dodge.triggers_cooldown && a.dodge.disable_during_cooldown,
        "Dodge: 50 once, a 0.65 s cooldown the paid cost starts");
}

void check_rest_and_effect_rules(const content::AbilityRules &a) {
  CHECK(a.rest.guid == content::kGuidRest && a.rest.held == content::Held::Toggle && a.rest.requires_out_of_combat && a.rest.cost_stat_guid == 0 && a.rest.targets_self && a.rest.cancel_section == "SitEnd",
        "Rest: a free toggle, out of combat only, on the caster, its end section SitEnd");
  const content::EffectRule *ev = a.effect(a.dodge.effect_guid);
  const content::EffectRule *ender = ev ? a.effect(ev->end_effect_guid) : nullptr;
  CHECK(evasion_effect_ok(ev, ender), "the Dodge's Evasion effect lasts 0.5 s and stops regeneration; its End hit pauses it for 1 s more");
  const content::EffectRule *sp = a.effect(a.sprint.effect_guid);
  CHECK(sp && sp->infinite && !sp->stops_regen_standing && sp->stops_regen_moving && sp->end_effect_duration == 1.0, "the Sprint's effect is infinite, stops regeneration only while moving, and pauses it for 1 s when it ends");
}

void check_stat_mod_rules(const content::AbilityRules &a) {
  const content::EffectRule *sp = a.effect(a.sprint.effect_guid);
  CHECK(sp && sp->tick_timer == 1.0 && sp->stat_mods.size() == 1 && sp->stat_mods[0].stat_guid == kMoveSpeedMult && sp->stat_mods[0].source_stat_guid == 0 && sp->stat_mods[0].addend == 0.2 && sp->stat_mods[0].factor == 0,
        "and it ticks every 1 s (retail's TickTime) and adds 0.2 to MoveSpeedMult: 1.2, 720 cm/s against 600 running");
  const content::EffectRule *rest = a.effect(a.rest.effect_guid);
  CHECK(rest && rest->infinite && rest->stat_mods.size() == 2 && rest->stat_mods[0].stat_guid == kHealthRegen && rest->stat_mods[0].source_stat_guid == kMaxHealth && rest->stat_mods[0].factor == 0.01 && rest->stat_mods[0].addend == 1.0,
        "the Rest effect adds 0.01 x MaxHealth + 1 to HealthRegeneration, and the same with Mana");
}

void check_whitelist_rules(const content::AbilityRules &a) {
  CHECK(a.stat_regen_time == 1.0 && a.regen_rate == 5.0, "Stamina regenerates 5 every 1 s");
  CHECK(a.ability(content::kGuidSprint) == &a.sprint && a.ability(content::kGuidDodge) == &a.dodge && a.ability(content::kGuidRest) == &a.rest && a.ability(kScepterCombo) == nullptr && a.ability(0) == nullptr,
        "only Sprint, Dodge and Rest are abilities worldd runs");
}

std::string altered_rules_error(const fs::path &dir, const char *name, const char *from, const char *to) {
  const fs::path path = dir / (std::string(name) + ".db");
  content_fixture::DbOptions o;
  o.ability_from = from;
  o.ability_to = to;
  if (!content_fixture::make_db(path, o))
    std::exit(2);
  std::string error;
  auto opened = content::ContentDb::open(path.string(), error);
  if (!opened)
    return std::string("open: ") + error;
  const auto rules = opened->ability_rules(error);
  return rules ? std::string() : error;
}

void check_altered_rules_refusals(const Rules &r) {
  const std::string e = altered_rules_error(r.dir, "unchanged", "'Smart'", "'Smart'");
  CHECK(e.empty(), "(a file rebuilt from the same rows reads)");
  const std::string held = altered_rules_error(r.dir, "notheld", "'Smart'", "'Naive'");
  CHECK(contains(held, "Sprint") && contains(held, "contentc"), "a Sprint that is not held is refused, naming the Sprint and how to rebuild the file");
  const std::string unknown = altered_rules_error(r.dir, "unknown", "'Smart'", "'Sticky'");
  CHECK(contains(unknown, "General_Sprint") && contains(unknown, "Sticky"), "an unknown held behaviour is refused by name");
  const std::string nocost = altered_rules_error(r.dir, "nocost", "1.25, 1, 0.25", "0.0, 1, 0.25");
  CHECK(contains(nocost, "Sprint"), "a Sprint that costs nothing is refused");
  const std::string nocd = altered_rules_error(r.dir, "nocd", "50.0, 0, 0.0, 0.65, 1, 1", "50.0, 0, 0.0, 0.0, 1, 1");
  CHECK(contains(nocd, "Dodge"), "a Dodge with no cooldown is refused");
  const std::string fast = altered_rules_error(r.dir, "fastcharge", "1.25, 1, 0.25", "1.25, 1, 0.001");
  CHECK(contains(fast, "Sprint") && contains(fast, "interval") && contains(fast, "contentc"), "a Sprint that charges every millisecond is refused");
  const std::string edge = altered_rules_error(r.dir, "edgecharge", "1.25, 1, 0.25", "1.25, 1, 0.01");
  CHECK(edge.empty(), "(0.01 s is the shortest charge interval accepted)");
}

void check_world_and_legacy_db_refusals(const Rules &r) {
  content_fixture::DbOptions slow_regen;
  slow_regen.stat_regen_time = "0.001";
  const fs::path regen_path = r.dir / "fastregen.db";
  if (!content_fixture::make_db(regen_path, slow_regen))
    std::exit(2);
  std::string regen_error;
  auto regen_db = content::ContentDb::open(regen_path.string(), regen_error);
  const auto regen_rules = regen_db ? regen_db->ability_rules(regen_error) : std::nullopt;
  CHECK(regen_db && !regen_rules && contains(regen_error, "regeneration") && contains(regen_error, "interval"), "so is a Stamina regeneration tick of a millisecond");

  const fs::path path = r.dir / "world_notheld.db";
  content_fixture::DbOptions o;
  o.ability_from = "'Smart'";
  o.ability_to = "'Naive'";
  if (!content_fixture::make_db(path, o))
    std::exit(2);
  std::string error;
  auto bad = content::ContentDb::open(path.string(), error);
  ServerGuidAllocator guids{60, 0x6ee87ee4u};
  if (bad) {
    PlayerWorld refused(&*bad, "", bs::GameStateMode::Tier2, 60, guids);
    CHECK(!refused.ready() && refused.ability_rules() == nullptr && contains(refused.content_problem(), "Sprint"), "a PlayerWorld given such a content.db is not ready (no login) and its problem names the Sprint");
  }
  PlayerWorld ready(&*r.db, "", bs::GameStateMode::Tier2, 60, guids);
  CHECK(ready.ready() && ready.ability_rules() && ready.ability_rules()->sprint.cost_amount == 1.25, "with the right rows it is ready and holds the rules for its sessions");

  content_fixture::DbOptions old_opt;
  old_opt.ability_tables = false;
  const fs::path old_path = r.dir / "old.db";
  if (!content_fixture::make_db(old_path, old_opt))
    std::exit(2);
  const auto opened = content::ContentDb::open(old_path.string(), error);
  CHECK(!opened && contains(error, "contentc") && contains(error, "ability_runtime"), "a content.db from before plan A7 (no ability tables) fails at open and says to rebuild it with this checkout's contentc");
}

void test_rules(const Rules &r) {
  std::printf("content.db's ability rules\n");
  check_ability_table_rules(r.rules);
  check_rest_and_effect_rules(r.rules);
  check_stat_mod_rules(r.rules);
  check_whitelist_rules(r.rules);
  check_altered_rules_refusals(r);
  check_world_and_legacy_db_refusals(r);
}

bool sprint_start_change_ok(const bs::AbilityChange &c) {
  return c.abilities_added.size() == 1 && c.abilities_added[0].ability_guid == content::kGuidSprint && c.abilities_added[0].server_instance_id == 0 && c.abilities_added[0].montage_group == "Sprint" &&
         c.abilities_added[0].montage_path == bs::kSprintMontagePath && c.effects_added.size() == 1 && c.effects_added[0].effect_guid == kSprintEffect && c.effects_added[0].duration > 1e38f && c.effects_added[0].tick_time == 1.0f;
}

void check_sprint_start_and_drain(Sim &s) {
  s.move(true);
  CHECK(s.trigger(content::kGuidSprint, 0) == Refusal::None && s.rt.sprinting(), "a Sprint starts with full Stamina");
  const auto starts = s.with("the Sprint starts");
  CHECK(starts.size() == 1 && sprint_start_change_ok(starts[0]->change), "it adds its ability item (instance 0, MontageGroup Sprint, the Sprint montage) and its effect: infinite, ticking every 1 s");
  CHECK(stat_of(starts[0]->change, kMoveSpeedMult) == 1.2f && s.rt.stat(kMoveSpeedMult) == 1.2, "in the same step MoveSpeedMult goes from 1.0 to 1.2 (the retail server replicates it with the Sprint: the client moves at 600 x "
                                                                                                "it)");
  CHECK(stat_of(starts[0]->change, content::kGuidStamina) == 98.75f && s.rt.stamina() == 98.75, "and the first charge is taken with the activation: 98.75, in the same step");
  s.run_to(1.0);
  const auto series = s.stamina_series();
  CHECK(series.size() == 5 && series[0] == 98.75f && series[1] == 97.5f && series[2] == 96.25f && series[4] == 93.75f, "then every 0.25 s it takes 1.25 (98.75, 97.5 ... 93.75 after a second)");
  CHECK(s.rt.stamina() == 93.75, "5 Stamina a second");
  s.run_to(19.5);
  CHECK(s.rt.sprinting() && s.rt.stamina() == 1.25, "after 19.5 s it still runs, with 1.25 left (79 charges)");
  s.run_to(19.75);
  CHECK(!s.rt.sprinting() && s.rt.stamina() == 0.0 && s.stamina_series().size() == 80, "the 80th charge, at 19.75 s, leaves 0 and the Sprint is over: a full bar lasts 20 s from the first charge");
}

void check_sprint_exhaustion_step(Sim &s) {
  const auto ends = s.with("out of Stamina");
  CHECK(ends.size() == 1 && !ends[0]->end_ability && ends[0]->at == 19.75 && ends[0]->change.abilities_removed == std::vector<int64_t>{0} && ends[0]->change.effects_removed.size() == 1 && stat_of(ends[0]->change, kMoveSpeedMult) == 1.0f,
        "that step removes the ability item and the effect and puts MoveSpeedMult back to 1.0 ...");
  CHECK(ends.size() == 1 && ends[0]->change.effects_added.size() == 1 && ends[0]->change.effects_added[0].effect_guid == kDisableRegenEffect && ends[0]->change.effects_added[0].duration == 1.0f && s.rt.regen_pause_until() == 20.75,
        "... and adds the 1 s DisableRegen effect (retail: all in one packet)");
  CHECK(s.ends().empty(), "ClientEndAbility has not gone yet");
}

void check_sprint_exhaustion_and_empty_refusal(Sim &s) {
  check_sprint_exhaustion_step(s);
  s.run_to(20.0);
  const auto told = s.with("the client is told");
  CHECK(told.size() == 1 && told[0]->end_ability == 0 && near_d(told[0]->at, 19.75 + AbilityRuntime::kEndAbilityDelay, 1e-9) && told[0]->change.empty() && s.ends() == std::vector<int32_t>{0},
        "ClientEndAbility(0) follows 163 ms later, after the update, on its own");
  CHECK(s.trigger(content::kGuidSprint, 1) == Refusal::Stamina && s.ends().back() == 1 && !s.rt.sprinting() && s.rt.refused() == 1, "a new Sprint with no Stamina is refused with ClientEndAbility(1)");
  CHECK(s.log.back().change.empty() && s.log.back().end_ability == 1, "(and nothing else changes: the refusal carries no state change)");
}

void check_sprint_standing_and_cancel(const Rules &r) {
  Sim stand(r.rules);
  stand.trigger(content::kGuidSprint, 0);
  CHECK(stand.rt.stamina() == 100.0 && !stat_of(stand.log[0].change, content::kGuidStamina), "a Sprint started standing still takes no charge with the activation");
  Sim last(r.rules, [] {
    auto m = level1_stats();
    m[content::kGuidStamina] = 1.25;
    return m;
  }());
  last.move(true);
  last.trigger(content::kGuidSprint, 6);
  CHECK(!last.rt.sprinting() && last.rt.stamina() == 0.0 && last.with("out of Stamina").size() == 1 && last.ends().empty(), "with 1.25 Stamina (enough to start) the activation's charge leaves 0 and the Sprint ends in the same instant");
  last.run_to(0.15);
  CHECK(last.ends().empty(), "its ClientEndAbility has not gone at 0.15 s ...");
  last.run_to(0.2);
  CHECK(last.ends() == std::vector<int32_t>{6}, "... and goes at 0.163 s, with the client's own id");

  Sim s(r.rules);
  s.move(true);
  s.trigger(content::kGuidSprint, 0);
  s.run_to(2.0);
  CHECK(s.rt.stamina() == 88.75, "(2 s of Sprint, nine charges with the first: 88.75)");
  CHECK(s.cancel(0) && !s.rt.sprinting(), "ServerNotifyHeldAbilityCancelled(0) releases the Sprint");
  const auto released = s.with("released");
  CHECK(released.size() == 1 && !released[0]->end_ability && released[0]->change.abilities_removed.size() == 1 && released[0]->change.effects_removed.size() == 1 && stat_of(released[0]->change, kMoveSpeedMult) == 1.0f,
        "its items are removed, MoveSpeedMult is back to 1.0 and no ClientEndAbility is sent");
  s.run_to(2.99);
  CHECK(s.rt.stamina() == 88.75, "the drain stopped, and regeneration waits out the 1 s pause");
  s.run_to(3.0);
  CHECK(s.rt.stamina() == 93.75, "the first regeneration tick after the pause (t = 3 s) gives 5");
  CHECK(!s.cancel(0) && !s.cancel(7), "a cancel of a finished or an unknown instance is ignored");
  const auto pause_end = s.with("regeneration pause ends");
  CHECK(pause_end.size() == 1 && pause_end[0]->at == 3.0 && pause_end[0]->change.effects_removed.size() == 1, "the DisableRegen effect is removed when the pause is over (retail: 1.0 s after it was added)");
}

void check_sprint_moving_and_odd_bar(const Rules &r) {
  Sim s(r.rules);
  s.trigger(content::kGuidSprint, 0);
  s.run_to(2.0);
  CHECK(s.rt.sprinting() && s.rt.stamina() == 100.0 && s.with("charges").empty(), "a Sprint with the player standing charges nothing");
  s.move(true);
  s.run_to(3.0);
  CHECK(s.rt.stamina() == 95.0, "once the player moves it takes 5 a second");
  s.moving = false;
  s.run_to(6.0);
  double last_charge = 0;
  for (const AbilityEvent *e : s.with("charges"))
    last_charge = e->at;
  CHECK(last_charge == 3.75 && !s.rt.moving(6.0), "a player no move has been heard from for 0.75 s counts as standing: the last charge is at 3.75 s");

  Sim b(r.rules);
  b.move(true);
  b.trigger(content::kGuidSprint, 0);
  b.run_to(1.5);
  CHECK(b.rt.regeneration_blocked(1.5), "sprinting and moving: regeneration is blocked");
  b.moving = false;
  b.rt.set_moving(false, b.now);
  CHECK(!b.rt.regeneration_blocked(b.now), "sprinting and standing: it is not");

  Sim odd(r.rules, [] {
    auto m = level1_stats();
    m[content::kGuidStamina] = 101;
    m[content::kGuidMaxStamina] = 101;
    return m;
  }());
  odd.move(true);
  odd.trigger(content::kGuidSprint, 0);
  odd.run_to(19.75);
  CHECK(odd.rt.sprinting() && odd.rt.stamina() == 1.0, "a bar of 101: after 80 charges 1 is left, which is still enough to Sprint");
  odd.run_to(20.0);
  float lowest = 1000;
  for (const float v : odd.stamina_series())
    lowest = std::min(lowest, v);
  CHECK(!odd.rt.sprinting() && odd.rt.stamina() == 0.0 && lowest == 0.0f, "the 81st charge stops at 0, not at -0.25: Stamina is never negative");
}

void check_sprint_interactions_and_duplicates(const Rules &r) {
  Sim low(r.rules, [] {
    auto m = level1_stats();
    m[content::kGuidStamina] = 0.5;
    return m;
  }());
  CHECK(low.trigger(content::kGuidSprint, 0) == Refusal::Stamina && low.ends() == std::vector<int32_t>{0}, "with 0.5 Stamina a Sprint is refused (it needs 1)");
  Sim d(r.rules);
  d.move(true);
  d.trigger(content::kGuidSprint, 0);
  d.run_to(1.0);
  d.trigger(content::kGuidDodge, 1);
  CHECK(d.rt.sprinting() && d.rt.evading() && d.ends().empty() && !stat_of(d.with("the Dodge starts")[0]->change, kMoveSpeedMult),
        "a Dodge does not end the Sprint (the retail capture: the Sprint, MoveSpeedMult and the drain run on through it): no "
        "ClientEndAbility, no change to MoveSpeedMult");
  const double before = d.rt.stamina();
  d.run_to(2.0);
  CHECK(d.rt.sprinting() && d.rt.stamina() < before && d.series_of(kMoveSpeedMult) == std::vector<float>{1.2f}, "the drain goes on, and MoveSpeedMult was set once, by the Sprint");
  Sim rest(r.rules);
  rest.move(true);
  rest.trigger(content::kGuidSprint, 0);
  rest.trigger(content::kGuidRest, 1);
  CHECK(!rest.rt.sprinting() && rest.rt.resting() && rest.ends() == std::vector<int32_t>{0} && rest.series_of(kMoveSpeedMult) == std::vector<float>({1.2f, 1.0f}),
        "a Rest ends the Sprint with ClientEndAbility (no retail capture has a Rest, and no instance may linger on the client), "
        "MoveSpeedMult back to 1.0");
  Sim dup(r.rules);
  dup.trigger(content::kGuidSprint, 0);
  const size_t events = dup.log.size();
  CHECK(dup.trigger(content::kGuidSprint, 0) == Refusal::Duplicate && dup.log.size() == events && dup.rt.sprinting(), "the same ClientInstanceId again while it runs: dropped, nothing answered (ClientEndAbility would end the real one)");
  dup.trigger(content::kGuidSprint, 3);
  CHECK(dup.rt.sprinting() && dup.with("replaced").size() == 1, "a Sprint while another runs replaces it");
}

void test_sprint(const Rules &r) {
  std::printf("Sprint\n");
  Sim s(r.rules);
  check_sprint_start_and_drain(s);
  check_sprint_exhaustion_and_empty_refusal(s);
  check_sprint_standing_and_cancel(r);
  check_sprint_moving_and_odd_bar(r);
  check_sprint_interactions_and_duplicates(r);
}

void check_regeneration_cap_and_clock_jump(const Rules &r) {
  Sim s(r.rules, [] {
    auto m = level1_stats();
    m[content::kGuidStamina] = 98;
    return m;
  }());
  s.run_to(3.0);
  const auto series = s.stamina_series();
  CHECK(series.size() == 1 && series[0] == 100.0f && s.rt.stamina() == 100.0, "98 and +5 is 100, not 103");

  Sim fine(r.rules), jump(r.rules);
  fine.trigger(content::kGuidDodge, 0);
  jump.trigger(content::kGuidDodge, 0);
  fine.run_to(30.0);
  jump.rt.tick(30.0);
  jump.take();
  bool same = fine.log.size() == jump.log.size() && fine.log.size() > 10;
  for (size_t i = 0; same && i < fine.log.size(); ++i) {
    const AbilityEvent &a = fine.log[i];
    const AbilityEvent &b = jump.log[i];
    same = a.note == b.note && a.at == b.at && a.change.stats.size() == b.change.stats.size() && (a.change.stats.empty() || a.change.stats[0].value == b.change.stats[0].value);
  }
  CHECK(same && fine.rt.stamina() == 100.0 && jump.rt.stamina() == 100.0, "a Dodge and 30 s in 600 steps, or in one tick: the same events, each at its own time, and a full bar in both");
}

void test_regeneration(const Rules &r) {
  std::printf("Stamina regeneration\n");
  Sim s(r.rules);
  s.move(true);
  s.trigger(content::kGuidSprint, 0);
  s.run_to(20.0);
  CHECK(s.rt.stamina() == 0.0, "(the bar is empty at t = 20 s: it was at 19.75)");
  s.move(false);
  s.run_to(20.99);
  CHECK(s.rt.stamina() == 0.0, "the End hit's pause (to 20.75 s) and then the tick at 21 s: nothing before it");
  s.run_to(21.0);
  CHECK(s.rt.stamina() == 5.0 && s.rt.regen_pause_until() == 20.75, "then +5 at the first tick after the pause");
  s.run_to(30.0);
  CHECK(s.rt.stamina() == 50.0, "50 after 10 s of regeneration");
  s.run_to(39.95);
  CHECK(s.rt.stamina() == 95.0, "95 after 19 ticks");
  s.run_to(40.0);
  CHECK(s.rt.stamina() == 100.0, "back to 100 at t = 40 s: 20 ticks of 5 after the Sprint ended at 19.75 s");
  const size_t events = s.log.size();
  s.run_to(60.0);
  CHECK(s.rt.stamina() == 100.0 && s.log.size() == events, "a full bar sends nothing more (no change, no event)");
  const auto series = s.stamina_series();
  CHECK(series.size() == 80 + 20 && series.back() == 100.0f, "(the whole run: 80 charges and 20 regeneration ticks)");
  check_regeneration_cap_and_clock_jump(r);
}

bool dodge_start_change_ok(const bs::AbilityChange &c) {
  return c.stats.size() == 1 && c.stats[0].value == 50.0f && c.abilities_added.size() == 1 && c.abilities_added[0].ability_guid == content::kGuidDodge && c.cooldowns_set.size() == 1 && c.effects_added.size() == 1 &&
         c.effects_added[0].effect_guid == kEvasionEffect && c.effects_added[0].duration == 0.5f;
}

void check_dodge_start(Sim &s) {
  s.run_to(0.5);
  CHECK(s.trigger(content::kGuidDodge, 0) == Refusal::None && s.rt.stamina() == 50.0, "a Dodge costs 50, at once");
  const auto start = s.with("the Dodge starts");
  CHECK(start.size() == 1 && start[0]->at == 0.5 && dodge_start_change_ok(start[0]->change), "one step: Stamina 50, the ability item, the Cooldowns item and the 0.5 s Evasion effect");
  const bs::CooldownItem &cd = start[0]->change.cooldowns_set[0];
  CHECK(cd.ability_guid == content::kGuidDodge && cd.triggered && cd.initial_cooldown == 0.65f && cd.charges == 1 && cd.end_time - cd.start_time > 0.6499 && cd.end_time - cd.start_time < 0.6501 &&
            std::fabs(cd.start_time - bs::effect_clock(1000.5)) < 1e-6 && start[0]->change.effects_added[0].start_time == cd.start_time,
        "the cooldown: triggered, 0.65 s, one charge, from now to now + 0.65 on the synchronized clock (the Evasion's StartTime is its "
        "start, as retail's)");
  CHECK(s.rt.evading() && s.rt.dodge_cooldown_until() == 0.5 + 0.65, "the Evasion runs; the cooldown ends at 1.15 s");
}

void check_dodge_stamina_threshold(const Rules &r) {
  Sim exact(r.rules, [] {
    auto m = level1_stats();
    m[content::kGuidStamina] = 50;
    return m;
  }());
  CHECK(exact.trigger(content::kGuidDodge, 0) == Refusal::None && exact.rt.stamina() == 0.0, "50 Stamina is enough for a Dodge");
  Sim short_(r.rules, [] {
    auto m = level1_stats();
    m[content::kGuidStamina] = 49.9;
    return m;
  }());
  CHECK(short_.trigger(content::kGuidDodge, 0) == Refusal::Stamina && short_.rt.stamina() == 49.9, "49.9 is not");
}

void test_dodge(const Rules &r) {
  std::printf("Dodge\n");
  Sim s(r.rules);
  check_dodge_start(s);
  s.run_to(1.0);
  const auto evasion_end = s.with("the Evasion ends");
  CHECK(!s.rt.evading() && evasion_end.size() == 1 && evasion_end[0]->change.effects_removed.size() == 1 && evasion_end[0]->change.effects_added.size() == 1 && evasion_end[0]->change.effects_added[0].effect_guid == kDisableRegenEffect &&
            evasion_end[0]->change.effects_added[0].duration == 1.0f,
        "the Evasion effect ends after 0.5 s, and the 1 s DisableRegen effect takes its place in the same step (retail)");
  CHECK(s.trigger(content::kGuidDodge, 1) == Refusal::Cooldown && s.ends() == std::vector<int32_t>{1} && s.rt.stamina() == 50.0,
        "a second Dodge at 0.5 s after the first, in the cooldown: refused with ClientEndAbility(1), Stamina unchanged");
  s.run_to(1.2);
  CHECK(s.with("cooldown is over").size() == 1 && s.rt.stamina() == 50.0, "the cooldown ends at 1.15 s and its items go");
  const auto over = s.with("cooldown is over");
  CHECK(over.size() == 1 && over[0]->change.abilities_removed == std::vector<int64_t>{0} && over[0]->change.cooldowns_removed == std::vector<int64_t>{content::kGuidDodge}, "the ability item and the Cooldowns item are removed");
  CHECK(s.trigger(content::kGuidDodge, 2) == Refusal::None && s.rt.stamina() == 0.0, "after the cooldown a Dodge is allowed: 50 left, so 0");
  s.run_to(2.5);
  CHECK(s.trigger(content::kGuidDodge, 3) == Refusal::Stamina && s.ends() == ints({1, 3}) && s.rt.stamina() < 50.0, "a Dodge with less than 50 Stamina is refused with ClientEndAbility(3)");
  s.run_to(2.99);
  CHECK(s.rt.stamina() == 0.0, "regeneration stays off for 1.5 s after a Dodge (the Evasion, then the pause): still 0 at 2.99 s");
  s.run_to(3.0);
  CHECK(s.rt.stamina() == 5.0, "and resumes at the next tick (t = 3 s): 5");
  check_dodge_stamina_threshold(r);
}

void test_dodge_cooldown_edge(const Rules &r) {
  std::printf("Dodge: the cooldown to the tick\n");
  Sim at_edge(r.rules);
  at_edge.run_to(0.25);
  at_edge.trigger(content::kGuidDodge, 0);
  at_edge.run_to(0.9);
  CHECK(at_edge.rt.dodge_cooldown_until() == 0.9, "(the first Dodge at 0.25 s: its cooldown ends at 0.9 s)");
  CHECK(at_edge.trigger(content::kGuidDodge, 1) == Refusal::None && at_edge.rt.stamina() == 0.0 && at_edge.rt.evading(), "a Dodge at exactly 0.65 s after the first is accepted");
  Sim early(r.rules);
  early.run_to(0.25);
  early.trigger(content::kGuidDodge, 0);
  early.run_to(0.85);
  CHECK(early.rt.trigger(content::kGuidDodge, 1, 0.899) == Refusal::Cooldown, "one at 0.649 s after it is refused: in the cooldown");
  early.take();
  CHECK(early.ends() == std::vector<int32_t>{1} && early.rt.stamina() == 50.0, "with ClientEndAbility and no cost");
}

void test_clock_never_goes_back(const Rules &r) {
  std::printf("the clock never goes back\n");
  Sim s(r.rules);
  s.run_to(5.0);
  s.rt.tick(3.0);
  s.take();
  CHECK(s.log.empty(), "a tick with an earlier time changes nothing");
  CHECK(s.rt.trigger(content::kGuidDodge, 0, 3.0) == Refusal::None, "(a Dodge with an earlier time is a Dodge)");
  s.take();
  const auto start = s.with("the Dodge starts");
  CHECK(start.size() == 1 && start[0]->at == 5.0 && s.rt.dodge_cooldown_until() == 5.0 + 0.65, "... made at the runtime's own time, 5 s, not the caller's 3 s: its cooldown ends at 5.65 s");
  s.rt.set_moving(true, 1.0);
  s.rt.tick(5.4);
  s.take();
  CHECK(s.rt.evading() && s.with("the Evasion ends").empty(), "and the Evasion (0.5 s from 5 s) runs to 5.5 s, as if the clock had been 5 s all along");
  s.rt.tick(5.5);
  s.take();
  CHECK(!s.rt.evading() && s.with("the Evasion ends").size() == 1, "(it ends at 5.5 s)");
}

bs::ActiveAbilityItem dodge_item_at(const Rules &r, int32_t index, double x, double y) {
  Sim s(r.rules);
  s.trigger(content::kGuidDodge, 0, TriggerInfo{index, x, y});
  const auto start = s.with("the Dodge starts");
  return start.size() == 1 && start[0]->change.abilities_added.size() == 1 ? start[0]->change.abilities_added[0] : bs::ActiveAbilityItem{};
}

uint64_t montage_id_of(const bs::ActiveAbilityItem &a) {
  const auto m = bs::montage_for(a.montage_path);
  return m ? m->guid.ObjectId : 0ull;
}

uint64_t montage_package_of(const bs::ActiveAbilityItem &a) {
  const auto m = bs::montage_for(a.montage_path);
  return m && m->chain.size() == 2 ? m->chain[1].guid.ObjectId : 0ull;
}

void check_dodge_edge_inputs_and_dash_window(const Rules &r, const bs::ActiveAbilityItem &fwd) {
  const bs::ActiveAbilityItem unknown = dodge_item_at(r, 6, 0, -1);
  CHECK(unknown.montage_path.empty() && unknown.mesh_yaw == 0 && unknown.ability_guid == content::kGuidDodge, "an index no capture shows (6): the Dodge still happens, with no Montage");
  CHECK(dodge_item_at(r, 5, std::nan(""), 1).mesh_yaw == 0 && dodge_item_at(r, 5, 1e308, -1e308).mesh_yaw == 57344, "an input that is not a number gives MeshYaw 0; a huge one is only its direction");
  CHECK(dodge_item_at(r, 0, 1, 0).montage_path.empty() && dodge_item_at(r, -3, 1, 0).montage_path.empty() && dodge_item_at(r, 2147483647, 1, 0).montage_path.empty(), "and so does any other index, whatever it is");
  CHECK(r.rules.dodge.montage_path.find("RogueDash_Fwd") != std::string::npos && fwd.montage_path != r.rules.dodge.montage_path, "the rules' own montage for the Dodge (the Rogue's forward dash) is not what is sent");

  Sim s(r.rules);
  s.run_to(0.5);
  CHECK(!s.rt.dash_allowed(0.5), "before a Dodge the A6 checker gets no dash allowance");
  s.trigger(content::kGuidDodge, 0);
  CHECK(s.rt.dash_allowed(0.5) && s.rt.dash_allowed(1.0) && s.rt.dash_allowed(1.29) && !s.rt.dash_allowed(1.31),
        "a Dodge allows the dash from its trigger to the end of its Evasion plus 0.3 s (0.8 s: the dash covers 559-810 cm within 0.8 s)");
}

void test_dodge_items(const Rules &r) {
  std::printf("Dodge: the montage and MeshYaw of the trigger\n");
  const bs::ActiveAbilityItem fwd = dodge_item_at(r, 5, 1, 0);
  CHECK(montage_id_of(fwd) == 3289872098355792151ull && fwd.mesh_yaw == 0 && fwd.montage_group == "DefaultGroup" && fwd.targets_self == false,
        "AnimationIndex 5, input (1, 0): the forward montage (3289872098355792151), MeshYaw 0, group DefaultGroup");
  CHECK(montage_id_of(dodge_item_at(r, 5, 1, -1)) == 3289872098355792151ull && dodge_item_at(r, 5, 1, -1).mesh_yaw == 57344, "AnimationIndex 5 with the diagonal (1, -1): the same montage, MeshYaw 57344 (315 degrees)");
  CHECK(montage_id_of(dodge_item_at(r, 7, 0, 1)) == 1215243220362629129ull && dodge_item_at(r, 7, 0, 1).mesh_yaw == 0, "AnimationIndex 7, input (0, 1): the right montage (1215243220362629129), MeshYaw 0");
  CHECK(montage_id_of(dodge_item_at(r, 8, -1, 0)) == 858903896054330709ull && dodge_item_at(r, 8, -1, 0).mesh_yaw == 0, "AnimationIndex 8, input (-1, 0): the back montage (858903896054330709), MeshYaw 0");
  const bs::ActiveAbilityItem none = dodge_item_at(r, 9, 0, 0);
  CHECK(montage_id_of(none) == 11479915692035936417ull && montage_package_of(none) == 5290943011317832207ull && none.mesh_yaw == 32768,
        "AnimationIndex 9, no input: the no-input montage (11479915692035936417, package 5290943011317832207), MeshYaw 32768");
  CHECK(montage_id_of(dodge_item_at(r, 2, 0, 1)) == 10081100002258589133ull && dodge_item_at(r, 2, 0, 1).mesh_yaw == 0, "AnimationIndex 2: the Rogue's dash to the right (10081100002258589133), MeshYaw 0");
  check_dodge_edge_inputs_and_dash_window(r, fwd);
}

} // namespace

int main() {
  const Rules r;
  test_rules(r);
  test_sprint(r);
  test_regeneration(r);
  test_dodge(r);
  test_dodge_cooldown_edge(r);
  test_clock_never_goes_back(r);
  test_dodge_items(r);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
