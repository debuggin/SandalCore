// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_abilities_common.h"

#include "capture_bootstrap.h"

using namespace aoc::worldd;
using namespace aoc::protocol;
using namespace aoc::protocol::repl;

namespace {

struct Ref {
    const char *name;
    size_t bits;
    const char *hex;
};
template <class B> Ref ref(const B &b) { return {b.name, b.bits, b.hex}; }

bool same_as(const aoc::protocol::emit::BunchWriter &w, const Ref &b) {
    const auto want = capture_blocks::bytes_of(b.hex);
    size_t diff = 0;
    if (w.bit_pos() != b.bits) {
        std::printf("    wrote %zu bits, expected %zu (%s)", w.bit_pos(), b.bits, b.name);
        const size_t n = w.bit_pos() < b.bits ? w.bit_pos() : b.bits;
        if (!capture_blocks::same_bit_range(w.data(), want.data(), n, &diff)) std::printf("; first differing bit %zu", diff);
        std::printf("\n");
        return false;
    }
    if (!capture_blocks::same_bit_range(w.data(), want.data(), b.bits, &diff)) {
        std::printf("    first differing bit %zu (%s)\n", diff, b.name);
        return false;
    }
    return true;
}

template <class B> double captured_start_time(const B &b, const bs::PlayerSetup &setup) {
    aoc::protocol::emit::BunchWriter w;
    const auto bytes = capture_blocks::bytes_of(b.hex);
    w.write_bit_range(bytes.data(), 0, b.bits);
    const auto fields = fast_fields(w, setup.guids.stats, "UAoCStatsComponent");
    const aoc::protocol::repl::FastArrayField *fx = fields ? field_named(*fields, "UAoCStatsComponent", "CurrentEffects") : nullptr;
    if (!fx || fx->delta.changed.size() != 1) return 0;
    const auto it = fx->delta.changed[0].values.values.find(3);
    return it != fx->delta.changed[0].values.values.end() && it->second.get_if<double>() ? *it->second.get_if<double>() : 0;
}

bs::ActiveAbilityItem rest_item(int64_t instance, int32_t counter, float position = 0, bool jump = false) {
    bs::ActiveAbilityItem a;
    a.ability_guid = content::kGuidRest;
    a.server_instance_id = instance;
    a.targets_self = true;
    a.montage_path = std::string(bs::kRestMontagePath);
    a.montage_position = position;
    a.may_jump_to_position = jump;
    a.animation_counter = counter;
    return a;
}

bs::EffectItem rest_effect(int64_t id, double start) {
    bs::EffectItem e;
    e.effect_id = id;
    e.effect_guid = kRestEffect;
    e.start_time = start;
    e.duration = aoc::worldd::AbilityRuntime::kInfiniteDuration;
    e.tick_time = 0;
    e.stacks = 1;
    return e;
}

void regen_to(bs::AbilityChange &c, float v) {
    c.stats.push_back({kHealthRegen, v});
    c.stats.push_back({kManaRegen, v});
}

} // namespace

static void check_rest_start(Sim &s, const Rules &r) {
  CHECK(s.trigger(content::kGuidRest, 0) == Refusal::None && s.rt.resting(), "Rest starts (a toggle, out of combat)");
  const auto start = s.with("the Rest starts");
  CHECK(start.size() == 1 && start[0]->change.abilities_added.size() == 1 && start[0]->change.abilities_added[0].targets_self && start[0]->change.abilities_added[0].animation_counter == 0 &&
            start[0]->change.abilities_added[0].montage_path == r.rules.rest.montage_path && start[0]->change.effects_added.empty() && start[0]->change.stats.empty(),
        "first only the ability item: the caster as target, the Rest montage, AnimationCounter 0");
  s.run_to(1.9);
  CHECK(!s.rt.rest_effect_on(), "the effect is not there during the sit-down");
}

static void check_rest_effect(Sim &s) {
  s.run_to(2.0);
  const auto effect = s.with("the Rest effect starts");
  CHECK(s.rt.rest_effect_on() && effect.size() == 1 && effect[0]->change.effects_added.size() == 1 && effect[0]->change.effects_added[0].effect_guid == kRestEffect && effect[0]->change.effects_added[0].effect_id == 0,
        "after kRestEffectDelay (2 s, inside the capture's 1.77 and 2.04) the Rest effect is added (EffectId 0)");
  CHECK(effect[0]->change.stats.size() == 2 && effect[0]->change.stats[0].record == kHealthRegen && effect[0]->change.stats[0].value == 2.5f && effect[0]->change.stats[1].record == kManaRegen && effect[0]->change.stats[1].value == 2.5f,
        "with it HealthRegeneration and ManaRegeneration go from 0.6 to 2.5 (0.6 + 0.01 x 90 + 1), as pkt#8228");
  CHECK(effect[0]->change.effects_added[0].start_time == bs::effect_clock(1000.0 + 2.0) && effect[0]->change.effects_added[0].tick_time == 0.0f, "its StartTime is the synchronized clock at that moment");
}

