// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include <utility>
#include <vector>

#include "bootstrap/bootstrap.h"
#include "bootstrap/bootstrap_internal.h"
#include "bootstrap/captured_gamestate.h"
#include "bootstrap/captured_level_list.h"
#include "content/content_db.h"

namespace aoc::worldd::bootstrap {

using protocol::emit::BunchWriter;
using protocol::emit::FIntrepidNetworkGUID;
using repl::ActorShadow;
using repl::Emission;
using repl::ExportChain;
using repl::Value;
namespace schema = ::aoc::protocol::schema;
namespace gen = ::aoc::protocol::schema::gen;

std::optional<Send> PlayerBootstrap::player_controller_open() {
  if (!ok_) return std::nullopt;
  bool failed = false;
  return finish(Role::PlayerController, true, "PlayerController open",
                repl::open_actor(pc_, pc_shadow_, acks_, &failed));
}

std::optional<Send> PlayerBootstrap::hud() {
  if (!ok_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientSetHUD");
  if (!rpc) return std::nullopt;
  repl::RpcParams params;
  if (!repl::set_rpc_param(*rpc, params, "NewHUDClass", object(hud_class_guid()))) return std::nullopt;
  const ExportChain hud_class = hud_class_export();
  return finish(
      Role::PlayerController, false, "ClientSetHUD",
      repl::call_rpc(pc_, nullptr, "ClientSetHUD", params, std::span<const ExportChain>(&hud_class, 1), acks_));
}

std::optional<Send> PlayerBootstrap::voice_and_level_list() {
  if (!ok_) return std::nullopt;
  const schema::ClassSchema &cls = *pc_.obj.schema;
  std::vector<repl::RpcRequest> calls(3);
  calls[0].rpc = "ClientVoiceHandshakeComplete";
  calls[1].rpc = "ClientEnableNetworkVoice";
  calls[2].rpc = "ClientUpdateMultipleLevelsStreamingStatusCompressed";
  const schema::RpcSchema *voice = cls.rpc(calls[1].rpc);
  const schema::RpcSchema *levels = cls.rpc(calls[2].rpc);
  if (!cls.rpc(calls[0].rpc) || !voice || !levels) return std::nullopt;
  if (!repl::set_rpc_param(*voice, calls[1].params, "bEnable", Value::boolean(false))) return std::nullopt;
  repl::ArrayValue bytes;
  bytes.elements.reserve(sizeof(captured::kLevelStatuses));
  for (const uint8_t b : captured::kLevelStatuses) {
    repl::ElementState e;
    e.values[1] = Value::u64(b);
    bytes.elements.push_back(std::move(e));
  }
  if (!repl::set_rpc_param(*levels, calls[2].params, "LevelStatuses", Value(std::move(bytes)))) return std::nullopt;
  return rpcs(Role::PlayerController, "voice handshake, level streaming list", calls);
}

std::optional<Send> PlayerBootstrap::flush_bandwidth_and_time() {
  if (!ok_) return std::nullopt;
  const schema::ClassSchema &cls = *pc_.obj.schema;
  std::vector<repl::RpcRequest> calls(3);
  calls[0].rpc = "ClientFlushLevelStreaming";
  calls[1].rpc = "ClientCapBandwidth";
  calls[2].rpc = "ClientSetTime";
  const schema::RpcSchema *cap = cls.rpc(calls[1].rpc);
  const schema::RpcSchema *time = cls.rpc(calls[2].rpc);
  if (!cls.rpc(calls[0].rpc) || !cap || !time) return std::nullopt;
  const ClientTimes t = client_times(setup_.world_time);
  if (!repl::set_rpc_param(*cap, calls[1].params, "Cap", Value::i64(kBandwidthCap)) ||
      !repl::set_rpc_param(*time, calls[2].params, "NewGameTime", Value::f64(t.game_time)) ||
      !repl::set_rpc_param(*time, calls[2].params, "NewSynchronizedTime", Value::f64(t.synchronized_time))) {
    return std::nullopt;
  }
  return rpcs(Role::PlayerController, "flush, bandwidth cap, clock", calls);
}

std::optional<Send> PlayerBootstrap::set_time(double world_time) {
  if (!ok_) return std::nullopt;
  const schema::RpcSchema *time = pc_.obj.schema->rpc("ClientSetTime");
  if (!time) return std::nullopt;
  const ClientTimes t = client_times(world_time);
  repl::RpcParams params;
  if (!repl::set_rpc_param(*time, params, "NewGameTime", Value::f64(t.game_time)) ||
      !repl::set_rpc_param(*time, params, "NewSynchronizedTime", Value::f64(t.synchronized_time))) {
    return std::nullopt;
  }
  return finish(Role::PlayerController, false, "ClientSetTime",
                repl::call_rpc(pc_, nullptr, "ClientSetTime", params, {}, acks_));
}

std::optional<Emission> PlayerBootstrap::game_state_captured_open() const {
  Emission e;
  e.exports.emplace_back();
  write_bits_of(e.exports.back(), captured::kGameStateOpenExports);
  write_bits_of(e.payload, captured::kGameStateOpenContent);
  for (const captured::CapturedGuid &c : captured::kGameStateOpenExported) {
    FIntrepidNetworkGUID g;
    g.ObjectId = c.object_id;
    g.ServerId = c.server_id;
    g.Randomizer = c.randomizer;
    e.exported.push_back(g);
  }
  e.exported_by_bunch.push_back(e.exported);
  return e;
}

std::optional<Send> PlayerBootstrap::game_state_open() {
  if (!ok_) return std::nullopt;
  if (mode_ == GameStateMode::Captured) {
    return finish(Role::GameState, true, "GameState open (captured)", game_state_captured_open());
  }
  // GameModeClass names a class by path, which the replicator does not export
  // on its own: add it to the actor's block.
  std::vector<repl::ContentBlock> blocks;
  repl::ShadowUndo undo;
  if (!repl::collect_open_blocks(gs_, gs_shadow_, blocks, &undo)) return std::nullopt;
  if (blocks.empty() || blocks[0].sub) {
    undo.rollback(gs_shadow_);
    return std::nullopt;
  }
  blocks[0].exports.push_back(game_mode_class_export());
  auto e = repl::assemble_open(gs_, blocks, acks_);
  if (!e) undo.rollback(gs_shadow_);
  const char *label = mode_ == GameStateMode::Tier1   ? "GameState open (tier 1)"
                      : mode_ == GameStateMode::Tier3 ? "GameState open (tier 3)"
                                                      : "GameState open (tier 2)";
  return finish(Role::GameState, true, label, std::move(e));
}

std::optional<Send> PlayerBootstrap::load_tracker_before_possession() {
  if (!ok_) return std::nullopt;
  if (!pc_.obj.state.set(*pc_.obj.schema, "CharacterLoadTracker", Value(load_tracker(kLoadTrackerBeforePossession)))) {
    return std::nullopt;
  }
  return update(Role::PlayerController, "CharacterLoadTracker 1011...1", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::pawn_open() {
  if (!ok_) return std::nullopt;
  bool failed = false;
  return finish(Role::Pawn, true, "pawn open", repl::open_actor(pawn_, pawn_shadow_, acks_, &failed));
}

std::optional<Send> PlayerBootstrap::restart_camera_rotation() {
  if (!ok_) return std::nullopt;
  const schema::ClassSchema &cls = *pc_.obj.schema;
  std::vector<repl::RpcRequest> calls(3);
  calls[0].rpc = "ClientRestart";
  calls[1].rpc = "ClientSetCameraMode";
  calls[2].rpc = "ClientSetRotation";
  const schema::RpcSchema *restart = cls.rpc(calls[0].rpc);
  const schema::RpcSchema *camera = cls.rpc(calls[1].rpc);
  const schema::RpcSchema *rotation = cls.rpc(calls[2].rpc);
  if (!restart || !camera || !rotation) return std::nullopt;
  if (!repl::set_rpc_param(*restart, calls[0].params, "NewPawn", object(setup_.guids.pawn)) ||
      !repl::set_rpc_param(*camera, calls[1].params, "NewCamMode",
                           Value(repl::NameValue::hardcoded_name(kCameraModeEName))) ||
      !repl::set_rpc_param(*rotation, calls[2].params, "NewRotation",
                           Value(repl::Rot{0, compress_short(setup_.yaw_degrees), 0})) ||
      !repl::set_rpc_param(*rotation, calls[2].params, "bResetCamera", Value::boolean(true))) {
    return std::nullopt;
  }
  return rpcs(Role::PlayerController, "ClientRestart, camera mode, rotation", calls);
}

std::optional<Send> PlayerBootstrap::possession_update() {
  if (!ok_) return std::nullopt;
  const SessionGuids &g = setup_.guids;
  auto set = [&](std::string_view path, Value v) { return pc_.obj.state.set(*pc_.obj.schema, path, std::move(v)); };
  if (!set("Pawn", object(g.pawn)) || !set("ControllersOriginalPawn", object(g.pawn)) ||
      !set("CharacterLoadTracker", Value(load_tracker(kLoadTrackerAfterPossession))) ||
      !set("Name", Value::str(setup_.name)) || !set("AttachmentReplication.AttachParent", object(g.pawn)) ||
      !set("AttachmentReplication.RelativeScale3D",
           Value(repl::QuantVec{repl::QuantVec::Mode::Scaled, kRelativeScale100, kRelativeScale100,
                                kRelativeScale100})) ||
      !set("AttachmentReplication.AttachComponent", object(g.collision_cylinder))) {
    return std::nullopt;
  }
  std::vector<repl::ContentBlock> blocks;
  repl::ShadowUndo undo;
  if (!repl::collect_update_blocks(pc_, pc_shadow_, blocks, &undo) || blocks.empty() || blocks[0].sub) {
    undo.rollback(pc_shadow_);
    return std::nullopt;
  }
  blocks[0].exports.push_back(
      repl::component_export(g.pawn, g.collision_cylinder, "CollisionCylinder", kChecksumCollisionCylinder));
  auto e = repl::assemble_blocks(pc_, blocks, acks_);
  if (!e) undo.rollback(pc_shadow_);
  return finish(Role::PlayerController, false, "PlayerController after possession", std::move(e));
}

std::optional<Send> PlayerBootstrap::pawn_second_state() {
  if (!ok_) return std::nullopt;
  if (!second_state_added_) {
    add_second_state_components();
    if (!error_.empty()) return std::nullopt;
  }
  if (second_state_sent_) return std::nullopt;
  // The original also sent AuthServerIDReplicated = 0 and CombatInfo.bIsDead =
  // 0 again at possession: forgetting them here keeps a restored checkpoint's
  // retry identical to the capture.
  pawn_shadow_.actor.invalidate(1);
  if (const auto it = pawn_shadow_.subobjects.find(setup_.guids.combat_info.ObjectId);
      it != pawn_shadow_.subobjects.end()) {
    it->second.invalidate(5);
  }
  auto s = update(Role::Pawn, "pawn second state", pawn_, pawn_shadow_);
  if (s) second_state_sent_ = true;
  return s;
}

std::optional<Send> PlayerBootstrap::player_state_open() {
  if (!ok_) return std::nullopt;
  bool failed = false;
  return finish(Role::PlayerState, true, "PlayerState open", repl::open_actor(ps_, ps_shadow_, acks_, &failed));
}

namespace {

repl::DesignDataId adventuring_key() {
  return repl::DesignDataId::named(content::kGuidAdventuring, content::kTypeLevelingTypeDef, "Adventuring");
}

repl::DesignDataId tree_key(int64_t guid) {
  repl::DesignDataId d;
  d.guid = guid;
  d.type_id = content::kTypeSkillTree;
  return d;
}

repl::DesignDataId rank_key(int64_t guid) {
  repl::DesignDataId d;
  d.guid = guid;
  d.type_id = content::kTypeSkillRank;
  return d;
}

repl::Value ftue_list(const std::vector<int64_t> &ids) {
  repl::ArrayValue list;
  list.elements.reserve(ids.size());
  for (const int64_t id : ids) {
    repl::ElementState e;
    e.values[1] = Value::i64(id);
    list.elements.push_back(std::move(e));
  }
  return Value(std::move(list));
}

bool apply_initial_progression(PlayerBootstrap &self, const ProgressSetup &p) {
  if (!self.set_progress(p.xp, p.debt)) return false;
  for (const TreeSetup &t : p.trees) {
    if (!self.set_tree_ranks(t.tree, t.ranks)) return false;
  }
  for (const ProgressSetup::Profession &pr : p.professions) {
    if (!self.set_profession(pr.leveling_type, pr.xp, pr.certification)) return false;
  }
  for (const ProgressSetup::WeaponLine &w : p.weapon_lines) {
    if (!self.set_weapon_line(w.leveling_type, w.xp)) return false;
  }
  return true;
}

bool append_send(std::vector<Send> &out, std::optional<Send> s) {
  if (!s) return false;
  out.push_back(std::move(*s));
  return true;
}

bool append_storage_adds(PlayerBootstrap &self, ItemStorage storage, std::vector<Send> &out) {
  auto adds = self.storage_adds(storage);
  if (!adds) return false;
  for (Send &s : *adds) out.push_back(std::move(s));
  return true;
}

bool append_resource_containers(PlayerBootstrap &self, const std::array<ResourceBagSetup, 3> &bags,
                                std::vector<Send> &out) {
  for (int i = 0; i < 3; ++i) {
    if (!bags[static_cast<size_t>(i)].present) continue;
    const auto storage = static_cast<ItemStorage>(static_cast<int>(ItemStorage::Resource0) + i);
    if (!append_storage_adds(self, storage, out) ||
        !append_send(out, self.storage_payload(resource_kind(i), Role::Pawn))) {
      return false;
    }
  }
  return true;
}

bool append_pre_possession_join(PlayerBootstrap &self, const std::array<ResourceBagSetup, 3> &bags,
                                std::vector<Send> &out) {
  return append_send(out, self.player_controller_open()) && append_send(out, self.hud()) &&
         append_send(out, self.voice_and_level_list()) && append_send(out, self.flush_bandwidth_and_time()) &&
         append_send(out, self.game_state_open()) && append_send(out, self.load_tracker_before_possession()) &&
         append_storage_adds(self, ItemStorage::Backpack, out) && append_send(out, self.pawn_open()) &&
         append_send(out, self.storage_payload(StorageKind::Backpack, Role::Pawn)) &&
         append_storage_adds(self, ItemStorage::Equipment, out) &&
         append_send(out, self.storage_payload(StorageKind::Equipment, Role::Pawn)) &&
         append_storage_adds(self, ItemStorage::Quest, out) &&
         append_send(out, self.storage_payload(StorageKind::QuestStorage, Role::Pawn)) &&
         append_resource_containers(self, bags, out);
}

bool append_post_possession_join(PlayerBootstrap &self, std::vector<Send> &out) {
  return append_send(out, self.restart_camera_rotation()) && append_send(out, self.possession_update()) &&
         append_send(out, self.pawn_second_state()) && append_send(out, self.player_state_open()) &&
         append_send(out, self.xp_tracker_open());
}

}  // namespace

void PlayerBootstrap::sync_xp_tracker() {
  for (repl::Subobject &sub : pc_.subobjects) {
    if (sub.guid == setup_.guids.xp_tracker) sub.obj.arrays = leveling_.arrays();
  }
}

bool PlayerBootstrap::set_progress(int64_t xp, int64_t debt) {
  if (!ok_ || xp < 0 || debt < 0) return false;
  if (!leveling_.set_experience(adventuring_key(), xp)) return false;
  if (!leveling_.set_experience_debt(adventuring_key(), debt)) return false;
  if (xp_tracker_added_) sync_xp_tracker();
  return true;
}

bool PlayerBootstrap::set_profession(int64_t leveling_type, int64_t xp, int64_t certification) {
  if (!ok_ || leveling_type == 0 || certification == 0 || xp < 0) return false;
  repl::DesignDataId key;
  key.guid = leveling_type;
  key.type_id = content::kTypeLevelingTypeDef;
  if (!leveling_.set_certification(key, certification)) return false;
  if (xp > 0 || leveling_.experience(key)) {
    if (!leveling_.set_experience(key, xp)) return false;
  }
  if (xp_tracker_added_) sync_xp_tracker();
  return true;
}

bool PlayerBootstrap::set_weapon_line(int64_t leveling_type, int64_t xp) {
  if (!ok_ || leveling_type == 0 || xp < 0) return false;
  repl::DesignDataId key;
  key.guid = leveling_type;
  key.type_id = content::kTypeLevelingTypeDef;
  if (xp > 0 || leveling_.experience(key)) {
    if (!leveling_.set_experience(key, xp)) return false;
  }
  if (xp_tracker_added_) sync_xp_tracker();
  return true;
}

bool PlayerBootstrap::set_tree_ranks(int64_t tree, const std::vector<int64_t> &ranks) {
  if (!ok_ || tree == 0) return false;
  if (ranks.empty()) {
    leveling_.erase_purchased_ranks(tree_key(tree));
  } else {
    std::vector<repl::DesignDataId> ids;
    ids.reserve(ranks.size());
    for (const int64_t g : ranks) ids.push_back(rank_key(g));
    if (!leveling_.set_purchased_ranks(tree_key(tree), ids)) return false;
  }
  if (xp_tracker_added_) sync_xp_tracker();
  return true;
}

bool PlayerBootstrap::resend_tree(int64_t tree) {
  if (!ok_ || !xp_tracker_added_ || tree == 0) return false;
  if (!leveling_.resend_purchased_ranks(tree_key(tree))) return false;
  sync_xp_tracker();
  return true;
}

std::vector<int64_t> PlayerBootstrap::tree_ranks(int64_t tree) const {
  std::vector<int64_t> out;
  if (const auto ids = leveling_.purchased_ranks(tree_key(tree))) {
    for (const repl::DesignDataId &d : *ids) out.push_back(d.guid);
  }
  return out;
}

std::optional<Send> PlayerBootstrap::xp_tracker_open() {
  if (!ok_) return std::nullopt;
  if (!xp_tracker_added_) {
    if (!apply_initial_progression(*this, setup_.progress)) return std::nullopt;
    repl::Subobject tracker = make_component(setup_.guids.xp_tracker, "XPTracker", kChecksumXPTracker);
    if (!tracker.obj.schema) {
      fail("no schema for the XPTracker (ULevelingComponent)");
      return std::nullopt;
    }
    tracker.obj.arrays = leveling_.arrays();
    pc_.subobjects.push_back(std::move(tracker));
    xp_tracker_added_ = true;
  }
  if (!quest_added_ && !add_quest_component()) return std::nullopt;
  if (!ftue_added_ && !add_ftue_component()) return std::nullopt;
  return update(Role::PlayerController, "XPTracker", pc_, pc_shadow_);
}

bool PlayerBootstrap::add_ftue_component() {
  repl::Subobject ftue = make_component(setup_.guids.ftue, "FTUE Component", kChecksumFTUE);
  if (!ftue.obj.schema) return fail("no schema for the tutorial component (UFTUEComponent)");
  if (setup_.completed_ftues.size() > kMaxCompletedFtues) {
    return fail(
        "the character has more finished tutorials than the tutorial "
        "component holds");
  }
  if (!setup_.completed_ftues.empty() &&
      !ftue.obj.state.set(*ftue.obj.schema, "CompletedFTUEs", ftue_list(setup_.completed_ftues))) {
    return fail("CompletedFTUEs refused");
  }
  pc_.subobjects.push_back(std::move(ftue));
  ftue_added_ = true;
  return true;
}

bool PlayerBootstrap::add_quest_component() {
  repl::Subobject quest = make_component(setup_.guids.quest, "QuestConsumerComponent", kChecksumQuest);
  if (!quest.obj.schema) return fail("no schema for the quest component (UQuestConsumerComponent)");
  quest.obj.arrays = std::move(quest_arrays_);
  quest_arrays_ = repl::FastArrays{};
  pc_.subobjects.push_back(std::move(quest));
  quest_added_ = true;
  return true;
}

repl::FastArrays *PlayerBootstrap::quest_arrays() {
  if (!ok_) return nullptr;
  if (!quest_added_) return &quest_arrays_;
  repl::Subobject *quest = pc_.find(setup_.guids.quest);
  return quest ? &quest->obj.arrays : nullptr;
}

std::optional<Send> PlayerBootstrap::quest_update() {
  if (!ok_ || !quest_added_) return std::nullopt;
  return update(Role::PlayerController, "QuestConsumerComponent", pc_, pc_shadow_);
}

bool PlayerBootstrap::set_completed_ftues(const std::vector<int64_t> &ids) {
  if (!ok_ || ids.size() > kMaxCompletedFtues) return false;
  setup_.completed_ftues = ids;
  if (!ftue_added_) return true;
  repl::Subobject *ftue = pc_.find(setup_.guids.ftue);
  if (!ftue) return false;
  return ftue->obj.state.set(*ftue->obj.schema, "CompletedFTUEs", ftue_list(ids));
}

std::optional<Send> PlayerBootstrap::ftue_update() {
  if (!ok_ || !ftue_added_) return std::nullopt;
  return update(Role::PlayerController, "FTUE Component", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::leveling_update() {
  if (!ok_ || !xp_tracker_added_) return std::nullopt;
  sync_xp_tracker();
  return update(Role::PlayerController, "XPTracker", pc_, pc_shadow_);
}

bool PlayerBootstrap::set_level(int level) {
  if (!ok_ || level < 1 || level > 255) return false;
  repl::Subobject *base = component(setup_.guids.base_character_info);
  if (!base) return false;
  setup_.level = level;
  return base->obj.state.set(*base->obj.schema, "Level", Value::i64(level));
}

bool PlayerBootstrap::set_class(int64_t class_record_guid) {
  if (!ok_ || class_record_guid == 0) return false;
  repl::Subobject *combat = component(setup_.guids.combat_info);
  if (!combat) return false;
  setup_.class_record_guid = class_record_guid;
  return combat->obj.state.set(*combat->obj.schema, "PrimaryClassId.Guid", Value::i64(class_record_guid));
}

bool PlayerBootstrap::set_in_combat(bool in_combat) {
  if (!ok_) return false;
  repl::Subobject *combat = component(setup_.guids.combat_info);
  if (!combat || !combat->obj.schema->command("bInCombat")) return false;
  return combat->obj.state.set(*combat->obj.schema, "bInCombat", Value::boolean(in_combat));
}

bool PlayerBootstrap::set_using_ranged_weapon(bool ranged) {
  if (!ok_) return false;
  repl::Subobject *combat = component(setup_.guids.combat_info);
  if (!combat || !combat->obj.schema->command("bIsUsingRangedWeapon")) return false;
  return combat->obj.state.set(*combat->obj.schema, "bIsUsingRangedWeapon", Value::boolean(ranged));
}

bool PlayerBootstrap::set_active_targets(const std::array<FIntrepidNetworkGUID, 2> &targets) {
  if (!ok_) return false;
  repl::Subobject *combat = component(setup_.guids.combat_info);
  if (!combat || !combat->obj.schema->command("ActiveTargets.ActiveTargets")) return false;
  repl::ArrayValue list;
  for (const FIntrepidNetworkGUID &t : targets) {
    repl::ElementState e;
    e.values[1] = Value(t);
    list.elements.push_back(std::move(e));
  }
  return combat->obj.state.set(*combat->obj.schema, "ActiveTargets.ActiveTargets", Value(std::move(list)));
}

bool PlayerBootstrap::set_hover_target(const std::array<uint32_t, 4> &words) {
  if (!ok_) return false;
  repl::Subobject *combat = component(setup_.guids.combat_info);
  constexpr const char *kPaths[4] = {"HoverTargetGuid.A", "HoverTargetGuid.B", "HoverTargetGuid.C",
                                     "HoverTargetGuid.D"};
  if (!combat) return false;
  for (const char *path : kPaths) {
    if (!combat->obj.schema->command(path)) return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!combat->obj.state.set(*combat->obj.schema, kPaths[i], Value::i64(static_cast<int32_t>(words[i]))))
      return false;
  }
  return true;
}

bool PlayerBootstrap::set_pvp_force_flag(bool on) {
  if (!ok_) return false;
  repl::Subobject *combat = component(setup_.guids.combat_info);
  if (!combat || !combat->obj.schema->command("bPvPForceFlag")) return false;
  return combat->obj.state.set(*combat->obj.schema, "bPvPForceFlag", Value::boolean(on));
}

void PlayerBootstrap::note_trigger_pvp_flag(int64_t instance, bool flag) {
  if (!flag) {
    trigger_pvp_.erase(instance);
    return;
  }
  trigger_pvp_.insert(instance);
  while (trigger_pvp_.size() > kMaxTriggerFlags) trigger_pvp_.erase(trigger_pvp_.begin());
}

bool PlayerBootstrap::set_weapon_skill_line(int64_t guid, const std::string &name) {
  if (!ok_ || !second_state_added_ || guid == 0) return false;
  repl::Subobject *owner = component(setup_.guids.owner_info);
  if (!owner) return false;
  setup_.weapon_skill_line.guid = guid;
  setup_.weapon_skill_line.name = name;
  return owner->obj.state.set(*owner->obj.schema, "WeaponSkillLineId.Guid", Value::i64(guid)) &&
         owner->obj.state.set(*owner->obj.schema, "WeaponSkillLineId.Name", Value(repl::NameValue::plain(name)));
}

bool PlayerBootstrap::mark_temp_health() {
  if (!ok_ || !second_state_added_) return false;
  repl::Subobject *stats = component(setup_.guids.stats);
  if (!stats) return false;
  repl::FastArrayState *f = stats->obj.arrays.get(*stats->obj.schema, "StatRepFloatArrayEveryoneProxy");
  if (!f || f->items().empty()) return false;
  std::vector<std::pair<int32_t, repl::ElementState>> again;
  for (const repl::FastArrayItem &it : f->items()) again.emplace_back(it.id, it.values);
  for (auto &[id, values] : again) {
    if (!f->change(id, std::move(values))) return false;
  }
  return true;
}

std::optional<Send> PlayerBootstrap::pawn_state_update(const char *what) {
  if (!ok_ || !second_state_sent_) return std::nullopt;
  return update(Role::Pawn, what, pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::level_up_broadcast(int64_t category_guid, int level) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pawn_.obj.schema->rpc("BroadcastPlayLevelUpEffects");
  if (!rpc) return std::nullopt;
  repl::RpcParams params;
  if (!repl::set_rpc_param(*rpc, params, "Category", Value::i64(category_guid)) ||
      !repl::set_rpc_param(*rpc, params, "InLevel", Value::i64(level))) {
    return std::nullopt;
  }
  return finish(Role::Pawn, false, "BroadcastPlayLevelUpEffects",
                repl::call_rpc(pawn_, nullptr, "BroadcastPlayLevelUpEffects", params, {}, acks_));
}

std::optional<Send> PlayerBootstrap::skill_on_actionbar(const repl::SkillOnActionbar &u) {
  if (!ok_) return std::nullopt;
  return finish(Role::PlayerController, false, "ClientUpdateSkillOnActionbar",
                repl::client_update_skill_on_actionbar(pc_, u, acks_));
}

std::optional<std::vector<Send>> PlayerBootstrap::join_sequence() {
  if (!ok_) return std::nullopt;
  const Checkpoint before = checkpoint();
  const repl::ExportAcks acks_before = acks_;
  std::vector<Send> out;
  const bool done =
      append_pre_possession_join(*this, setup_.resource_bags, out) && append_post_possession_join(*this, out);
  if (!done) {
    restore(before);
    acks_ = acks_before;
    return std::nullopt;
  }
  return out;
}

std::optional<Send> PlayerBootstrap::phase_in() {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const SessionGuids &g = setup_.guids;
  repl::Subobject *base = component(g.base_character_info);
  if (!base) return std::nullopt;
  if (!pawn_.obj.state.set(*pawn_.obj.schema, "bHidden", Value::boolean(false)) ||
      !base->obj.state.set(*base->obj.schema, "bIsPhasedOut", Value::boolean(false)) ||
      !base->obj.state.set(*base->obj.schema, "bClientIsReadyToPhaseIn", Value::boolean(true))) {
    return std::nullopt;
  }
  return update(Role::Pawn, "phase-in", pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::combat_settings_echo(std::span<const PropertyValue> values) {
  if (!ok_) return std::nullopt;
  for (const PropertyValue &v : values) {
    if (!pc_.obj.state.set(*pc_.obj.schema, v.path, v.value)) return std::nullopt;
  }
  return update(Role::PlayerController, "combat settings echo", pc_, pc_shadow_);
}

}  // namespace aoc::worldd::bootstrap
