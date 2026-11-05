// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_abilities_common.h"

using namespace aoc::worldd;
using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::FIntrepidNetworkGUID;

namespace {

std::map<int64_t, double> stats_of(const bs::PlayerSetup &setup) {
    std::map<int64_t, double> stats;
    for (const bs::StatArray *a : {&setup.stats.owner, &setup.stats.owner_proxy, &setup.stats.everyone_proxy}) {
        for (const bs::StatItem &it : a->items)
            stats[it.record] = std::bit_cast<float>(it.value);
    }
    return stats;
}

std::vector<aoc::protocol::repl::FastArrayDelta::Changed> items_of(const std::optional<bs::Send> &s, const aoc::protocol::emit::FIntrepidNetworkGUID &object, const char *cls, const char *field) {
    const auto fields = s ? fast_fields(s->emission.payload, object, cls) : std::nullopt;
    const aoc::protocol::repl::FastArrayField *f = fields ? field_named(*fields, cls, field) : nullptr;
    return f ? f->delta.changed : std::vector<aoc::protocol::repl::FastArrayDelta::Changed>{};
}

bool same_guid(const aoc::protocol::repl::Value *v, uint64_t id) {
    const aoc::protocol::emit::FIntrepidNetworkGUID *g = v ? v->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>() : nullptr;
    return g && g->ObjectId == id;
}

struct RetailItemSpec {
    int64_t ability = 0;
    const char *group = "";
    uint64_t montage = 0;
    uint16_t yaw = 0;
};

bool empty_array_field(const aoc::protocol::repl::ElementState &e, uint16_t handle) {
    const aoc::protocol::repl::Value *v = item_value(e, handle);
    if (!v) return true;
    const auto *arr = v->get_if<aoc::protocol::repl::ArrayValue>();
    return arr && arr->elements.empty();
}

bool retail_item_defaults(const aoc::protocol::repl::ElementState &e) {
    return empty_array_field(e, 3) && empty_array_field(e, 4) && !item_get<bool>(e, 5) && item_get<int64_t>(e, 6) == 0 && item_get<float>(e, 7) == 0.0f && item_get<float>(e, 8) == 0.0f && item_get<bool>(e, 9) &&
           item_get<float>(e, 12) == 1.0f && item_get<float>(e, 13) == 0.0f && !item_get<bool>(e, 14);
}

bool retail_item_shape(const aoc::protocol::repl::ElementState &e, const bs::PlayerSetup &setup, const RetailItemSpec &spec) {
    const auto *g = item_value(e, 10) ? item_value(e, 10)->get_if<aoc::protocol::repl::NameValue>() : nullptr;
    const auto *who = item_value(e, 17) ? item_value(e, 17)->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>() : nullptr;
    return retail_item_defaults(e) && item_get<int64_t>(e, 1) == spec.ability && g && g->text == spec.group && same_guid(item_value(e, 11), spec.montage) && item_get<uint64_t>(e, 16) == spec.yaw && who && *who == setup.guids.pawn;
}

bool retail_effect_shape(const aoc::protocol::repl::ElementState &e, const bs::PlayerSetup &setup, int64_t guid, float duration, float tick) {
    bool same_character = true;
    for (uint16_t i = 0; i < 4; ++i) {
        const int64_t word = static_cast<int32_t>(setup.character_guid[i]);
        same_character = same_character && item_get<int64_t>(e, static_cast<uint16_t>(7 + i)) == word && item_get<int64_t>(e, static_cast<uint16_t>(11 + i)) == word;
    }
    return item_get<int64_t>(e, 2) == guid && item_get<float>(e, 4) == duration && item_get<float>(e, 5) == tick && item_get<int64_t>(e, 6) == 1 && same_character && !item_get<bool>(e, 15) && item_get<int64_t>(e, 16) == 0;
}

} // namespace

