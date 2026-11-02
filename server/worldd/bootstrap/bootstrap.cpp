// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "bootstrap/bootstrap.h"

#include <string>
#include <utility>
#include <vector>

#include "bootstrap/bootstrap_internal.h"

namespace aoc::worldd::bootstrap {

using protocol::emit::BunchWriter;
using protocol::emit::FIntrepidNetworkGUID;
using repl::ActorShadow;
using repl::Emission;
using repl::ExportChain;
using repl::Value;
namespace schema = ::aoc::protocol::schema;
namespace gen = ::aoc::protocol::schema::gen;

namespace {

bool record_fail(std::string &err, std::string message) {
  if (err.empty()) err = std::move(message);
  return false;
}

bool set_prop(repl::ObjectState &o, std::string_view path, Value v, std::string &err) {
  if (!o.schema) return record_fail(err, "no schema for " + std::string(path));
  if (!o.state.set(*o.schema, path, std::move(v))) {
    return record_fail(err, std::string(o.schema->name) + "." + std::string(path) + " refused");
  }
  return true;
}

bool validate_bag_items(const PlayerSetup &s, std::map<std::pair<uint8_t, int32_t>, repl::ItemInstance> &items,
                        std::string &err) {
  for (const StoredItem &it : s.items) {
    int limit = it.storage == ItemStorage::Backpack    ? storage_info(StorageKind::Backpack).slots
                : it.storage == ItemStorage::Equipment ? storage_info(StorageKind::Equipment).slots
                                                       : storage_info(StorageKind::QuestStorage).slots;
    if (is_resource_storage(it.storage)) {
      const size_t idx = static_cast<size_t>(static_cast<int>(it.storage) - static_cast<int>(ItemStorage::Resource0));
      const ResourceBagSetup &bag = s.resource_bags[idx];
      limit = bag.present ? bag.columns * bag.rows : 0;
    }
    if (it.slot < 0 || it.slot >= limit || !it.item.has_item) {
      return record_fail(err, "an item of the setup is in a slot outside its storage or is empty");
    }
    if (!items.emplace(std::make_pair(static_cast<uint8_t>(it.storage), it.slot), it.item).second) {
      return record_fail(err, "two items of the setup are in one slot");
    }
  }
  return true;
}

bool validate_and_load_items(const PlayerSetup &s, std::map<std::pair<uint8_t, int32_t>, repl::ItemInstance> &items,
                             std::string &err) {
  if (!validate_bag_items(s, items, err)) return false;
  if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z) || !std::isfinite(s.yaw_degrees)) {
    return record_fail(err, "the spawn point is not finite");
  }
  if (!std::isfinite(s.world_time) || s.world_time < 0 || s.world_time >= 2147483648.0) {
    return record_fail(err,
                       "the world time is outside 0 .. 2^31 seconds (the "
                       "PlayerState's StartTime is an int32)");
  }
  if (s.race > static_cast<uint8_t>(ClientRace::Tulnar) || s.gender > static_cast<uint8_t>(ClientGender::Female)) {
    return record_fail(err,
                       "race or gender is not a value of the client's "
                       "ECharacterRace / ECharacterGender");
  }
  return true;
}

bool apply_tier2_game_state(repl::Actor &gs, std::string &err) {
  const auto tier2 = game_state_tier2_values();
  if (!tier2) return record_fail(err, "the captured GameState values do not read");
  bool ok = true;
  for (const auto &[handle, value] : tier2->values()) {
    if (!gs.obj.state.set(*gs.obj.schema, handle, value)) {
      ok = record_fail(err, "GameState handle " + std::to_string(handle) + " refused") && ok;
    }
  }
  return ok;
}