static void check_rest_cancel(Sim &s) {
  CHECK(s.cancel(0) && !s.rt.resting(), "ServerNotifyHeldAbilityCancelled(0) releases the Rest");
  const auto jump = s.with("the Rest is released");
  CHECK(jump.size() == 1 && jump[0]->change.abilities_changed.size() == 1 && jump[0]->change.abilities_changed[0].may_jump_to_position && jump[0]->change.abilities_changed[0].montage_position == 5.82606792f &&
            jump[0]->change.abilities_changed[0].animation_counter == 1 && jump[0]->change.abilities_removed.empty(),
        "the ability item changes: MontagePosition 5.826 (SitEnd), bMayJumpToPosition, AnimationCounter 1; it stays, as in the capture");
  CHECK(s.rt.rest_effect_on(), "the effect is still on");
  s.run_to(2.1);
  const auto gone = s.with("the Rest effect ends");
  CHECK(!s.rt.rest_effect_on() && gone.size() == 1 && gone[0]->change.effects_removed == std::vector<int64_t>{0} && gone[0]->change.stats.size() == 2 && gone[0]->change.stats[0].value == k06 && gone[0]->change.stats[1].value == k06,
        "0.067 s later the effect is removed and the regeneration is back to 0.6 (pkt#8318 -> #8320)");
}

static void check_rest_replace(const Rules &r) {
  Sim again(r.rules);
  again.trigger(content::kGuidRest, 0);
  again.run_to(2.5);
  again.trigger(content::kGuidRest, 1);
  const auto replaced = again.with("the Rest starts");
  CHECK(replaced.size() == 2 && replaced[1]->change.abilities_removed == std::vector<int64_t>{0} && replaced[1]->change.abilities_added.size() == 1 && replaced[1]->change.abilities_added[0].server_instance_id == 1 &&
            replaced[1]->change.abilities_added[0].animation_counter == 2 && replaced[1]->change.effects_removed == std::vector<int64_t>{0} && replaced[1]->change.stats.size() == 2 && replaced[1]->change.stats[0].value == k06,
        "one step: instance 0 removed, instance 1 added with AnimationCounter 2, the effect removed, the regeneration back to 0.6");
  again.run_to(4.6);
  const auto second = again.with("the Rest effect starts");
  CHECK(second.size() == 2 && second[1]->change.effects_added[0].effect_id == 1 && second[1]->change.stats[0].value == 2.5f, "the second effect (EffectId 1, ids are never reused) comes 2 s after it");
}

static void test_rest(const Rules &r) {
  std::printf("Rest\n");
  Sim s(r.rules);
  check_rest_start(s, r);
  check_rest_effect(s);
  check_rest_cancel(s);
  check_rest_replace(r);

  Sim fight(r.rules);
  fight.rt.set_in_combat(true, 0);
  CHECK(fight.trigger(content::kGuidRest, 0) == Refusal::InCombat && fight.ends() == std::vector<int32_t>{0}, "in combat a Rest is refused with ClientEndAbility");
  Sim d(r.rules);
  d.trigger(content::kGuidRest, 0);
  d.run_to(3.0);
  d.trigger(content::kGuidDodge, 1);
  CHECK(!d.rt.resting() && d.with("the Rest ends").size() == 1, "a Dodge ends a Rest (the item jumps to its end section)");
}

static void check_mounted_sprint_cancel(Sim &s) {
  s.move(true);
  CHECK(s.trigger(content::kGuidSprint, 31) == Refusal::None && s.rt.sprinting(), "(a Sprint runs, the player moves)");
  s.run_to(2.0);
  const double stamina_before = s.rt.stamina();
  CHECK(stamina_before < 100.0 && s.rt.stat(kMoveSpeedMult) == 1.2, "(it has taken Stamina, MoveSpeedMult is 1.2)");
  const size_t events_before = s.log.size();
  s.rt.set_mounted(true, s.now);
  s.take();
  CHECK(s.rt.mounted() && !s.rt.sprinting(), "getting on ends the Sprint");
  CHECK(s.ends() == ints({31}), "with ClientEndAbility(the client's instance) at once (not 163 ms later)");
  const auto ending = s.with("gets on a mount");
  CHECK(ending.size() == 1 && ending[0]->change.abilities_removed.size() == 1 && ending[0]->change.effects_removed.size() == 1 && ending[0]->change.effects_added.empty() && stat_of(ending[0]->change, kMoveSpeedMult) == 1.0f &&
            s.rt.stat(kMoveSpeedMult) == 1.0,
        "the way a Sprint ends: its ability item and effect removed, MoveSpeedMult back to 1.0, and no DisableRegen effect (no "
        "regeneration pause for a rider)");
  CHECK(s.log.size() > events_before && s.rt.regen_pause_until() < 0, "(the exertion pause did not start)");
  s.move(true);
  s.run_to(6.0);
  CHECK(!s.rt.moving(s.now), "a mounted rider never counts as moving");
  CHECK(s.rt.stamina() >= stamina_before, "four more seconds mounted take no Stamina (they give it back, standing)");
}

