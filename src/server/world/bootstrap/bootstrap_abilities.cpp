// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#include <bit>
#include <set>

#include "bootstrap/bootstrap.h"
#include "bootstrap/bootstrap_internal.h"
#include "content/content_db.h"
#include "protocol/repl/ability_rpc.h"
#include "protocol/repl/hit_rpc.h"

namespace aoc::worldd::bootstrap {

using namespace ::aoc::protocol;
using namespace ::aoc::protocol::emit;
using namespace ::aoc::protocol::schema;
using repl::Value;

repl::ElementState effect_item_state(const std::array<uint32_t, 4> &character_guid, const EffectItem &e) {
  repl::ElementState es;
  auto &v = es.values;
  v[1] = Value::i64(e.effect_id);
  v[2] = Value::i64(e.effect_guid);
  v[3] = Value::f64(e.start_time);
  v[4] = Value::f32(e.duration);
  v[5] = Value::f32(e.tick_time);
  v[6] = Value::i64(e.stacks);
  for (uint16_t i = 0; i < 4; ++i) {
    v[static_cast<uint16_t>(7 + i)] = Value::i64(static_cast<int32_t>(character_guid[i]));
    v[static_cast<uint16_t>(11 + i)] = Value::i64(static_cast<int32_t>(character_guid[i]));
  }
  v[15] = Value::boolean(false);
  v[16] = Value::i64(0);
  return es;
}

repl::ElementState cooldown_item_state(const CooldownItem &c) {
  repl::ElementState es;
  auto &v = es.values;
  v[1] = Value::i64(c.ability_guid);
  v[2] = Value::boolean(c.triggered);
  v[3] = Value::f32(c.initial_cooldown);
  v[4] = Value::f64(c.start_time);
  v[5] = Value::f64(c.end_time);
  v[6] = Value::i64(c.charges);
  return es;
}

namespace {

Value guid_word(uint32_t w) {
  return Value::i64(static_cast<int32_t>(w));
}

repl::ElementState effect_state(const PlayerSetup &s, const EffectItem &e) {
  return effect_item_state(s.character_guid, e);
}

repl::ElementState ability_state(const PlayerSetup &s, const ActiveAbilityItem &a, bool pvp_force) {
  repl::ElementState es;
  auto &v = es.values;
  v[1] = Value::i64(a.ability_guid);
  v[2] = Value::i64(a.server_instance_id);
  repl::ArrayValue targets;
  if (a.targets_self) {
    repl::ElementState t;
    t.values[1] = object(s.guids.collision_cylinder);
    targets.elements.push_back(std::move(t));
  }
  for (const FIntrepidNetworkGUID &g : a.target_objects) {
    repl::ElementState t;
    t.values[1] = object(g);
    targets.elements.push_back(std::move(t));
  }
  v[3] = Value(std::move(targets));
  v[4] = Value(repl::ArrayValue{});
  v[5] = Value::boolean(pvp_force);
  v[6] = Value::i64(0);
  v[7] = Value::f32(0);
  v[8] = Value::f32(0);
  v[9] = Value::boolean(a.branch_predicted);
  v[10] = Value(repl::NameValue::plain(a.montage_group));
  const std::optional<MontageRef> montage =
      a.montage_guid != 0 ? montage_for(a.montage_path, a.montage_guid) : montage_for(a.montage_path);
  v[11] = Value(montage ? montage->guid : FIntrepidNetworkGUID{});
  v[12] = Value::f32(1.0f);
  v[13] = Value::f32(a.montage_position);
  v[14] = Value::boolean(a.may_jump_to_position);
  v[15] = Value::i64(a.animation_counter);
  v[16] = Value::u64(a.mesh_yaw);
  v[17] = object(s.guids.pawn);
  return es;
}

repl::ElementState cooldown_state(const CooldownItem &c) {
  return cooldown_item_state(c);
}

struct FoundStat {
  repl::FastArrayState *array = nullptr;
  int32_t id = 0;
  repl::StatInt32Inst item;
};

const repl::StatInt32Inst *read_stat_int32_inst(const repl::FastArrayItem &it) {
  const auto found = it.values.values.find(1);
  return found != it.values.values.end() ? found->second.get_if<repl::StatInt32Inst>() : nullptr;
}

std::optional<FoundStat> find_stat(repl::Subobject &stats, int64_t record) {
  for (const char *field : {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy"}) {
    repl::FastArrayState *a = stats.obj.arrays.get(*stats.obj.schema, field);
    if (!a) continue;
    for (const repl::FastArrayItem &it : a->items()) {
      const repl::StatInt32Inst *t = read_stat_int32_inst(it);
      if (t && t->record == record) return FoundStat{a, it.id, *t};
    }
  }
  return std::nullopt;
}

std::optional<int32_t> find_shield_item_id(repl::FastArrayState *shield_array) {
  if (!shield_array) return std::nullopt;
  for (const repl::FastArrayItem &it : shield_array->items()) {
    const auto found = it.values.values.find(1);
    const auto *record = found != it.values.values.end() ? found->second.get_if<int64_t>() : nullptr;
    if (record && *record == content::kGuidTempHealth) return it.id;
  }
  return std::nullopt;
}

bool validate_effect_keys(const AbilityChange &c, const std::map<int64_t, int32_t> &effect_ids) {
  std::set<int64_t> effect_keys;
  for (const auto &[k, id] : effect_ids) effect_keys.insert(k);
  for (const int64_t k : c.effects_removed) {
    if (effect_keys.erase(k) == 0) return false;
  }
  for (const EffectItem &e : c.effects_added) {
    if (!effect_keys.insert(e.effect_id).second) return false;
  }
  for (const EffectItem &e : c.effects_changed) {
    if (effect_keys.count(e.effect_id) == 0) return false;
  }
  return true;
}

bool validate_ability_keys(const AbilityChange &c, const std::map<int64_t, int32_t> &ability_ids) {
  std::set<int64_t> ability_keys;
  for (const auto &[k, id] : ability_ids) ability_keys.insert(k);
  for (const int64_t k : c.abilities_removed) {
    if (ability_keys.erase(k) == 0) return false;
  }
  for (const ActiveAbilityItem &a : c.abilities_added) {
    if (!ability_keys.insert(a.server_instance_id).second) return false;
  }
  for (const ActiveAbilityItem &a : c.abilities_changed) {
    if (ability_keys.count(a.server_instance_id) == 0) return false;
  }
  return true;
}

bool validate_cooldown_keys(const AbilityChange &c, const std::map<int64_t, int32_t> &cooldown_ids) {
  std::set<int64_t> cooldown_keys;
  for (const auto &[k, id] : cooldown_ids) cooldown_keys.insert(k);
  for (const int64_t k : c.cooldowns_removed) {
    if (cooldown_keys.erase(k) == 0) return false;
  }
  return true;
}

bool validate_ability_change(const AbilityChange &c, repl::Subobject &stats,
                             const std::map<int64_t, int32_t> &effect_ids,
                             const std::map<int64_t, int32_t> &ability_ids,
                             const std::map<int64_t, int32_t> &cooldown_ids) {
  for (const StatChange &s : c.stats) {
    if (find_stat(stats, s.record)) continue;
    const bool add = s.add_to_replication == 1 || s.add_to_replication == 5 || s.add_to_replication == 7;
    if (!add) return false;
  }
  return validate_effect_keys(c, effect_ids) && validate_ability_keys(c, ability_ids) &&
         validate_cooldown_keys(c, cooldown_ids);
}

bool insert_new_stat_change(const StatChange &s, repl::Subobject &stats, std::set<int64_t> &stat_layout,
                            std::string &err) {
  const uint32_t bits = std::bit_cast<uint32_t>(s.value);
  const uint32_t base = std::bit_cast<uint32_t>(s.base);
  const uint32_t equipment = std::bit_cast<uint32_t>(s.equipment);
  const char *field = s.add_to_replication == 1   ? "StatRepInt32Owner"
                      : s.add_to_replication == 5 ? "StatRepInt32OwnerProxy"
                                                  : "StatRepInt32EveryoneProxy";
  repl::FastArrayState *array = stats.obj.arrays.get(*stats.obj.schema, field);
  if (!array) {
    err = std::string("StatsComponent has no fast array ") + field;
    return false;
  }
  repl::ElementState es;
  es.values[1] =
      Value(repl::StatInt32Inst{s.record, bits, s.buckets, s.buckets ? base : 0u, s.buckets ? equipment : 0u});
  if (!array->add(std::move(es))) {
    err = "a stat item does not fit its array";
    return false;
  }
  stat_layout.insert(s.record);
  return true;
}

bool apply_single_stat_change(const StatChange &s, repl::Subobject &stats, std::set<int64_t> &stat_layout,
                              std::string &err) {
  const auto found = find_stat(stats, s.record);
  if (!found) return insert_new_stat_change(s, stats, stat_layout, err);
  const uint32_t bits = std::bit_cast<uint32_t>(s.value);
  const uint32_t base = std::bit_cast<uint32_t>(s.base);
  const uint32_t equipment = std::bit_cast<uint32_t>(s.equipment);
  const bool buckets = s.buckets && found->item.extended;
  if (found->item.value == bits && (!buckets || (found->item.base == base && found->item.equipment == equipment)))
    return true;
  repl::StatInt32Inst now = found->item;
  now.value = bits;
  if (buckets) {
    now.base = base;
    now.equipment = equipment;
  }
  repl::ElementState es;
  es.values[1] = Value(now);
  if (!found->array->change(found->id, std::move(es))) {
    err = "a stat item does not take its new value";
    return false;
  }
  return true;
}

bool apply_shield_changes(const std::vector<ShieldElement> &shields, repl::FastArrayState &shield_array,
                          int32_t shield_item, std::string &err) {
  repl::ElementState item;
  for (const repl::FastArrayItem &it : shield_array.items()) {
    if (it.id == shield_item) item = it.values;
  }
  repl::ArrayValue values;
  for (const ShieldElement &e : shields) {
    repl::ElementState v;
    for (uint16_t i = 0; i < 4; ++i) v.values[static_cast<uint16_t>(1 + i)] = guid_word(e.guid[i]);
    v.values[5] = Value::f32(e.base);
    v.values[6] = Value::f32(e.value);
    v.values[7] = Value::f32(0);
    values.elements.push_back(std::move(v));
  }
  item.values[2] = Value(std::move(values));
  if (!shield_array.change(shield_item, std::move(item))) {
    err = "the TempHealth item does not take its shields";
    return false;
  }
  return true;
}

bool apply_effect_changes(const AbilityChange &c, const PlayerSetup &setup, repl::FastArrayState &effects,
                          std::map<int64_t, int32_t> &effect_ids, std::string &err) {
  for (const int64_t k : c.effects_removed) {
    effects.remove(effect_ids.at(k));
    effects.mark_array_dirty();
    effect_ids.erase(k);
  }
  for (const EffectItem &e : c.effects_added) {
    const auto id = effects.add(effect_state(setup, e));
    if (!id) {
      err = "an effect item does not fit CurrentEffects";
      return false;
    }
    effect_ids[e.effect_id] = *id;
  }
  for (const EffectItem &e : c.effects_changed) {
    if (!effects.change(effect_ids.at(e.effect_id), effect_state(setup, e))) {
      err = "an effect item does not take its change";
      return false;
    }
  }
  return true;
}

bool upsert_single_cooldown(const CooldownItem &cd, repl::FastArrayState &cooldowns,
                            std::map<int64_t, int32_t> &cooldown_ids, std::string &err) {
  const auto it = cooldown_ids.find(cd.ability_guid);
  if (it != cooldown_ids.end()) {
    if (!cooldowns.change(it->second, cooldown_state(cd))) {
      err = "a cooldown item does not take its change";
      return false;
    }
    return true;
  }
  const auto id = cooldowns.add(cooldown_state(cd));
  if (!id) {
    err = "a cooldown item does not fit Cooldowns";
    return false;
  }
  cooldown_ids[cd.ability_guid] = *id;
  return true;
}

bool apply_cooldown_changes(const AbilityChange &c, repl::FastArrayState &cooldowns,
                            std::map<int64_t, int32_t> &cooldown_ids, std::string &err) {
  for (const int64_t k : c.cooldowns_removed) {
    cooldowns.remove(cooldown_ids.at(k));
    cooldowns.mark_array_dirty();
    cooldown_ids.erase(k);
  }
  for (const CooldownItem &cd : c.cooldowns_set) {
    if (!upsert_single_cooldown(cd, cooldowns, cooldown_ids, err)) return false;
  }
  return true;
}

std::vector<std::pair<FIntrepidNetworkGUID, FIntrepidNetworkGUID>> targets_of_ability(const ActiveAbilityItem &a) {
  std::vector<std::pair<FIntrepidNetworkGUID, FIntrepidNetworkGUID>> out;
  for (size_t i = 0; i < a.target_objects.size() && i < a.target_actors.size(); ++i) {
    if (a.target_actors[i].is_valid() && a.target_objects[i].is_valid())
      out.emplace_back(a.target_actors[i], a.target_objects[i]);
  }
  return out;
}

struct AbilityTrackRefs {
  std::map<int64_t, int32_t> &ids;
  std::set<int64_t> &trigger_pvp;
  std::map<int64_t, std::pair<std::string, uint64_t>> &montages;
  std::map<int64_t, std::vector<std::pair<FIntrepidNetworkGUID, FIntrepidNetworkGUID>>> &targets;
};

bool apply_active_ability_changes(const AbilityChange &c, const PlayerSetup &setup, repl::FastArrayState &abilities,
                                  AbilityTrackRefs track, std::string &err) {
  for (const int64_t k : c.abilities_removed) {
    abilities.remove(track.ids.at(k));
    abilities.mark_array_dirty();
    track.ids.erase(k);
    track.trigger_pvp.erase(k);
    track.montages.erase(k);
    track.targets.erase(k);
  }
  for (const ActiveAbilityItem &a : c.abilities_added) {
    const auto id = abilities.add(ability_state(setup, a, track.trigger_pvp.count(a.server_instance_id) != 0));
    if (!id) {
      err = "an ability item does not fit ReplicatedActiveAbilities";
      return false;
    }
    track.ids[a.server_instance_id] = *id;
    track.montages[a.server_instance_id] = {a.montage_path, a.montage_guid};
    track.targets[a.server_instance_id] = targets_of_ability(a);
  }
  for (const ActiveAbilityItem &a : c.abilities_changed) {
    if (!abilities.change(track.ids.at(a.server_instance_id),
                          ability_state(setup, a, track.trigger_pvp.count(a.server_instance_id) != 0))) {
      err = "an ability item does not take its change";
      return false;
    }
    track.montages[a.server_instance_id] = {a.montage_path, a.montage_guid};
    track.targets[a.server_instance_id] = targets_of_ability(a);
  }
  return true;
}

void attach_montage_exports(repl::ContentBlock &b,
                            const std::map<int64_t, std::pair<std::string, uint64_t>> &ability_montages) {
  std::set<std::string> done;
  for (const auto &[key, montage] : ability_montages) {
    if (montage.first.empty() || !done.insert(montage.first).second) continue;
    const auto m = montage.second != 0 ? montage_for(montage.first, montage.second) : montage_for(montage.first);
    if (m) b.exports.push_back(m->chain);
  }
}

void attach_target_cylinder_exports(
    repl::ContentBlock &b,
    const std::map<int64_t, std::vector<std::pair<FIntrepidNetworkGUID, FIntrepidNetworkGUID>>> &ability_targets) {
  std::set<uint64_t> exported;
  for (const auto &[key, targets] : ability_targets) {
    for (const auto &[actor, component] : targets) {
      if (exported.insert(component.ObjectId).second) {
        b.exports.push_back(repl::component_export(actor, component, "CollisionCylinder", kChecksumCollisionCylinder));
      }
    }
  }
}

void attach_ability_block_exports(
    repl::ContentBlock &b, const std::map<int64_t, std::pair<std::string, uint64_t>> &ability_montages,
    const std::map<int64_t, std::vector<std::pair<FIntrepidNetworkGUID, FIntrepidNetworkGUID>>> &ability_targets) {
  attach_montage_exports(b, ability_montages);
  attach_target_cylinder_exports(b, ability_targets);
}

bool populate_lingering_actor_props(const LingeringSpec &spec, const FIntrepidNetworkGUID &pawn_guid,
                                    repl::ObjectState &obj) {
  bool ok = true;
  auto set = [&](std::string_view path, Value v) { ok = obj.state.set(*obj.schema, path, std::move(v)) && ok; };
  set("AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated));
  set("RemoteRole", Value::u64(1));
  set("AttachmentReplication.AttachParent", object(pawn_guid));
  set("AttachmentReplication.LocationOffset", Value(repl::QuantVec{repl::QuantVec::Mode::Scaled, 3, 0, 0}));
  set("AttachmentReplication.RelativeScale3D",
      Value(repl::QuantVec{repl::QuantVec::Mode::Scaled, kRelativeScale100, kRelativeScale100, kRelativeScale100}));
  set("AttachmentReplication.AttachComponent", object(spec.pawn_mesh));
  set("Role", Value::u64(4));
  set("Instigator", object(pawn_guid));
  static constexpr const char *kWords[4] = {"A", "B", "C", "D"};
  for (size_t i = 0; i < 4; ++i) set(std::string("LingeringEffectGuid.") + kWords[i], guid_word(spec.effect_guid[i]));
  set("LingeringEffectId", Value::i64(spec.zone));
  set("SourceRecordGuid", Value::i64(spec.source));
  set("SourceRecordType", Value::i64(content::kSourceTypeAbility));
  for (size_t i = 0; i < 4; ++i)
    set(std::string("LingeringZoneInstGuid.") + kWords[i], guid_word(spec.instance_guid[i]));
  return ok;
}

repl::HitTarget make_hit_target_from_out(const HitTargetOut &o, const repl::Vec3d &at) {
  repl::HitTarget t;
  t.hit_comp = o.component;
  t.impact_location = o.has_impact ? o.impact_location : at;
  if (o.has_impact) t.impact_normal = o.impact_normal;
  for (const HitStatModOut &m : o.mods) t.stat_mods.push_back({m.stat_mod_guid, m.value, m.effective});
  t.flags = o.flags;
  return t;
}

void populate_hit_targets(const HitConfirm &c, const FIntrepidNetworkGUID &pawn_guid, const repl::Vec3d &at,
                          std::vector<repl::HitTarget> &targets) {
  if (!c.targets.empty()) {
    for (const HitTargetOut &o : c.targets) targets.push_back(make_hit_target_from_out(o, at));
    return;
  }
  if (c.no_target) return;
  repl::HitTarget t;
  t.hit_comp = pawn_guid;
  t.impact_location = at;
  for (const HitStatModOut &m : c.mods) t.stat_mods.push_back({m.stat_mod_guid, m.value, m.effective});
  t.flags = c.flags;
  targets.push_back(std::move(t));
}

repl::HitInstance build_hit_instance(const HitConfirm &c, const FIntrepidNetworkGUID &pawn_guid, const repl::Vec3d &at,
                                     int64_t inst_guid) {
  repl::HitInstance h;
  h.inst_guid = inst_guid;
  h.record_guid = c.record_guid;
  h.source_record_type = c.source_type;
  h.source_record_guid = c.source_guid;
  h.instigator = c.instigator ? *c.instigator : pawn_guid;
  if (c.source) {
    h.source = *c.source;
  } else if (c.source_is_actor) {
    h.source = h.instigator;
  }
  populate_hit_targets(c, pawn_guid, at, h.targets);
  h.predicted_on_autonomous = c.predicted_autonomous;
  h.predicted_on_simulated = c.predicted_simulated;
  return h;
}

bool apply_stat_and_shield_changes(const AbilityChange &c, repl::Subobject &stats, std::set<int64_t> &stat_layout,
                                   std::string &err) {
  repl::FastArrayState *shield_array = stats.obj.arrays.get(*stats.obj.schema, "StatRepFloatArrayEveryoneProxy");
  const std::optional<int32_t> shield_item = c.shields ? find_shield_item_id(shield_array) : std::nullopt;
  for (const StatChange &s : c.stats) {
    if (!apply_single_stat_change(s, stats, stat_layout, err)) return false;
  }
  if (c.shields && shield_item && !apply_shield_changes(*c.shields, *shield_array, *shield_item, err)) return false;
  return true;
}

void attach_exports_to_ability_blocks(
    std::vector<repl::ContentBlock> &blocks, const FIntrepidNetworkGUID &ability_guid,
    const std::map<int64_t, std::pair<std::string, uint64_t>> &ability_montages,
    const std::map<int64_t, std::vector<std::pair<FIntrepidNetworkGUID, FIntrepidNetworkGUID>>> &ability_targets) {
  for (repl::ContentBlock &b : blocks) {
    if (b.sub && b.sub->guid == ability_guid) attach_ability_block_exports(b, ability_montages, ability_targets);
  }
}

struct AbilityChangeArrays {
  repl::FastArrayState *effects = nullptr;
  repl::FastArrayState *abilities = nullptr;
  repl::FastArrayState *cooldowns = nullptr;
};

std::optional<AbilityChangeArrays> resolve_ability_change_arrays(repl::Subobject *stats, repl::Subobject *ability) {
  if (!stats || !ability) return std::nullopt;
  AbilityChangeArrays out{stats->obj.arrays.get(*stats->obj.schema, "CurrentEffects"),
                          ability->obj.arrays.get(*ability->obj.schema, "ReplicatedActiveAbilities"),
                          ability->obj.arrays.get(*ability->obj.schema, "Cooldowns")};
  if (!out.effects || !out.abilities || !out.cooldowns) return std::nullopt;
  return out;
}

void collect_stat_array(const repl::FastArrayState *a, std::map<int64_t, float> &out) {
  if (!a) return;
  for (const repl::FastArrayItem &it : a->items()) {
    if (const repl::StatInt32Inst *t = read_stat_int32_inst(it)) out[t->record] = std::bit_cast<float>(t->value);
  }
}

}  // namespace

constexpr int64_t kHitInstanceBase = 6064635119377315210ll;

bool PlayerBootstrap::apply_ability_change(const AbilityChange &c) {
  if (!ok_ || !second_state_added_ || (c.shields && c.shields->size() > 64)) return false;
  repl::Subobject *stats = component(setup_.guids.stats);
  const auto arrays = resolve_ability_change_arrays(stats, component(setup_.guids.ability));
  if (!arrays || !validate_ability_change(c, *stats, effect_ids_, ability_ids_, cooldown_ids_)) return false;
  std::string err;
  const AbilityTrackRefs track{ability_ids_, trigger_pvp_, ability_montages_, ability_targets_};
  const bool applied = apply_stat_and_shield_changes(c, *stats, stat_layout_, err) &&
                       apply_effect_changes(c, setup_, *arrays->effects, effect_ids_, err) &&
                       apply_active_ability_changes(c, setup_, *arrays->abilities, track, err) &&
                       apply_cooldown_changes(c, *arrays->cooldowns, cooldown_ids_, err);
  if (!applied) return fail(std::move(err));
  return true;
}

bool PlayerBootstrap::set_abilities(std::span<const int64_t> known, std::span<const AbilityRecord> records) {
  if (!ok_ || !second_state_added_) return false;
  constexpr size_t kMaxLists = 4096;
  if (known.size() > kMaxLists || records.size() > kMaxLists) return false;
  repl::Subobject *ability = component(setup_.guids.ability);
  if (!ability) return false;
  repl::ArrayValue k;
  for (const int64_t guid : known) {
    repl::ElementState e;
    e.values[1] = Value::i64(guid);
    k.elements.push_back(std::move(e));
  }
  repl::ArrayValue r;
  for (const AbilityRecord &rec : records) {
    repl::ElementState e;
    e.values[1] = Value::i64(rec.guid);
    e.values[2] = Value::i64(rec.type_id);
    e.values[3] = Value(repl::NameValue::hardcoded_name(0));
    r.elements.push_back(std::move(e));
  }
  if (!ability->obj.state.set(*ability->obj.schema, "KnownAbilities", Value(std::move(k))) ||
      !ability->obj.state.set(*ability->obj.schema, "ReplicatedRecordsToLoad", Value(std::move(r)))) {
    return false;
  }
  return true;
}

bool PlayerBootstrap::has_stat_item(int64_t record) const {
  return ok_ && second_state_added_ && stat_layout_.count(record) != 0;
}

std::map<int64_t, float> PlayerBootstrap::current_stats() {
  std::map<int64_t, float> out;
  if (!ok_ || !second_state_added_) return out;
  repl::Subobject *stats = component(setup_.guids.stats);
  if (!stats) return out;
  for (const char *field : {"StatRepInt32Owner", "StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy"}) {
    collect_stat_array(stats->obj.arrays.get(*stats->obj.schema, field), out);
  }
  return out;
}

std::optional<Send> PlayerBootstrap::ability_update(bool *failed) {
  if (failed) *failed = false;
  if (!ok_ || !second_state_sent_) return std::nullopt;
  std::vector<repl::ContentBlock> blocks;
  repl::ShadowUndo undo;
  if (!repl::collect_update_blocks(pawn_, pawn_shadow_, blocks, &undo)) {
    if (failed) *failed = true;
    fail("an ability item cannot be written (a value the codecs refuse)");
    return std::nullopt;
  }
  if (blocks.empty()) return std::nullopt;
  attach_exports_to_ability_blocks(blocks, setup_.guids.ability, ability_montages_, ability_targets_);
  auto e = repl::assemble_blocks(pawn_, blocks, acks_);
  if (!e) {
    undo.rollback(pawn_shadow_);
    if (failed) *failed = true;
    fail(
        "the ability update does not assemble into a chain (an export that "
        "does not fit a bunch)");
  }
  return finish(Role::Pawn, false, "abilities", std::move(e));
}

std::optional<Send> PlayerBootstrap::end_ability(int32_t client_instance_id) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const repl::Subobject *ability = component(setup_.guids.ability);
  if (!ability) return std::nullopt;
  return finish(Role::Pawn, false, "ClientEndAbility",
                repl::client_end_ability(pawn_, *ability, client_instance_id, acks_));
}

bool PlayerBootstrap::lingering_add(const LingeringSpec &spec) {
  if (effect_actors_.count(spec.actor.ObjectId) != 0) return false;
  auto actor = lingering_actor_of(spec);
  if (!actor) return false;
  EffectActor e;
  e.actor = std::move(*actor);
  effect_actors_[spec.actor.ObjectId] = std::move(e);
  mesh_ = spec.pawn_mesh;
  return true;
}

repl::ExportChain PlayerBootstrap::lingering_mesh_export(const emit::FIntrepidNetworkGUID &pawn_mesh) const {
  return repl::component_export(setup_.guids.pawn, pawn_mesh, "CharacterMesh0", kChecksumPawnMesh);
}

std::optional<repl::Actor> PlayerBootstrap::lingering_actor_of(const LingeringSpec &spec) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::ClassSchema *cls = gen::find_class_schema("AAoCAbilityLingeringEffect");
  const schema::ClassSchema *component = gen::find_class_schema("UActorComponent");
  if (!cls || !component || !std::isfinite(spec.x) || !std::isfinite(spec.y) || !std::isfinite(spec.z) ||
      !std::isfinite(spec.yaw_degrees)) {
    return std::nullopt;
  }
  repl::Actor a;
  a.guid = spec.actor;
  a.obj.schema = cls;
  if (!populate_lingering_actor_props(spec, setup_.guids.pawn, a.obj)) return std::nullopt;
  a.open.archetype = lingering_effect_archetype_export();
  a.open.level = persistent_level_export();
  a.open.spawn.location = repl::quantize_vector(10, spec.x, spec.y, spec.z);
  a.open.spawn.rotation = repl::Rot{0, compress_short(spec.yaw_degrees), 0};
  a.open.spawn.velocity = spec.velocity;
  repl::Subobject movement;
  movement.guid = spec.movement;
  movement.name = "LingeringEffectMovementComp";
  movement.checksum = kChecksumLingeringMovement;
  movement.obj.schema = component;
  if (!movement.obj.state.set(*movement.obj.schema, "bIsActive", Value::boolean(true))) return std::nullopt;
  a.subobjects.push_back(std::move(movement));
  a.subobjects.push_back(
      repl::Subobject{.guid = spec.ability, .name = "AbilityComp", .checksum = kChecksumLingeringAbility});
  a.subobjects.push_back(repl::Subobject{.guid = spec.stats, .name = "StatsComp", .checksum = kChecksumLingeringStats});
  return a;
}

std::optional<Send> PlayerBootstrap::lingering_open(uint64_t actor_id) {
  const auto it = effect_actors_.find(actor_id);
  if (!ok_ || it == effect_actors_.end()) return std::nullopt;
  EffectActor &e = it->second;
  std::vector<repl::ContentBlock> blocks;
  repl::ShadowUndo undo;
  e.shadow.forget_sent();
  if (!repl::collect_open_blocks(e.actor, e.shadow, blocks, &undo) || blocks.empty() || blocks[0].sub) {
    undo.rollback(e.shadow);
    return std::nullopt;
  }
  blocks[0].exports.push_back(lingering_mesh_export(mesh_));
  auto emission = repl::assemble_open(e.actor, blocks, acks_);
  if (!emission) undo.rollback(e.shadow);
  auto send = finish(Role::Pawn, true, "lingering zone open", std::move(emission));
  if (send) send->effect_actor = actor_id;
  return send;
}

std::optional<Send> PlayerBootstrap::lingering_fade(uint64_t actor_id) {
  const auto it = effect_actors_.find(actor_id);
  if (!ok_ || it == effect_actors_.end()) return std::nullopt;
  EffectActor &e = it->second;
  if (!e.actor.obj.state.set(*e.actor.obj.schema, "bFadingOut", Value::boolean(true))) return std::nullopt;
  auto send = update(Role::Pawn, "lingering zone fades out", e.actor, e.shadow);
  if (send) send->effect_actor = actor_id;
  return send;
}

void PlayerBootstrap::lingering_remove(uint64_t actor_id) {
  effect_actors_.erase(actor_id);
}

const repl::Actor *PlayerBootstrap::lingering_actor(uint64_t actor_id) const {
  const auto it = effect_actors_.find(actor_id);
  return it == effect_actors_.end() ? nullptr : &it->second.actor;
}

std::optional<Send> PlayerBootstrap::hit_confirmations(const std::vector<HitConfirm> &confirms, const repl::Vec3d &at) {
  if (!ok_ || !second_state_added_ || confirms.empty()) return std::nullopt;
  const SessionGuids &g = setup_.guids;
  repl::HitConfirmations out;
  std::vector<repl::ExportChain> exports;
  std::set<uint64_t> exported;
  for (const HitConfirm &c : confirms) {
    const int64_t inst_guid = kHitInstanceBase + static_cast<int64_t>(++hit_instances_) * 65537;
    out.hits.push_back(build_hit_instance(c, g.pawn, at, inst_guid));
    for (const HitTargetOut &o : c.targets) {
      if (o.cylinder.is_valid() && o.component.is_valid() && !(o.component == g.pawn) &&
          exported.insert(o.cylinder.ObjectId).second) {
        exports.push_back(npc_component_export(o.component, o.cylinder));
      }
    }
  }
  if (!repl::hit_confirmations_fit(out)) return std::nullopt;
  return finish(Role::PlayerController, false, "ClientHitConfirmationsReliable",
                repl::client_hit_confirmations(pc_, out, true, acks_, exports));
}

repl::ExportChain PlayerBootstrap::npc_component_export(const FIntrepidNetworkGUID &actor,
                                                        const FIntrepidNetworkGUID &component) {
  return repl::component_export(actor, component, "CollisionCylinder", kChecksumCollisionCylinder);
}

std::optional<Send> PlayerBootstrap::correction(float time_stamp, const repl::Vec3d &location,
                                                const repl::Vec3d &velocity, uint8_t movement_mode) const {
  if (!ok_) return std::nullopt;
  const schema::RpcSchema *rpc = pawn_.obj.schema->rpc("ClientAdjustPosition");
  if (!rpc) return std::nullopt;
  repl::RpcParams params;
  if (!repl::set_rpc_param(*rpc, params, "TimeStamp", Value::f32(time_stamp)) ||
      !repl::set_rpc_param(*rpc, params, "NewLoc", Value(location)) ||
      !repl::set_rpc_param(*rpc, params, "NewVel", Value(velocity)) ||
      !repl::set_rpc_param(*rpc, params, "ServerMovementMode", Value::u64(movement_mode))) {
    return std::nullopt;
  }
  return finish(Role::Pawn, false, "ClientAdjustPosition",
                repl::call_rpc(pawn_, nullptr, "ClientAdjustPosition", params, {}, acks_));
}

}  // namespace aoc::worldd::bootstrap