bool build_pc_and_gs_actors(const PlayerSetup &s, GameStateMode mode, repl::Actor &pc, repl::Actor &gs,
                            std::string &err) {
  const SessionGuids &g = s.guids;
  bool ok = true;
  auto all = [&](bool b) { ok = b && ok; };
  const repl::QuantVec spawn = repl::quantize_vector(10, s.x, s.y, s.z);

  pc.guid = g.player_controller;
  pc.obj.schema = gen::find_class_schema("AoCPlayerControllerBP_C");
  all(set_prop(pc.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated), err));
  all(set_prop(pc.obj, "RemoteRole", Value::u64(2), err));
  all(set_prop(pc.obj, "Role", Value::u64(4), err));
  all(set_prop(pc.obj, "PlayerState", object(g.player_state), err));
  all(set_prop(pc.obj, "SpawnLocation", Value(repl::Vec3d{s.x, s.y, s.z}), err));
  all(set_prop(pc.obj, "CaravanLaunchNode", Value::i64(0), err));
  all(set_prop(pc.obj, "SummonCooldownTimer", Value::f32(static_cast<float>(s.world_time)), err));
  pc.open.archetype = pc_archetype_export();
  pc.open.level = persistent_level_export();
  pc.open.spawn.location = spawn;
  pc.open.player_controller = repl::PlayerControllerOpen{0, 0};
  repl::Subobject gm;
  gm.guid = g.global_gm_commands;
  gm.dynamic_class = global_gm_commands_class_export();
  pc.subobjects.push_back(std::move(gm));

  gs.guid = g.game_state;
  gs.obj.schema = gen::find_class_schema("AoCGameStateBP_C");
  all(set_prop(gs.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated), err));
  all(set_prop(gs.obj, "RemoteRole", Value::u64(1), err));
  all(set_prop(gs.obj, "Role", Value::u64(4), err));
  all(set_prop(gs.obj, "GameModeClass", object(game_mode_class_guid()), err));
  all(set_prop(gs.obj, "bReplicatedHasBegunPlay", Value::boolean(true), err));
  all(set_prop(gs.obj, "ReplicatedWorldTimeSecondsDouble", Value::f64(s.world_time), err));
  if (mode != GameStateMode::Tier1) all(apply_tier2_game_state(gs, err));
  if (mode == GameStateMode::Tier3 && !repl::sync_node_maps(*gs.obj.schema, gs.obj.arrays, s.nodes)) {
    all(record_fail(err, "the node maps do not fit the GameState's fast arrays"));
  }
  gs.open.archetype = game_state_archetype_export();
  gs.open.level = persistent_level_export();
  return ok;
}

bool populate_owner_currencies(const std::vector<CurrencyBalance> &currencies, repl::Subobject &owner,
                               std::string &err) {
  if (currencies.empty()) return true;
  repl::FastArrayState *wallet = owner.obj.arrays.get(*owner.obj.schema, "Currencies");
  if (!wallet) return record_fail(err, "OwnerInfo has no Currencies");
  bool ok = true;
  for (const CurrencyBalance &c : currencies) {
    repl::ElementState e;
    e.values[1] = Value::u64(static_cast<uint64_t>(c.amount));
    e.values[2] = Value::i64(c.currency);
    if (!wallet->add(std::move(e))) ok = record_fail(err, "OwnerInfo.Currencies: an entry does not fit") && ok;
  }
  return ok;
}

bool build_pawn_owner_subobject(const PlayerSetup &s, GameStateMode mode, repl::Subobject &owner, std::string &err) {
  bool ok = true;
  auto all = [&](bool b) { ok = b && ok; };
  owner = make_component(s.guids.owner_info, "OwnerInfo", kChecksumOwnerInfo);
  all(populate_owner_currencies(s.currencies, owner, err));
  if (mode == GameStateMode::Tier3) {
    if (s.citizen_node != 0) all(set_prop(owner.obj, "CitizenNodeGuid", Value::i64(s.citizen_node), err));
    if (!s.account_characters.empty()) {
      repl::ArrayValue list;
      for (const AccountCharacterNode &c : s.account_characters) {
        repl::ElementState e;
        e.values[1] = Value::str("");
        e.values[2] = Value::str(c.guid_hex);
        e.values[3] = Value::str(c.name);
        e.values[4] = Value::i64(c.citizen_node);
        list.elements.push_back(std::move(e));
      }
      all(set_prop(owner.obj, "CharactersNodeData", Value(std::move(list)), err));
    }
  }
  return ok;
}