static void test_mounted(const Rules &r) {
  std::printf("mounted: no Sprint, no Stamina use\n");
  {
    Sim s(r.rules);
    check_mounted_sprint_cancel(s);
    const size_t before = s.log.size();
    const Refusal sprint = s.trigger(content::kGuidSprint, 32);
    const Refusal dodge = s.trigger(content::kGuidDodge, 33);
    const Refusal rest = s.trigger(content::kGuidRest, 34);
    CHECK(sprint == Refusal::Mounted && dodge == Refusal::Mounted && rest == Refusal::Mounted && !s.rt.sprinting() && !s.rt.evading() && !s.rt.resting(), "a Sprint, a Dodge and a Rest asked for while mounted are refused (Mounted)");
    CHECK(s.ends() == ints({31, 32, 33, 34}) && s.log.size() == before + 3, "each is answered with ClientEndAbility and nothing else");
    CHECK(s.rt.stamina() == s.rt.stat(content::kGuidStamina) && s.rt.stat(kMoveSpeedMult) == 1.0, "(nothing changed on the pawn)");
    s.rt.set_mounted(false, s.now);
    s.take();
    CHECK(!s.rt.mounted() && !s.rt.sprinting(), "getting off does not bring the Sprint back");
    CHECK(s.trigger(content::kGuidSprint, 35) == Refusal::None && s.rt.sprinting(), "but a new Sprint starts");
  }
  {
    Sim s(r.rules);
    CHECK(s.trigger(content::kGuidRest, 40) == Refusal::None && s.rt.resting(), "(a Rest runs)");
    s.run_to(3.0);
    s.rt.set_mounted(true, s.now);
    s.take();
    CHECK(!s.rt.resting(), "getting on ends a Rest");
  }
  {
    Sim s(r.rules);
    s.rt.set_mounted(true, s.now);
    s.rt.set_mounted(true, s.now);
    s.take();
    CHECK(s.log.empty() && s.rt.mounted(), "getting on twice is the same as once");
  }
}

static void check_missing_stat_refusals(const Rules &r) {
  auto missing = level1_stats();
  missing.erase(content::kGuidMaxStamina);
  AbilityRuntime bad(r.rules, missing, 0);
  CHECK(!bad.ok() && contains(bad.error(), "MaxStamina"), "a character with no MaxStamina: ok() is false and says which stat");
  CHECK(bad.trigger(content::kGuidSprint, 0, 0) == Refusal::NotEnabled && bad.take_events().empty(), "(and it runs nothing)");
  auto no_regen = level1_stats();
  no_regen.erase(kHealthRegen);
  AbilityRuntime bad2(r.rules, no_regen, 0);
  CHECK(!bad2.ok() && contains(bad2.error(), "regeneration"), "so with no HealthRegeneration (the Rest effect raises it)");
  auto no_speed = level1_stats();
  no_speed.erase(kMoveSpeedMult);
  AbilityRuntime bad3(r.rules, no_speed, 0);
  CHECK(!bad3.ok() && contains(bad3.error(), "MoveSpeedMult"), "and with no MoveSpeedMult (the Sprint's effect raises it)");
}

static void test_other_abilities(const Rules &r) {
  std::printf("anything else is refused\n");
  Sim s(r.rules);
  CHECK(s.trigger(kScepterCombo, 0) == Refusal::NotEnabled && s.ends() == std::vector<int32_t>{0} && s.log.size() == 1 && s.log[0].change.empty(), "a combat ability: ClientEndAbility(ClientInstanceId) and nothing else");
  CHECK(s.trigger(kGuard, 4) == Refusal::NotEnabled && s.ends() == ints({0, 4}), "General_Guard too");
  CHECK(s.trigger(0, 5) == Refusal::NotEnabled && s.trigger(-1, 6) == Refusal::NotEnabled && s.ends() == ints({0, 4, 5, 6}), "and an ability that does not exist");
  CHECK(s.rt.refused() == 4 && s.rt.stamina() == 100.0 && !s.rt.sprinting() && !s.rt.resting() && !s.rt.evading(), "none of them changes anything");
  CHECK(!s.rt.enabled(kScepterCombo) && s.rt.enabled(content::kGuidSprint) && s.rt.enabled(content::kGuidDodge) && s.rt.enabled(content::kGuidRest), "enabled(): the whitelist is Sprint, Dodge and Rest");
  check_missing_stat_refusals(r);
}