static const StatInt32Inst *find_owner_stat_item(const std::vector<FastArrayDelta::Changed> &owner, int64_t record, int32_t id) {
  for (const auto &item : owner) {
    const StatInt32Inst *x = item_value(item.values, 1) ? item_value(item.values, 1)->get_if<StatInt32Inst>() : nullptr;
    if (x && x->record == record && item.id == id) {
      return x;
    }
  }
  return nullptr;
}

static void check_sprint_start_items(const FastArrayDelta::Changed &ability_item, const FastArrayDelta::Changed &speed_item, const FastArrayDelta::Changed &effect_item, const bs::PlayerSetup &setup) {
  CHECK(retail_item_shape(ability_item.values, setup, RetailItemSpec{content::kGuidSprint, "Sprint", 9309136633382094689ull, 0}),
        "the ability item: General_Sprint, MontageGroup \"Sprint\", Montage 9309136633382094689, nothing targeted, PvP 0, no progress, "
        "predicted on the autonomous client, PlayRate 1, MontagePosition 0, no jump, MeshYaw 0, Instigator the pawn");
  CHECK(item_get<int64_t>(ability_item.values, 2) == 0, "(its ServerInstanceId is the server's counter, 0: the retail one was 65 for ClientInstanceId 69)");
  const StatInt32Inst *mult = item_value(speed_item.values, 1) ? item_value(speed_item.values, 1)->get_if<StatInt32Inst>() : nullptr;
  CHECK(speed_item.id == 8 && mult && mult->record == kMoveSpeedMult && mult->value == std::bit_cast<uint32_t>(1.2f),
        "MoveSpeedMult: item 8 of StatRepInt32EveryoneProxy (retail's too), 1.2 = 1.0 + 0.2 (retail's characters had two skills: 1.3 = "
        "0x3fa66666)");
  CHECK(retail_effect_shape(effect_item.values, setup, kSprintEffect, AbilityRuntime::kInfiniteDuration, 1.0f) && item_get<double>(effect_item.values, 3) == bs::effect_clock(1000.0),
        "the effect item: General_Sprint, Duration FLT_MAX, TickTime 1.0, one stack, the character as instigator and source, no PvP "
        "force, SourceNode 0, StartTime on the synchronized clock");
}