bool build_pawn_subobjects(const PlayerSetup &s, GameStateMode mode, std::vector<repl::Subobject> &subs,
                           std::string &err) {
  const SessionGuids &g = s.guids;
  bool ok = true;
  auto all = [&](bool b) { ok = b && ok; };

  repl::Subobject base = make_component(g.base_character_info, "BaseCharacterInfo", kChecksumBaseCharacterInfo);
  all(set_prop(base.obj, "CharacterGuid.A", Value::i64(static_cast<int32_t>(s.character_guid[0])), err));
  all(set_prop(base.obj, "CharacterGuid.B", Value::i64(static_cast<int32_t>(s.character_guid[1])), err));
  all(set_prop(base.obj, "CharacterGuid.C", Value::i64(static_cast<int32_t>(s.character_guid[2])), err));
  all(set_prop(base.obj, "CharacterGuid.D", Value::i64(static_cast<int32_t>(s.character_guid[3])), err));
  all(set_prop(base.obj, "CharacterName", Value::str(s.name), err));
  if (s.race != 0) all(set_prop(base.obj, "Race", Value::u64(s.race), err));
  if (s.gender != 0) all(set_prop(base.obj, "Gender", Value::u64(s.gender), err));
  if (s.level != 1) all(set_prop(base.obj, "Level", Value::i64(s.level), err));
  all(set_prop(base.obj, "CurrentBreathingTime", Value::f32(kBreathingTime), err));
  subs.push_back(std::move(base));

  repl::Subobject combat = make_component(g.combat_info, "CombatInfo", kChecksumCombatInfo);
  all(set_prop(combat.obj, "bIsDead", Value::boolean(false), err));
  all(set_prop(combat.obj, "PrimaryClassId.Guid", Value::i64(s.class_record_guid), err));
  subs.push_back(std::move(combat));

  repl::Subobject owner;
  all(build_pawn_owner_subobject(s, mode, owner, err));
  subs.push_back(std::move(owner));
  subs.push_back(make_component(g.backpack, "BackpackComponent", kChecksumItemStorage));
  subs.push_back(make_component(g.equipment, "EquipmentComponent", kChecksumItemStorage));
  subs.push_back(make_component(g.quest_storage, "QuestStorageComponent", kChecksumItemStorage));
  subs.push_back(make_component(g.reward_storage, "RewardStorageComponent", kChecksumItemStorage));

  repl::Subobject appearance = make_component(g.appearance, "Character Appearance", kChecksumAppearance);
  all(set_prop(appearance.obj, "SharedAppearanceInfoId.Guid",
               Value::i64(static_cast<int64_t>(s.shared_appearance_info_id)), err));
  all(set_prop(appearance.obj, "CharacterCustomization", Value(s.customization), err));
  subs.push_back(std::move(appearance));
  for (const repl::Subobject &sub : subs) {
    if (!sub.obj.schema) all(record_fail(err, std::string("no schema for component ") + sub.name));
  }
  return ok;
}

bool build_pawn_and_ps_actors(const PlayerSetup &s, GameStateMode mode, repl::Actor &pawn, repl::Actor &ps,
                              std::string &err) {
  const SessionGuids &g = s.guids;
  bool ok = true;
  auto all = [&](bool b) { ok = b && ok; };
  const repl::QuantVec spawn = repl::quantize_vector(10, s.x, s.y, s.z);

  pawn.guid = g.pawn;
  pawn.obj.schema = gen::find_class_schema("PlayerPawn_C");
  all(set_prop(pawn.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated), err));
  repl::RepMovement m;
  m.location = repl::quantize_location(pawn.obj.opts, s.x, s.y, s.z);
  m.rotation = repl::Rot{0, compress_byte(s.yaw_degrees), 0};
  m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
  all(set_prop(pawn.obj, "ReplicatedMovement", Value(m), err));
  all(set_prop(pawn.obj, "RemoteRole", Value::u64(1), err));
  all(set_prop(pawn.obj, "Owner", object(g.player_controller), err));
  all(set_prop(pawn.obj, "Role", Value::u64(4), err));
  pawn.open.archetype = pawn_archetype_export();
  pawn.open.level = persistent_level_export();
  pawn.open.spawn.location = spawn;
  pawn.open.spawn.rotation = repl::Rot{0, compress_short(s.yaw_degrees), 0};
  all(build_pawn_subobjects(s, mode, pawn.subobjects, err));

  ps.guid = g.player_state;
  ps.obj.schema = gen::find_class_schema("AoCPlayerStateBP_C");
  all(set_prop(ps.obj, "AuthServerIDReplicated", Value::u64(kAuthServerIdReplicated), err));
  all(set_prop(ps.obj, "RemoteRole", Value::u64(1), err));
  all(set_prop(ps.obj, "Owner", object(g.player_controller), err));
  all(set_prop(ps.obj, "Role", Value::u64(4), err));
  all(set_prop(ps.obj, "PlayerId", Value::i64(s.player_id), err));
  all(set_prop(ps.obj, "StartTime", Value::i64(static_cast<int64_t>(s.world_time)), err));
  repl::UniqueNetId id;
  id.flags = 8;
  id.id = s.login.unique_id;
  all(set_prop(ps.obj, "UniqueId", Value(id), err));
  all(set_prop(ps.obj, "PlayerNamePrivate", Value::str(player_name_private(s.login.name_option)), err));
  ps.open.archetype = player_state_archetype_export();
  ps.open.level = persistent_level_export();
  ps.open.spawn.location = spawn;
  return ok;
}