static void test_instances(const Rules &r) {
  std::printf("ServerInstanceId: the server's own counter\n");
  Sim s(r.rules, [] {
    auto m = level1_stats();
    m[content::kGuidStamina] = 300;
    m[content::kGuidMaxStamina] = 300;
    return m;
  }());
  s.move(true);
  s.trigger(content::kGuidSprint, 40);
  s.trigger(kScepterCombo, 41);
  s.trigger(content::kGuidDodge, 42, TriggerInfo{5, 1, 0});
  const auto sprint = s.with("the Sprint starts");
  const auto dodge = s.with("the Dodge starts");
  CHECK(sprint.size() == 1 && sprint[0]->change.abilities_added[0].server_instance_id == 0 && dodge.size() == 1 && dodge[0]->change.abilities_added[0].server_instance_id == 1,
        "the Sprint (ClientInstanceId 40) is instance 0 and the Dodge (42) instance 1; the refused trigger (41) took none");
  CHECK(s.rt.instance_of(40) == 0 && s.rt.instance_of(42) == 1 && !s.rt.instance_of(41) && !s.rt.instance_of(0) && !s.rt.instance_of(1), "the runtime knows each running instance by the client's id, never by the server's");
  s.run_to(1.5);
  s.trigger(content::kGuidDodge, 43, TriggerInfo{5, 1, 0});
  const auto dodges = s.with("the Dodge starts");
  CHECK(dodges.size() == 2 && dodges[1]->change.abilities_added[0].server_instance_id == 2, "the next instance is 2: the counter never goes back or repeats");
  CHECK(!s.cancel(0) && !s.cancel(1) && s.rt.sprinting(), "a cancel names the client's id: 0 and 1, instance numbers, release nothing");
  CHECK(s.cancel(40) && !s.rt.sprinting(), "a cancel of the Sprint's ClientInstanceId (40) does");
  Sim e(r.rules, [] {
    auto m = level1_stats();
    m[content::kGuidStamina] = 1.25;
    return m;
  }());
  e.move(true);
  e.trigger(kScepterCombo, 9);
  e.trigger(content::kGuidSprint, 17);
  e.run_to(0.5);
  CHECK(e.ends() == ints({9, 17}) && e.with("the Sprint starts")[0]->change.abilities_added[0].server_instance_id == 0, "ClientEndAbility carries the client's id (17 for the Sprint that ran out, instance 0), after the refusal's (9)");
}

static void check_rest_golden_first_sit(Pawn &p, double start1) {
  {
    bs::AbilityChange c;
    c.abilities_added.push_back(rest_item(0, 0));
    const auto s = p.update(c);
    CHECK(s && s->role == bs::Role::Pawn && !s->opens_channel && s->reliable(), "pkt#8175: an ability update is one reliable chain on the pawn's channel");
    CHECK(s && s->emission.exports.size() == 1 && same_as(s->emission.exports[0], ref(capture_bootstrap::kRestAbilityAddExports0)),
          "the montage's export bunch (CM_M_ABLT_General_Rest_Montage and its package, 1,345 bits) equals the original's bit for bit");
    CHECK(s && same_as(s->emission.payload, ref(capture_bootstrap::kRestAbilityAddContent)), "ReplicatedActiveAbilities key 1 base 0, instance 0 (the caster as target, the montage, Instigator the pawn): 1,253 bits, "
                                                                                             "bit for bit");
    if (s)
      p.pb.mark_exported(s->emission.exported_by_bunch[0]);
  }
  {
    bs::AbilityChange c;
    regen_to(c, 2.5f);
    c.effects_added.push_back(rest_effect(0, start1));
    const auto s = p.update(c);
    CHECK(s && s->emission.exports.empty() && same_as(s->emission.payload, ref(capture_bootstrap::kRestStats8228)),
          "pkt#8228: Owner 692 against 690 (ids 15 and 19 to 2.5) and CurrentEffects key 1 base 0 (EffectId 0): 1,343 bits, bit for bit");
  }
}