static size_t check_retail_sprint_start_reply(const std::optional<bs::Send> &sprint, const bs::PlayerSetup &setup) {
  const FIntrepidNetworkGUID ability = setup.guids.ability;
  const FIntrepidNetworkGUID stats = setup.guids.stats;
  CHECK(sprint && sprint->role == bs::Role::Pawn && sprint->reliable(), "the Sprint's reply is ONE chain on the pawn's channel ...");
  const auto abilities = items_of(sprint, ability, "UAoCAbilityComponent", "ReplicatedActiveAbilities");
  const auto speed = items_of(sprint, stats, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
  const auto effects = items_of(sprint, stats, "UAoCStatsComponent", "CurrentEffects");
  const auto owner = items_of(sprint, stats, "UAoCStatsComponent", "StatRepInt32Owner");
  CHECK(abilities.size() == 1 && speed.size() == 1 && effects.size() == 1 && owner.size() == 2, "... with the ability item, the MoveSpeedMult item, the effect item, the first charge and StaminaRegeneration 0 (Owner items 172 "
                                                                                                "and 176): the same packet (the retail capture, t = 2.759)");
  const StatInt32Inst *regen = find_owner_stat_item(owner, content::kGuidStaminaRegeneration, 172);
  CHECK(regen && regen->value == std::bit_cast<uint32_t>(0.0f), "StaminaRegeneration is item 172 of the Owner array (retail's) and 0.0: nothing regenerates while the Sprint runs and the "
                                                                "player moves");
  if (abilities.size() == 1 && speed.size() == 1 && effects.size() == 1) {
    check_sprint_start_items(abilities[0], speed[0], effects[0], setup);
  }
  return speed.size();
}

static void check_retail_sprint_stop_reply(Pawn &p, Sim &s, const bs::PlayerSetup &setup) {
  const FIntrepidNetworkGUID stats = setup.guids.stats;
  s.run_to(2.0);
  for (const AbilityEvent &e : s.log) {
    if (&e == &s.log[0]) {
      continue;
    }
    if (!e.change.empty()) {
      p.pb.apply_ability_change(e.change);
    }
  }
  (void)p.pb.ability_update();
  s.cancel(69);
  const auto stop = p.update(s.log.back().change);
  const auto stop_speed = items_of(stop, stats, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
  const auto stop_effects = items_of(stop, stats, "UAoCStatsComponent", "CurrentEffects");
  const StatInt32Inst *back = stop_speed.size() == 1 && item_value(stop_speed[0].values, 1) ? item_value(stop_speed[0].values, 1)->get_if<StatInt32Inst>() : nullptr;
  CHECK(back && back->record == kMoveSpeedMult && back->value == std::bit_cast<uint32_t>(1.0f), "a release: one chain with MoveSpeedMult back to 1.0 (retail: 1.0 in the packet that removes the effect)");
  CHECK(stop_effects.size() == 1 && retail_effect_shape(stop_effects[0].values, setup, kDisableRegenEffect, 1.0f, 0.0f),
        "and the DisableRegen effect (Duration 1.0, no tick) added in the same chain (the removal of the Sprint effect is a delete)");
}

static void check_dodge_cooldown_and_evasion(const ElementState &cd, const ElementState &effect, const bs::PlayerSetup &setup) {
  CHECK(item_get<int64_t>(cd, 1) == content::kGuidDodge && item_get<bool>(cd, 2) && item_get<float>(cd, 3) == 0.65f && std::fabs((item_get<double>(cd, 5) - item_get<double>(cd, 4)) - 0.6499999761581421) < 1e-6 &&
            item_get<int64_t>(cd, 6) == 1 && item_get<double>(cd, 4) == bs::effect_clock(1000.5),
        "the Cooldowns item: General_Dodge, bCooldownTriggered, InitialCooldown 0.65, the end 0.65 after the start (retail: "
        "0.6499999762), Charges 1");
  CHECK(retail_effect_shape(effect, setup, kEvasionEffect, 0.5f, 0.0f) && item_get<double>(effect, 3) == item_get<double>(cd, 4),
        "the Evasion effect: Duration 0.5, no tick, StartTime the cooldown's start (retail: the same double)");
}

static void check_dodge_owner_stats(const std::optional<bs::Send> &dodge, const bs::PlayerSetup &setup, size_t sprint_speed_count) {
  const FIntrepidNetworkGUID stats = setup.guids.stats;
  const auto d_owner = items_of(dodge, stats, "UAoCStatsComponent", "StatRepInt32Owner");
  const StatInt32Inst *paid = find_owner_stat_item(d_owner, content::kGuidStamina, 176);
  const StatInt32Inst *d_regen = find_owner_stat_item(d_owner, content::kGuidStaminaRegeneration, 172);
  CHECK(d_owner.size() == 2 && paid && std::bit_cast<float>(paid->value) == static_cast<float>(stats_of(setup)[content::kGuidStamina] - 50), "Stamina is item 176 of the Owner array (retail's) and 50 lower");
  CHECK(d_regen && d_regen->value == std::bit_cast<uint32_t>(0.0f), "...and StaminaRegeneration (item 172) is 0 with the Dodge: the Evasion stops the regeneration [R: the retail capture]");
  CHECK(sprint_speed_count == 1 && items_of(dodge, stats, "UAoCStatsComponent", "StatRepInt32EveryoneProxy").empty(), "a Dodge does not touch MoveSpeedMult");
}

static void check_retail_dodge_reply(const Rules &r, const bs::PlayerSetup &setup, size_t sprint_speed_count) {
  const FIntrepidNetworkGUID ability = setup.guids.ability;
  const FIntrepidNetworkGUID stats = setup.guids.stats;
  Sim d(r.rules, stats_of(setup), 1000);
  d.run_to(0.5);
  d.trigger(content::kGuidDodge, 70, TriggerInfo{5, 1, 0});
  CHECK(d.log.size() == 1, "(a Dodge: one step)");
  if (d.log.size() != 1) {
    return;
  }
  Pawn q;
  const auto dodge = q.update(d.log[0].change);
  const auto d_abilities = items_of(dodge, ability, "UAoCAbilityComponent", "ReplicatedActiveAbilities");
  const auto d_cooldowns = items_of(dodge, ability, "UAoCAbilityComponent", "Cooldowns");
  const auto d_effects = items_of(dodge, stats, "UAoCStatsComponent", "CurrentEffects");
  CHECK(dodge && d_abilities.size() == 1 && d_cooldowns.size() == 1 && d_effects.size() == 1, "the Dodge's reply is one chain with the ability item, the Cooldowns item and the Evasion effect");
  if (!dodge || d_abilities.size() != 1 || d_cooldowns.size() != 1 || d_effects.size() != 1) {
    return;
  }
  CHECK(retail_item_shape(d_abilities[0].values, q.pb.setup(), RetailItemSpec{content::kGuidDodge, "DefaultGroup", 3289872098355792151ull, 0}),
        "the ability item: General_Dodge, MontageGroup \"DefaultGroup\", the forward montage (AnimationIndex 5), MeshYaw 0, the rest as "
        "the Sprint's");
  check_dodge_cooldown_and_evasion(d_cooldowns[0].values, d_effects[0].values, q.pb.setup());
  check_dodge_owner_stats(dodge, setup, sprint_speed_count);
}

static void test_retail_replies(const Rules &r) {
  std::printf("the retail server's Sprint and Dodge replies, field by field\n");
  Pawn p;
  const bs::PlayerSetup &setup = p.pb.setup();
  Sim s(r.rules, stats_of(setup), 1000);
  s.move(true);
  s.trigger(content::kGuidSprint, 69);
  CHECK(s.log.size() == 1 && s.log[0].change.stats.size() == 3, "(a Sprint: one step, with three stats: Stamina, MoveSpeedMult and StaminaRegeneration)");
  if (s.log.size() != 1)
    return;
  const auto sprint = p.update(s.log[0].change);
  const size_t sprint_speed_count = check_retail_sprint_start_reply(sprint, setup);
  check_retail_sprint_stop_reply(p, s, setup);
  check_retail_dodge_reply(r, setup, sprint_speed_count);
}

static void check_out_of_stamina_chain(const std::optional<bs::Send> &reply, const bs::PlayerSetup &setup) {
  const auto speed = items_of(reply, setup.guids.stats, "UAoCStatsComponent", "StatRepInt32EveryoneProxy");
  const auto effects = items_of(reply, setup.guids.stats, "UAoCStatsComponent", "CurrentEffects");
  const auto owner = items_of(reply, setup.guids.stats, "UAoCStatsComponent", "StatRepInt32Owner");
  const StatInt32Inst *mult = speed.size() == 1 && item_value(speed[0].values, 1) ? item_value(speed[0].values, 1)->get_if<StatInt32Inst>() : nullptr;
  CHECK(mult && mult->record == kMoveSpeedMult && mult->value == std::bit_cast<uint32_t>(1.0f), "the reply has MoveSpeedMult 1.0 (retail: 1.0 in the reply) ...");
  CHECK(effects.size() == 1 && retail_effect_shape(effects[0].values, setup, kDisableRegenEffect, 1.0f, 0.0f) && owner.size() == 2,
        "... the DisableRegen effect (and no Sprint effect), the Stamina left and StaminaRegeneration 0 (the pause), in that one chain");
}

static void test_out_of_stamina_reply(const Rules &r) {
  std::printf("a Sprint that starts with 1.25 Stamina: retail's reply\n");
  Pawn p;
  const bs::PlayerSetup &setup = p.pb.setup();
  auto st = stats_of(setup);
  st[content::kGuidStamina] = 1.25;
  Sim s(r.rules, st, 1000);
  s.move(true);
  s.trigger(content::kGuidSprint, 3);
  CHECK(s.log.size() == 2 && s.log[0].change.effects_added.size() == 1 && s.log[1].change.effects_removed.size() == 1 && s.log[1].change.effects_added.size() == 1, "the Sprint's start and its end are two steps of the runtime");
  if (s.log.size() != 2)
    return;
  bool applied = true;
  for (const AbilityEvent &e : s.log)
    applied = applied && p.pb.apply_ability_change(e.change);
  const auto reply = p.pb.ability_update();
  CHECK(applied && reply, "the pawn takes both, and sends one chain");
  check_out_of_stamina_chain(reply, setup);
  CHECK(s.ends().empty(), "ClientEndAbility has not gone with it");
  s.run_to(0.2);
  CHECK(s.ends() == std::vector<int32_t>{3} && s.log.back().end_ability == 3 && s.log.back().at > 0.16 && s.log.back().at < 0.17, "it follows 0.163 s later (retail: 163 ms), after the update");
}

static void track_chain_stamina(const std::optional<bs::Send> &chain, const FIntrepidNetworkGUID &stats, float &lowest, float &last) {
  for (const auto &item : items_of(chain, stats, "UAoCStatsComponent", "StatRepInt32Owner")) {
    const StatInt32Inst *v = item_value(item.values, 1) ? item_value(item.values, 1)->get_if<StatInt32Inst>() : nullptr;
    if (!v || v->record != content::kGuidStamina) {
      continue;
    }
    last = std::bit_cast<float>(v->value);
    lowest = std::min(lowest, last);
  }
}

static void test_client_never_sees_negative_stamina(const Rules &r) {
  std::printf("what the client reads back\n");
  Pawn p;
  const bs::PlayerSetup &setup = p.pb.setup();
  auto st = stats_of(setup);
  st[content::kGuidStamina] = 101;
  st[content::kGuidMaxStamina] = 101;
  Sim s(r.rules, st, 1000);
  s.move(true);
  s.trigger(content::kGuidSprint, 0);
  s.run_to(20.5);
  float lowest = 1000;
  float last = -1;
  size_t chains = 0;
  bool all_applied = true;
  for (const AbilityEvent &e : s.log) {
    if (e.change.empty()) {
      continue;
    }
    const auto chain = p.update(e.change);
    all_applied = all_applied && chain.has_value();
    ++chains;
    track_chain_stamina(chain, setup.guids.stats, lowest, last);
  }
  CHECK(all_applied && chains > 80 && lowest == 0.0f && last == 0.0f, "a bar of 101 run down through the pawn: every chain applies, and the lowest Stamina the client reads is 0 (never -0.25)");
}

static bool same_ability_core(const content::AbilityRule &a, const content::AbilityRule &b) {
  return a.guid == b.guid && a.name == b.name && a.held == b.held && a.requires_out_of_combat == b.requires_out_of_combat && a.min_stamina == b.min_stamina && a.cost_stat_guid == b.cost_stat_guid && a.cost_amount == b.cost_amount &&
         a.cost_requires_moving == b.cost_requires_moving;
}

static bool same_ability_rule(const content::AbilityRule &a, const content::AbilityRule &b) {
  return same_ability_core(a, b) && a.charge_interval == b.charge_interval && a.cooldown == b.cooldown && a.triggers_cooldown == b.triggers_cooldown && a.disable_during_cooldown == b.disable_during_cooldown &&
         a.targets_self == b.targets_self && a.effect_guid == b.effect_guid && a.montage_path == b.montage_path && a.cancel_section == b.cancel_section;
}

static bool same_effect_rule(const content::EffectRule &o, const content::EffectRule &e) {
  bool ok = o.name == e.name && o.infinite == e.infinite && o.duration == e.duration && o.tick_timer == e.tick_timer && o.stops_regen_standing == e.stops_regen_standing && o.stops_regen_moving == e.stops_regen_moving &&
            o.end_effect_guid == e.end_effect_guid && o.end_effect_duration == e.end_effect_duration && o.stat_mods.size() == e.stat_mods.size();
  for (size_t i = 0; ok && i < e.stat_mods.size(); ++i) {
    ok = o.stat_mods[i].stat_guid == e.stat_mods[i].stat_guid && o.stat_mods[i].source_stat_guid == e.stat_mods[i].source_stat_guid && o.stat_mods[i].factor == e.stat_mods[i].factor && o.stat_mods[i].addend == e.stat_mods[i].addend;
  }
  return ok;
}

static bool same_effect_table(const content::AbilityRules &rules, const content::AbilityRules &fixture) {
  bool ok = rules.effects.size() == fixture.effects.size();
  for (const auto &[guid, e] : fixture.effects) {
    const content::EffectRule *o = rules.effect(guid);
    ok = ok && o && same_effect_rule(*o, e);
  }
  return ok;
}

static void test_real_content_db(const Rules &fixture) {
  std::printf("the real content.db (AOC_CONTENT_DB)\n");
  const char *env = std::getenv("AOC_CONTENT_DB");
  if (!env || !*env) {
    std::printf("  [skip] AOC_CONTENT_DB is not set (cd services && go run ./cmd/contentc; AOC_CONTENT_DB=dist\\content\\content.db)\n");
    return;
  }
  std::string error;
  auto db = content::ContentDb::open(env, error);
  CHECK(db.has_value(), "the real content.db opens");
  if (!db) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  const auto rules = db->ability_rules(error);
  CHECK(rules.has_value(), "and its ability rules read");
  if (!rules) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  CHECK(same_ability_rule(rules->sprint, fixture.rules.sprint) && same_ability_rule(rules->dodge, fixture.rules.dodge) && same_ability_rule(rules->rest, fixture.rules.rest),
        "Sprint, Dodge and Rest are the numbers the other tests use (the fixture is contentc's output of the same rows)");
  CHECK(same_effect_table(*rules, fixture.rules) && rules->stat_regen_time == 1.0 && rules->regen_rate == 5.0, "so are the four effects and their stat mods, and the regeneration (5 every 1 s)");

  Sim s(*rules);
  s.move(true);
  s.trigger(content::kGuidSprint, 0);
  s.run_to(20.0);
  CHECK(!s.rt.sprinting() && s.rt.stamina() == 0.0 && s.ends() == std::vector<int32_t>{0}, "a Sprint on the real content.db empties the bar in 20 s and ends with ClientEndAbility");
  s.move(false);
  s.run_to(40.0);
  CHECK(s.rt.stamina() == 100.0, "and the bar is full again 20 s later");
}

static void check_gear_stats_tracked(Sim &s) {
  constexpr int64_t kStrength = 109183598614138ll;
  CHECK(s.rt.tracks(content::kGuidMaxStamina) && s.rt.tracks(kHealthRegen) && s.rt.tracks(kManaRegen) && s.rt.tracks(kMoveSpeedMult) && s.rt.tracks(kMaxHealth) && s.rt.tracks(kMaxMana),
        "the runtime holds MaxStamina, the two regenerations, MoveSpeedMult and the two maxima the Rest scales from");
  CHECK(!s.rt.tracks(content::kGuidStamina) && !s.rt.tracks(kStrength) && !s.rt.tracks(0), "it does not hold Stamina (it is the runtime's own, not the gear's) or an attribute");
  bs::AbilityChange none;
  s.rt.gear_stat(none, content::kGuidStamina, 10);
  CHECK(none.empty() && s.rt.stamina() == 100, "a gear value for Stamina is ignored: the runtime's current value is its own");
}

static void check_gear_stats_stamina_and_maxhp(const Rules &r) {
  Sim s(r.rules);
  check_gear_stats_tracked(s);

  bs::AbilityChange up;
  s.rt.gear_stat(up, content::kGuidMaxStamina, 150);
  CHECK(up.stats.size() == 1 && up.stats[0].record == content::kGuidMaxStamina && up.stats[0].value == 150.0f && s.rt.max_stamina() == 150 && s.rt.stamina() == 100,
        "MaxStamina 100 -> 150: the change names MaxStamina only, Stamina stays 100");

  bs::AbilityChange down;
  s.rt.gear_stat(down, content::kGuidMaxStamina, 60);
  CHECK(down.stats.size() == 2 && down.stats[0].record == content::kGuidMaxStamina && down.stats[0].value == 60.0f && down.stats[1].record == content::kGuidStamina && down.stats[1].value == 60.0f && s.rt.stamina() == 60 &&
            s.rt.max_stamina() == 60,
        "MaxStamina 150 -> 60 with Stamina 100: Stamina is clamped to 60 and says so in the change");
  s.trigger(content::kGuidDodge, 0);
  CHECK(s.rt.stamina() == 10, "(the runtime spends and regenerates from the clamped value)");

  bs::AbilityChange maxhp;
  s.rt.gear_stat(maxhp, kMaxHealth, 190);
  CHECK(maxhp.stats.size() == 1 && maxhp.stats[0].value == 190.0f && s.rt.stat(kMaxHealth) == 190, "MaxHealth 190 is the runtime's too");
}

static void check_gear_stats_rest_and_sprint(const Rules &r) {
  {
    Sim s(r.rules);
    s.trigger(content::kGuidRest, 0);
    s.run_to(2.0);
    CHECK(s.rt.rest_effect_on() && std::fabs(s.rt.stat(kHealthRegen) - 2.5) < 1e-6, "(the Rest effect is on: the regeneration is 0.6 + 0.01 x 90 + 1 = 2.5)");
    bs::AbilityChange c;
    s.rt.gear_stat(c, kHealthRegen, 0.9);
    CHECK(c.stats.size() == 1 && c.stats[0].record == kHealthRegen && std::fabs(c.stats[0].value - 2.8f) < 1e-5 && std::fabs(s.rt.stat(kHealthRegen) - 2.8) < 1e-5,
          "the base 0.6 -> 0.9 under the Rest: the pawn shows 2.8, the raise of 1.9 stays on top");
    s.cancel(0);
    s.run_to(2.1);
    CHECK(!s.rt.rest_effect_on() && std::fabs(s.rt.stat(kHealthRegen) - 0.9) < 1e-9, "when the effect ends the regeneration is the new base 0.9, not the old 0.6");
    const bs::AbilityChange &end = s.log.back().change;
    bool back = false;
    for (const bs::StatChange &sc : end.stats)
      back = back || (sc.record == kHealthRegen && std::fabs(sc.value - 0.9f) < 1e-6);
    CHECK(back, "and the update that ends the effect says 0.9");
  }
  {
    Sim s(r.rules);
    s.move(true);
    s.trigger(content::kGuidSprint, 0);
    s.run_to(0.5);
    CHECK(s.rt.sprinting() && std::fabs(s.rt.stat(kMoveSpeedMult) - 1.2) < 1e-9, "(the Sprint is on: MoveSpeedMult 1.2)");
    bs::AbilityChange c;
    s.rt.gear_stat(c, kMoveSpeedMult, 1.1);
    CHECK(std::fabs(s.rt.stat(kMoveSpeedMult) - 1.3) < 1e-9, "a gear speed of 1.1 under the Sprint shows 1.3");
    s.cancel(0);
    CHECK(std::fabs(s.rt.stat(kMoveSpeedMult) - 1.1) < 1e-9, "and the Sprint ends on 1.1");
  }
}

static void test_gear_stats(const Rules &r) {
  std::printf("gear: a change of a stat the runtime holds\n");
  check_gear_stats_stamina_and_maxhp(r);
  check_gear_stats_rest_and_sprint(r);
}

int main() {
  const Rules r;
  test_gear_stats(r);
  test_retail_replies(r);
  test_out_of_stamina_reply(r);
  test_client_never_sees_negative_stamina(r);
  test_real_content_db(r);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