repl::Subobject make_second_state_ability(const PlayerSetup &s, std::string &err) {
  repl::Subobject ability = make_component(s.guids.ability, "AbilityComponent", kChecksumAbility);
  repl::ArrayValue known;
  for (const int64_t guid : s.known_abilities) {
    repl::ElementState e;
    e.values[1] = Value::i64(guid);
    known.elements.push_back(std::move(e));
  }
  set_prop(ability.obj, "KnownAbilities", Value(std::move(known)), err);
  repl::ArrayValue records;
  for (const AbilityRecord &r : s.records_to_load) {
    repl::ElementState e;
    e.values[1] = Value::i64(r.guid);
    e.values[2] = Value::i64(r.type_id);
    e.values[3] = Value(repl::NameValue::hardcoded_name(0));
    records.elements.push_back(std::move(e));
  }
  set_prop(ability.obj, "ReplicatedRecordsToLoad", Value(std::move(records)), err);
  return ability;
}

void fill_stat_int32_array(repl::Subobject &stats, const char *field, const StatArray &in, std::string &err) {
  repl::FastArrayState *a = stats.obj.arrays.get(*stats.obj.schema, field);
  if (!a) {
    record_fail(err, std::string("StatsComponent has no fast array ") + field);
    return;
  }
  for (const StatItem &it : in.items) {
    repl::ElementState e;
    e.values[1] = Value(repl::StatInt32Inst{it.record, it.value, it.extended, it.base, it.equipment});
    if (!a->insert(it.id, std::move(e))) {
      record_fail(err, std::string(field) + ": a stat item does not fit");
      return;
    }
  }
  a->set_array_key(in.array_key);
}

repl::Subobject make_second_state_stats(const PlayerSetup &s, std::string &err) {
  repl::Subobject stats = make_component(s.guids.stats, "StatsComponent", kChecksumStats);
  fill_stat_int32_array(stats, "StatRepInt32Owner", s.stats.owner, err);
  fill_stat_int32_array(stats, "StatRepInt32OwnerProxy", s.stats.owner_proxy, err);
  fill_stat_int32_array(stats, "StatRepInt32EveryoneProxy", s.stats.everyone_proxy, err);
  repl::FastArrayState *f = stats.obj.arrays.get(*stats.obj.schema, "StatRepFloatArrayEveryoneProxy");
  if (!f) {
    record_fail(err, "StatsComponent has no StatRepFloatArrayEveryoneProxy");
    return stats;
  }
  for (const FloatStatItem &it : s.stats.float_everyone_proxy) {
    repl::ElementState e;
    e.values[1] = Value::i64(it.record);
    e.values[2] = Value(repl::ArrayValue{});
    if (!f->insert(it.id, std::move(e))) record_fail(err, "StatRepFloatArrayEveryoneProxy: an item does not fit");
  }
  f->set_array_key(s.stats.float_array_key);
  return stats;
}

void populate_base_held_items(const HeldItemsSetup &held_items, repl::Subobject &base, std::string &err) {
  if (held_items.items.empty()) return;
  repl::FastArrayState *held = base.obj.arrays.get(*base.obj.schema, "HeldItems");
  if (!held) {
    record_fail(err, "BaseCharacterInfo has no HeldItems");
    return;
  }
  for (const HeldItem &it : held_items.items) {
    repl::ElementState e;
    e.values[1] = Value::u64(it.slot);
    e.values[2] = Value::i64(it.item_guid);
    e.values[3] = Value::u64(it.enchantment);
    if (!held->insert(it.id, std::move(e))) record_fail(err, "HeldItems: an item does not fit");
  }
  held->set_array_key(held_items.array_key);
}