static void check_rest_golden_replace_and_stand(Pawn &p, double start2) {
  {
    bs::AbilityChange c;
    regen_to(c, k06);
    c.effects_removed.push_back(0);
    c.abilities_removed.push_back(0);
    c.abilities_added.push_back(rest_item(1, 2));
    const auto s = p.update(c);
    CHECK(s && s->emission.exports.empty() && same_as(s->emission.payload, ref(capture_bootstrap::kRestBoth8240)), "pkt#8240: the ability delete and add (key 1 -> 4), the regeneration back (694 against 692), the effect delete (key 3 "
                                                                                                                   "against 1): both blocks, 2,019 bits, bit for bit");
  }
  {
    bs::AbilityChange c;
    regen_to(c, 2.5f);
    c.effects_added.push_back(rest_effect(1, start2));
    const auto s = p.update(c);
    CHECK(s && same_as(s->emission.payload, ref(capture_bootstrap::kRestStats8301)), "pkt#8301: the second effect (EffectId 1, element ID 2): 1,343 bits, bit for bit");
  }
  {
    bs::AbilityChange c;
    c.abilities_changed.push_back(rest_item(1, 3, 5.82606792f, true));
    const auto s = p.update(c);
    CHECK(s && s->emission.exports.empty() && same_as(s->emission.payload, ref(capture_bootstrap::kRestAbility8318)),
          "pkt#8318: MontagePosition 5.826, bMayJumpToPosition, AnimationCounter 3 (key 5 against 4): 1,253 bits, bit for bit (the "
          "export is acknowledged)");
  }
  {
    bs::AbilityChange c;
    regen_to(c, k06);
    c.effects_removed.push_back(1);
    const auto s = p.update(c);
    CHECK(s && same_as(s->emission.payload, ref(capture_bootstrap::kRestStats8320)), "pkt#8320: the effect delete (key 6 against 4) and the regeneration back: 734 bits, bit for bit");
    CHECK(!p.pb.ability_update(), "and with nothing changed there is no update");
  }
}

static void test_rest_golden() {
  std::printf("General_Rest on the wire (pkt#8175-8320)\n");
  Pawn p;
  const bs::PlayerSetup &setup = p.pb.setup();
  const double start1 = captured_start_time(capture_bootstrap::kRestStats8228, setup);
  const double start2 = captured_start_time(capture_bootstrap::kRestStats8301, setup);
  CHECK(start1 > 15061867.6 && start1 < 15061867.8 && start2 > 15061870.0 && start2 < 15061870.2, "(the captured effects' StartTimes: 15,061,867.7 and 15,061,870.1)");
  check_rest_golden_first_sit(p, start1);
  check_rest_golden_replace_and_stand(p, start2);
}

static void check_stamina_delta_item(const FastArrayField *owner, const std::optional<bs::Send> &s) {
  const StatInt32Inst *item = owner && owner->delta.changed.size() == 1 && owner->delta.changed[0].values.values.count(1) ? owner->delta.changed[0].values.values.at(1).get_if<StatInt32Inst>() : nullptr;
  CHECK(item && item->record == content::kGuidStamina && item->value == std::bit_cast<uint32_t>(98.75f) && owner->delta.changed[0].id == 176, "the item is Stamina (element ID 176) with the new value as float bits");
  CHECK(s && s->emission.payload.bit_pos() > 0, "(and the chain's payload is the block alone)");
}