void populate_second_state_base(const PlayerSetup &s, repl::Subobject &base, std::string &err) {
  set_prop(base.obj, "ServerId", Value::i64(s.server_id), err);
  set_prop(base.obj, "bIsPhasedOut", Value::boolean(true), err);
  set_prop(base.obj, "bClientIsReadyToPhaseIn", Value::boolean(false), err);
  set_prop(base.obj, "bHasBeenPossessed", Value::boolean(true), err);
  if (s.orgs_at_war_toast) set_prop(base.obj, "bShouldSendOrgsAtWarWithToast", Value::boolean(true), err);
  if (!s.attuned_blocks.empty()) {
    repl::ArrayValue blocks;
    for (const auto &indices : s.attuned_blocks) {
      repl::ArrayValue inner;
      for (const uint32_t index : indices) {
        repl::ElementState e;
        e.values[1] = Value::u64(index);
        inner.elements.push_back(std::move(e));
      }
      repl::ElementState block;
      block.values[1] = Value(std::move(inner));
      blocks.elements.push_back(std::move(block));
    }
    set_prop(base.obj, "WorldAttunements.AttunedBlocks", Value(std::move(blocks)), err);
  }
  populate_base_held_items(s.held_items, base, err);
}

void populate_second_state_owner_and_app(const PlayerSetup &s, repl::Subobject *owner, repl::Subobject *appearance,
                                         std::string &err) {
  if (owner) {
    set_prop(owner->obj, "WeaponSkillLineId.Guid", Value::i64(s.weapon_skill_line.guid), err);
    set_prop(owner->obj, "WeaponSkillLineId.Name", Value(repl::NameValue::plain(s.weapon_skill_line.name)), err);
    repl::ArrayValue containers;
    containers.elements.resize(3);
    for (size_t i = 0; i < s.resource_bags.size(); ++i) {
      if (s.resource_bags[i].present) containers.elements[i].values[1] = object(s.guids.resource_storage[i]);
    }
    set_prop(owner->obj, "ResourceContainers", Value(std::move(containers)), err);
  }
  if (appearance && !s.appearance_ids.empty()) {
    repl::ArrayValue ids;
    for (const repl::AppearanceId &id : s.appearance_ids) {
      repl::ElementState e;
      e.values[1] = Value(id);
      ids.elements.push_back(std::move(e));
    }
    set_prop(appearance->obj, "AppearanceIDs", Value(std::move(ids)), err);
  }
}

}  // namespace

PlayerBootstrap::PlayerBootstrap(PlayerSetup setup, GameStateMode mode) : setup_(std::move(setup)), mode_(mode) {
  ok_ = build_actors();
}

bool PlayerBootstrap::fail(std::string message) {
  if (error_.empty()) error_ = std::move(message);
  return false;
}

bool PlayerBootstrap::build_actors() {
  std::string err;
  if (!validate_and_load_items(setup_, items_, err)) return fail(std::move(err));
  bool ok = true;
  auto all = [&](bool b) { ok = b && ok; };
  all(build_pc_and_gs_actors(setup_, mode_, pc_, gs_, err));
  all(build_pawn_and_ps_actors(setup_, mode_, pawn_, ps_, err));
  if (!pc_.obj.schema || !gs_.obj.schema || !pawn_.obj.schema || !ps_.obj.schema) {
    all(record_fail(err, "a class schema is missing"));
  }
  if (!err.empty()) fail(std::move(err));
  return ok;
}

PlayerBootstrap::Checkpoint PlayerBootstrap::checkpoint() {
  node_maps_before_.reset();
  Checkpoint c;
  c.player_controller = pc_shadow_;
  c.game_state = gs_shadow_;
  c.pawn = pawn_shadow_;
  c.player_state = ps_shadow_;
  c.second_state_sent = second_state_sent_;
  for (const auto &[id, e] : effect_actors_) c.effect_shadows[id] = e.shadow;
  if (second_state_added_) {
    if (const repl::Subobject *base = pawn_.find(setup_.guids.base_character_info)) c.base_arrays = base->obj.arrays;
  }
  if (xp_tracker_added_) c.leveling = leveling_;
  return c;
}