static void check_stamina_delta_first_step(Pawn &p) {
  bs::AbilityChange c;
  c.stats.push_back({content::kGuidStamina, 98.75f});
  const auto s = p.update(c);
  CHECK(s && s->emission.exports.empty() && s->reliable() && s->role == bs::Role::Pawn, "a Stamina step is one reliable chain, with no export");
  const auto fields = s ? fast_fields(s->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
  const FastArrayField *owner = fields ? field_named(*fields, "UAoCStatsComponent", "StatRepInt32Owner") : nullptr;
  CHECK(fields && fields->size() == 1 && owner && owner->delta.base_key == 690 && owner->delta.array_key == 691 && owner->delta.deleted.empty() && owner->delta.changed.size() == 1,
        "it is the StatsComponent's StatRepInt32Owner alone: key 691 against 690, one item changed");
  check_stamina_delta_item(owner, s);
}

static void test_stamina_delta() {
  std::printf("a Stamina delta\n");
  Pawn p;
  check_stamina_delta_first_step(p);

  bs::AbilityChange c;
  c.stats.push_back({content::kGuidStamina, 98.75f});
  CHECK(p.pb.apply_ability_change(c) && !p.pb.ability_update(), "the same value again is no change: nothing is sent");
  c = {};
  c.stats.push_back({content::kGuidStamina, 97.5f});
  c.stats.push_back({content::kGuidStamina, 96.25f});
  const auto two = p.update(c);
  const auto f2 = two ? fast_fields(two->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
  const FastArrayField *o2 = f2 ? field_named(*f2, "UAoCStatsComponent", "StatRepInt32Owner") : nullptr;
  CHECK(o2 && o2->delta.changed.size() == 1 && o2->delta.base_key == 691 && o2->delta.array_key == 693 && o2->delta.changed[0].values.values.at(1).get_if<StatInt32Inst>()->value == std::bit_cast<uint32_t>(96.25f),
        "two changes of one stat in a step go out as the last value (the key moved by two)");
  c = {};
  c.stats.push_back({109343399171812ll, 1.0f});
  const auto prox = p.update(c);
  const auto f3 = prox ? fast_fields(prox->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
  CHECK(f3 && field_named(*f3, "UAoCStatsComponent", "StatRepInt32OwnerProxy") && !field_named(*f3, "UAoCStatsComponent", "StatRepInt32Owner"), "MagicalEvasion is in StatRepInt32OwnerProxy and goes out there");
}

struct BucketSeen {
  bool item = false;
  bool extended = false;
  uint32_t value = 0;
  uint32_t base = 0;
  uint32_t equipment = 0;
};

static bs::AbilityChange make_bucket_change(int64_t record, float value, float base, float equipment, bool buckets = true) {
  bs::AbilityChange c;
  bs::StatChange sc;
  sc.record = record;
  sc.value = value;
  sc.buckets = buckets;
  sc.base = base;
  sc.equipment = equipment;
  c.stats.push_back(sc);
  return c;
}

static BucketSeen seen_bucket_stat(const std::optional<bs::Send> &s, const Pawn &p, int64_t record) {
  BucketSeen out;
  const auto fields = s ? fast_fields(s->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
  const FastArrayField *owner = fields ? field_named(*fields, "UAoCStatsComponent", "StatRepInt32Owner") : nullptr;
  if (!owner)
    return out;
  for (const auto &el : owner->delta.changed) {
    const auto found = el.values.values.find(1);
    const StatInt32Inst *it = found != el.values.values.end() ? found->second.get_if<StatInt32Inst>() : nullptr;
    if (it && it->record == record)
      out = {true, it->extended, it->value, it->base, it->equipment};
  }
  return out;
}

static void check_bucket_stat_base_then_equip(Pawn &p, int64_t kHook) {
  auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
  const auto base_only = p.update(make_bucket_change(kHook, 100.0f, 20.0f, 150.0f));
  BucketSeen a = seen_bucket_stat(base_only, p, kHook);
  CHECK(base_only && a.item && a.value == bits(100.0f) && a.base == bits(20.0f) && a.equipment == bits(150.0f), "a change of the BASE alone (value and equipment as they were) is sent, and the item's base is the new one");
  CHECK(p.pb.apply_ability_change(make_bucket_change(kHook, 100.0f, 20.0f, 150.0f)) && !p.pb.ability_update(), "(and then it is no change)");

  const auto equip_only = p.update(make_bucket_change(kHook, 100.0f, 20.0f, 350.0f));
  a = seen_bucket_stat(equip_only, p, kHook);
  CHECK(equip_only && a.item && a.value == bits(100.0f) && a.base == bits(20.0f) && a.equipment == bits(350.0f), "a change of the EQUIPMENT alone is sent, with the base kept");
}

static void check_bucket_stat_base_and_equip(Pawn &p, int64_t kHook) {
  auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
  const auto first = p.update(make_bucket_change(kHook, 100.0f, 10.0f, 150.0f));
  BucketSeen a = seen_bucket_stat(first, p, kHook);
  CHECK(first && a.item && a.extended && a.value == bits(100.0f) && a.base == bits(10.0f) && a.equipment == bits(150.0f), "(Hook_Rate is an extended item: value 100, base 10, equipment 150 are written)");
  CHECK(p.pb.apply_ability_change(make_bucket_change(kHook, 100.0f, 10.0f, 150.0f)) && !p.pb.ability_update(), "the same three numbers again: no change, nothing sent");
  check_bucket_stat_base_then_equip(p, kHook);
}

static void test_bucket_stat_changes() {
  std::printf("a stat item with base and equipment: each of the three numbers alone is a change\n");
  constexpr int64_t kHook = 6064634285519601665ll;
  Pawn p;
  check_bucket_stat_base_and_equip(p, kHook);
  auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };

  const auto zero = p.update(make_bucket_change(kHook, 100.0f, 0.0f, 350.0f));
  CHECK(zero && seen_bucket_stat(zero, p, kHook).base == bits(0.0f), "(a base of +0.0)");
  const auto neg_zero = p.update(make_bucket_change(kHook, 100.0f, -0.0f, 350.0f));
  CHECK(neg_zero && seen_bucket_stat(neg_zero, p, kHook).base == bits(-0.0f), "a base of -0.0 after +0.0 is a change (the float bits differ)");

  const auto value_only = p.update(make_bucket_change(kHook, 90.0f, -0.0f, 350.0f));
  BucketSeen a = seen_bucket_stat(value_only, p, kHook);
  CHECK(value_only && a.item && a.value == bits(90.0f) && a.base == bits(-0.0f) && a.equipment == bits(350.0f), "a change of the value alone is sent, base and equipment kept");

  const auto plain = p.update(make_bucket_change(kHook, 80.0f, 5.0f, 5.0f, false));
  a = seen_bucket_stat(plain, p, kHook);
  CHECK(plain && a.item && a.value == bits(80.0f) && a.base == bits(-0.0f) && a.equipment == bits(350.0f), "a change without buckets writes the value and not the base and equipment");
}

static void check_cooldown_element_values(const FastArrayField *f) {
  const ElementState *e = f && !f->delta.changed.empty() ? &f->delta.changed[0].values : nullptr;
  const bool ok = e && item_value(*e, 1) && *item_value(*e, 1) == Value::i64(content::kGuidDodge) && item_value(*e, 2) && *item_value(*e, 2) == Value::boolean(true) && item_get<float>(*e, 3) == 0.65f &&
                  item_get<double>(*e, 4) == 13809071.207829274 && item_get<double>(*e, 5) == 13809071.85782925 && item_value(*e, 6) && *item_value(*e, 6) == Value::i64(1);
  CHECK(ok, "it reads back: AbilityGuid, bCooldownTriggered, InitialCooldown, the synchronized start and end times (doubles), Charges 1 "
            "(retail's)");
}

static void check_cooldown_item_added(const std::optional<bs::Send> &s, const Pawn &p) {
  CHECK(s && s->emission.exports.empty(), "a Cooldowns item goes out on its own, with no export");
  const auto fields = s ? fast_fields(s->emission.payload, p.pb.setup().guids.ability, "UAoCAbilityComponent") : std::nullopt;
  const FastArrayField *f = fields ? field_named(*fields, "UAoCAbilityComponent", "Cooldowns") : nullptr;
  CHECK(fields && fields->size() == 1 && f && f->delta.array_key == 1 && f->delta.base_key == 0 && f->delta.changed.size() == 1 && f->delta.deleted.empty(), "Cooldowns key 1 against 0 with one new item");
  check_cooldown_element_values(f);
}

static void test_cooldown_item() {
  std::printf("the Cooldowns item (the retail Dodge reply's layout)\n");
  Pawn p;
  bs::AbilityChange c;
  bs::CooldownItem cd;
  cd.ability_guid = content::kGuidDodge;
  cd.triggered = true;
  cd.initial_cooldown = 0.65f;
  cd.start_time = 13809071.207829274;
  cd.end_time = 13809071.85782925;
  cd.charges = 1;
  c.cooldowns_set.push_back(cd);
  check_cooldown_item_added(p.update(c), p);

  c = {};
  c.cooldowns_removed.push_back(content::kGuidDodge);
  const auto gone = p.update(c);
  const auto f2 = gone ? fast_fields(gone->emission.payload, p.pb.setup().guids.ability, "UAoCAbilityComponent") : std::nullopt;
  const FastArrayField *g = f2 ? field_named(*f2, "UAoCAbilityComponent", "Cooldowns") : nullptr;
  CHECK(g && g->delta.base_key == 1 && g->delta.array_key == 3 && g->delta.deleted.size() == 1 && g->delta.changed.empty(), "its removal is a delete (key 3 against 1)");

  c = {};
  c.cooldowns_set.push_back(cd);
  const auto again = p.update(c);
  const auto f3 = again ? fast_fields(again->emission.payload, p.pb.setup().guids.ability, "UAoCAbilityComponent") : std::nullopt;
  const FastArrayField *h = f3 ? field_named(*f3, "UAoCAbilityComponent", "Cooldowns") : nullptr;
  CHECK(h && h->delta.changed.size() == 1 && h->delta.changed[0].id == 2, "a new cooldown is a new item (element ID 2: IDs are not reused)");
}

static void check_end_ability_block_payload(const bs::Send &s, const BlockView &v) {
  size_t pos = v.payload_pos;
  const auto calls = read_rpc_fields(class_named("UAoCAbilityComponent"), s.emission.payload.data(), v.payload_pos + v.payload_bits, pos);
  CHECK(calls && calls->size() == 1 && (*calls)[0].decoded && std::string((*calls)[0].rpc->name) == "ClientEndAbility", "it reads back as ClientEndAbility");
  const schema::RepCommand *cmd = calls && !calls->empty() ? rpc_command(*(*calls)[0].rpc, "ClientInstanceId") : nullptr;
  const Value *id = cmd ? item_value((*calls)[0].params, cmd->handle) : nullptr;
  CHECK(id && id->get_if<int64_t>() && *id->get_if<int64_t>() == 7, "with ClientInstanceId 7");
}

static void check_end_ability_wire_rpc(Pawn &p) {
  const auto s = p.pb.end_ability(7);
  CHECK(s && s->role == bs::Role::Pawn && !s->opens_channel && s->reliable() && s->emission.exports.empty(), "ClientEndAbility is reliable, on the pawn's channel, with no export");
  if (!s)
    return;
  const auto blocks = read_content_blocks(s->emission.payload.data(), s->emission.payload.bit_pos(), 0);
  CHECK(blocks && blocks->size() == 1 && !(*blocks)[0].header.is_actor && (*blocks)[0].header.object == p.pb.setup().guids.ability, "one block, on the AbilityComponent");
  if (!blocks || blocks->size() != 1)
    return;
  check_end_ability_block_payload(*s, (*blocks)[0]);
}

static void test_end_ability() {
  std::printf("ClientEndAbility\n");
  Pawn p;
  check_end_ability_wire_rpc(p);
  CHECK(p.pb.end_ability(7) && !p.pb.ability_update(), "(it changes no shadow: an update after it has nothing to send)");
  auto setup = bs::captured_character_setup();
  bs::PlayerBootstrap fresh(std::move(*setup), bs::GameStateMode::Tier2);
  CHECK(fresh.ok() && !fresh.end_ability(0) && !fresh.ability_update(), "before the pawn's second state there is nothing to call or update");
  bs::AbilityChange c;
  c.stats.push_back({content::kGuidStamina, 1.0f});
  CHECK(!fresh.apply_ability_change(c), "and no change to apply");
}

static void test_apply_refusals() {
  std::printf("apply_ability_change refuses what does not fit\n");
  Pawn p;
  bs::AbilityChange none;
  CHECK(p.pb.apply_ability_change(none) && !p.pb.ability_update(), "an empty change is applied and sends nothing");
  bs::AbilityChange c;
  c.stats.push_back({12345, 1.0f});
  CHECK(!p.pb.apply_ability_change(c), "a stat the character does not have is refused");
  c = {};
  c.effects_removed.push_back(9);
  CHECK(!p.pb.apply_ability_change(c), "an effect to remove that was never added is refused");
  c = {};
  c.abilities_removed.push_back(9);
  CHECK(!p.pb.apply_ability_change(c), "so is an ability instance");
  c = {};
  c.abilities_changed.push_back(rest_item(9, 1));
  CHECK(!p.pb.apply_ability_change(c), "an ability item to change that is not there");
  c = {};
  c.cooldowns_removed.push_back(content::kGuidDodge);
  CHECK(!p.pb.apply_ability_change(c), "a cooldown to remove that is not there");
  CHECK(!p.pb.ability_update(), "(none of them changed anything)");
  c = {};
  c.effects_added.push_back(rest_effect(0, 1.0));
  CHECK(p.pb.apply_ability_change(c), "an effect is added");
  c = {};
  c.stats.push_back({content::kGuidStamina, 50.0f});
  c.effects_added.push_back(rest_effect(0, 2.0));
  CHECK(!p.pb.apply_ability_change(c), "the same EffectId again is refused...");
  const auto after = p.pb.ability_update();
  const auto fields = after ? fast_fields(after->emission.payload, p.pb.setup().guids.stats, "UAoCStatsComponent") : std::nullopt;
  CHECK(fields && fields->size() == 1 && field_named(*fields, "UAoCStatsComponent", "CurrentEffects") && !field_named(*fields, "UAoCStatsComponent", "StatRepInt32Owner"), "...as a whole: its Stamina step was not applied either");
  c = {};
  c.effects_removed.push_back(0);
  c.effects_added.push_back(rest_effect(0, 3.0));
  CHECK(p.pb.apply_ability_change(c), "removing an effect and adding one with the same id in one step is allowed");
}

int main() {
  const Rules r;
  test_rest(r);
  test_other_abilities(r);
  test_mounted(r);
  test_instances(r);
  test_rest_golden();
  test_stamina_delta();
  test_bucket_stat_changes();
  test_cooldown_item();
  test_end_ability();
  test_apply_refusals();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