void PlayerBootstrap::restore(const Checkpoint &c) {
  pc_shadow_ = c.player_controller;
  gs_shadow_ = c.game_state;
  pawn_shadow_ = c.pawn;
  ps_shadow_ = c.player_state;
  second_state_sent_ = c.second_state_sent;
  for (auto &[id, e] : effect_actors_) {
    const auto it = c.effect_shadows.find(id);
    if (it != c.effect_shadows.end()) e.shadow = it->second;
  }
  if (c.base_arrays) {
    if (repl::Subobject *base = component(setup_.guids.base_character_info)) base->obj.arrays = *c.base_arrays;
  }
  if (c.leveling && xp_tracker_added_) {
    leveling_ = *c.leveling;
    sync_xp_tracker();
  }
  if (node_maps_before_) {
    gs_.obj.arrays = std::move(*node_maps_before_);
    node_maps_before_.reset();
  }
}

void PlayerBootstrap::add_second_state_components() {
  const PlayerSetup &s = setup_;
  const SessionGuids &g = s.guids;
  std::string err;

  set_prop(pawn_.obj, "RemoteRole", Value::u64(2), err);
  set_prop(pawn_.obj, "bHidden", Value::boolean(true), err);
  set_prop(pawn_.obj, "Instigator", object(g.pawn), err);
  set_prop(pawn_.obj, "PlayerState", object(g.player_state), err);
  set_prop(pawn_.obj, "Controller", object(g.player_controller), err);

  repl::Subobject alignment = make_component(g.alignment, "AlignmentComponent", kChecksumAlignment);
  repl::AlignmentContainer c;
  const repl::GuidValue none{0, 0, 0, 0};
  const repl::GuidValue self{s.character_guid[0], s.character_guid[1], s.character_guid[2], s.character_guid[3]};
  c.instances = {
      {kAlignmentKeyA, {none}}, {kAlignmentKeyB, {none}}, {kAlignmentKeyCharacter, {self}}, {kAlignmentKeyC, {none}}};
  set_prop(alignment.obj, "Alignments", Value(c), err);

  repl::Subobject ability = make_second_state_ability(s, err);
  repl::Subobject stats = make_second_state_stats(s, err);

  std::vector<repl::Subobject> resources;
  for (size_t i = 0; i < s.resource_bags.size(); ++i) {
    if (!s.resource_bags[i].present) continue;
    repl::Subobject resource;
    if (!make_resource_component(g.resource_storage[i], i, resource))
      record_fail(err, "no schema for UResourceStorageComponent");
    resources.push_back(std::move(resource));
  }

  pawn_.subobjects.insert(pawn_.subobjects.begin(), std::move(alignment));
  pawn_.subobjects.insert(pawn_.subobjects.begin() + 3, std::move(ability));
  pawn_.subobjects.insert(pawn_.subobjects.begin() + 4, std::move(stats));
  for (repl::Subobject &resource : resources) pawn_.subobjects.push_back(std::move(resource));

  if (repl::Subobject *base = component(g.base_character_info)) populate_second_state_base(s, *base, err);
  populate_second_state_owner_and_app(s, component(g.owner_info), component(g.appearance), err);
  if (!err.empty()) fail(std::move(err));
  second_state_added_ = true;
  for (const auto &[record, value] : current_stats()) stat_layout_.insert(record);
}

std::optional<Send> PlayerBootstrap::finish(Role role, bool opens, const char *what, std::optional<Emission> e) const {
  if (!e) return std::nullopt;
  Send s;
  s.role = role;
  s.opens_channel = opens;
  s.what = what;
  s.emission = std::move(*e);
  return s;
}

std::optional<Send> PlayerBootstrap::update(Role role, const char *what, repl::Actor &actor, ActorShadow &shadow) {
  bool failed = false;
  return finish(role, false, what, repl::update_actor(actor, shadow, acks_, &failed));
}

std::optional<Send> PlayerBootstrap::rpcs(Role role, const char *what, std::span<const repl::RpcRequest> calls) {
  return finish(role, false, what, repl::call_rpcs(role == Role::Pawn ? pawn_ : pc_, calls, acks_));
}

}  // namespace aoc::worldd::bootstrap
