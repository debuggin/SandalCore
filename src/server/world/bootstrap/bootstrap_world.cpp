// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include <string>
#include <utility>
#include <vector>

#include "bootstrap/bootstrap.h"
#include "bootstrap/bootstrap_internal.h"
#include "protocol/repl/dialogue_rpc.h"

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

using ItemMap = std::map<std::pair<uint8_t, int32_t>, repl::ItemInstance>;

bool fill_storage_payload(StorageKind kind, const PlayerSetup &setup, const ItemMap &items,
                          const FIntrepidNetworkGUID &target_guid, repl::StoragePayload &payload) {
  const StorageInfo &info = storage_info(kind);
  payload.target = target_guid;
  int slots = info.slots;
  payload.dimension = repl::Vec2d{info.dimension_x, info.dimension_y};
  if (is_resource_kind(kind)) {
    const ResourceBagSetup &bag = setup.resource_bags[static_cast<size_t>(resource_kind_index(kind))];
    if (!bag.present) return false;
    slots = bag.columns * bag.rows;
    payload.dimension = repl::Vec2d{static_cast<double>(bag.columns), static_cast<double>(bag.rows)};
    for (const std::vector<int> &column : bag.override_matrix) {
      payload.override_matrix.emplace_back(column.begin(), column.end());
    }
  }
  payload.maximum_items = slots;
  payload.storage.resize(static_cast<size_t>(slots));
  if (const auto storage = item_storage_of(kind)) {
    const uint8_t key_kind = static_cast<uint8_t>(*storage);
    for (auto it = items.lower_bound({key_kind, 0}); it != items.end() && it->first.first == key_kind; ++it) {
      repl::StorageSlot &slot = payload.storage[static_cast<size_t>(it->first.second)];
      slot.item = it->second;
      slot.slot = it->first.second;
    }
  }
  return true;
}

uint16_t find_elem_handle(std::span<const schema::RepCommand> elems, const std::string &full_path) {
  for (const schema::RepCommand &c : elems) {
    if (full_path == c.path) return c.handle;
  }
  return 0;
}

bool fill_sub_dialogue_ids(std::span<const schema::RepCommand> elems, const std::string &tail,
                           const std::vector<int64_t> &ids, repl::ElementState &e) {
  constexpr int64_t kObjectiveTypeId = 2174867120698124769LL;
  const std::string full = "QueuedSubDialogues." + tail;
  const schema::RepCommand *c = nullptr;
  for (const schema::RepCommand &x : elems) {
    if (full == x.path) c = &x;
  }
  if (!c || c->type != schema::PropType::DynamicArray || c->elem_end <= c->elem_begin) return false;
  repl::ArrayValue list;
  for (const int64_t id : ids) {
    repl::ElementState item;
    item.values[1] = Value::i64(id);
    item.values[2] = Value::i64(kObjectiveTypeId);
    item.values[3] = Value(repl::NameValue::hardcoded_name(0));
    list.elements.push_back(std::move(item));
  }
  e.values[c->handle] = Value(std::move(list));
  return true;
}

bool build_sub_dialogue_element(const schema::ClassSchema &cls, const PlayerBootstrap::DialogueView::SubDialogue &sd,
                                repl::ElementState &e) {
  const schema::RepCommand *array_cmd = cls.command("CurrentDialogueInstance.QueuedSubDialogues");
  if (!array_cmd) return false;
  const auto elems = cls.elements(*array_cmd);
  bool ok = true;
  const auto setv = [&](const std::string &tail, Value v) {
    const uint16_t h = find_elem_handle(elems, "QueuedSubDialogues." + tail);
    if (h == 0)
      ok = false;
    else
      e.values[h] = std::move(v);
  };
  const auto record_id = [&](const std::string &prefix, int64_t guid, int64_t type) {
    setv(prefix + ".Guid", Value::i64(guid));
    setv(prefix + ".TypeId", Value::i64(type));
    setv(prefix + ".Name", Value(repl::NameValue::hardcoded_name(0)));
  };
  constexpr int64_t kQuestTypeId = 6732126019658962568LL;
  setv("SubDialogueType", Value::u64(sd.type));
  record_id("PendingNextNode", sd.pending_next, repl::kDialogueNodeTypeId);
  record_id("PreviousNode", sd.previous_node, repl::kDialogueNodeTypeId);
  record_id("SubDialogueNodeId", sd.node, repl::kDialogueNodeTypeId);
  for (const char *w : {"A", "B", "C", "D"}) {
    setv(std::string("QuestPreview.InstanceGuid.") + w, Value::i64(0));
    setv(std::string("QuestGuid.") + w, Value::i64(0));
  }
  record_id("QuestPreview.QuestId", sd.quest, kQuestTypeId);
  record_id("QuestId", sd.quest, kQuestTypeId);
  setv("QuestPreview.BaseQuestRewards", Value(repl::RewardInstance{}));
  setv("QuestPreview.NodeId", Value::i64(0));
  setv("QuestPreview.bCompletedAlready", Value::boolean(false));
  ok = ok && fill_sub_dialogue_ids(elems, "QuestPreview.InitialObjectives", sd.initial_objectives, e);
  ok = ok && fill_sub_dialogue_ids(elems, "ObjectiveIds", sd.objectives, e);
  return ok;
}

}  // namespace

const FIntrepidNetworkGUID &PlayerBootstrap::storage_guid(StorageKind kind) const {
  const SessionGuids &g = setup_.guids;
  switch (kind) {
    case StorageKind::Backpack:
      return g.backpack;
    case StorageKind::Equipment:
      return g.equipment;
    case StorageKind::QuestStorage:
      return g.quest_storage;
    case StorageKind::Resource0:
      return g.resource_storage[0];
    case StorageKind::Resource1:
      return g.resource_storage[1];
    case StorageKind::Resource2:
      return g.resource_storage[2];
  }
  return g.backpack;
}

const FIntrepidNetworkGUID &PlayerBootstrap::item_storage_guid(ItemStorage storage) const {
  const SessionGuids &g = setup_.guids;
  switch (storage) {
    case ItemStorage::Backpack:
      return g.backpack;
    case ItemStorage::Equipment:
      return g.equipment;
    case ItemStorage::Quest:
      return g.quest_storage;
    case ItemStorage::Resource0:
      return g.resource_storage[0];
    case ItemStorage::Resource1:
      return g.resource_storage[1];
    case ItemStorage::Resource2:
      return g.resource_storage[2];
  }
  return g.backpack;
}

size_t PlayerBootstrap::item_count(ItemStorage storage) const {
  size_t n = 0;
  for (const auto &[key, item] : items_) n += key.first == static_cast<uint8_t>(storage) ? 1 : 0;
  return n;
}

const repl::ItemInstance *PlayerBootstrap::item_at(ItemStorage storage, int32_t slot) const {
  const auto it = items_.find({static_cast<uint8_t>(storage), slot});
  return it == items_.end() ? nullptr : &it->second;
}

namespace {

bool pack_storage_rpc_field(const schema::ClassSchema &cls, const schema::RpcSchema &rpc,
                            const repl::StoragePayload &payload, BunchWriter &field) {
  const auto params = repl::storage_payload_params(rpc, payload);
  if (!params) return false;
  BunchWriter bits;
  bool failed = false;
  if (repl::write_rpc_params(rpc, *params, bits, {}, &failed) == 0 || failed) return false;
  return repl::write_field(cls, rpc.field_index, bits, field) != 0;
}

std::optional<repl::RpcRequest> make_item_event_call(const schema::RpcSchema &rpc, const repl::GuidValue &character,
                                                     const FIntrepidNetworkGUID &container, const ItemEvent &e) {
  if (e.type != repl::StorageEventType::Add && e.type != repl::StorageEventType::Remove) return std::nullopt;
  if (!e.item.has_item || e.slot < 0) return std::nullopt;
  repl::ItemStorageEvent ev;
  ev.type = static_cast<uint8_t>(e.type);
  ev.item = e.item;
  if (e.instigated) ev.instigator = character;
  ev.callout = e.callout;
  ev.extra_guid_b = e.craft_txn;
  repl::StorageEventContainer target;
  target.object = container;
  target.slots.push_back({e.slot, e.count > 0 ? e.count : static_cast<int32_t>(e.item.quantity), e.item.guid});
  ev.targets.push_back(std::move(target));
  auto params = repl::storage_event_params(rpc, ev);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientReceiveStorageEvent";
  call.params = std::move(*params);
  return call;
}

repl::ArrayValue build_dialogue_choice_lines(const std::vector<PlayerBootstrap::DialogueLine> &view_lines) {
  repl::ArrayValue lines;
  for (const PlayerBootstrap::DialogueLine &l : view_lines) {
    repl::ElementState e;
    e.values[1] = Value::i64(l.node);
    e.values[2] = Value::i64(repl::kDialogueNodeTypeId);
    e.values[3] = Value(repl::NameValue::hardcoded_name(0));
    e.values[4] = Value::i64(l.index);
    e.values[5] = Value::u64(0);
    lines.elements.push_back(std::move(e));
  }
  return lines;
}

bool put_dialogue_prop(repl::Actor &pc, const repl::ActorShadow &pc_shadow, const char *tail, Value v,
                       bool is_default) {
  const schema::ClassSchema &cls = *pc.obj.schema;
  const std::string full = std::string("CurrentDialogueInstance.") + tail;
  const schema::RepCommand *cmd = cls.command(full);
  if (!cmd) return false;
  if (is_default && pc_shadow.actor.rep.sent.count(cmd->handle) == 0) {
    pc.obj.state.erase(cmd->handle);
    return true;
  }
  return pc.obj.state.set(cls, full, std::move(v));
}

bool write_dialogue_view_props(const PlayerBootstrap::DialogueView &view, const schema::ClassSchema &cls,
                               repl::Actor &pc, const repl::ActorShadow &pc_shadow) {
  repl::ArrayValue lines = build_dialogue_choice_lines(view.lines);
  const bool no_lines = lines.elements.empty();
  repl::ArrayValue queued;
  if (view.sub) {
    repl::ElementState e;
    if (!build_sub_dialogue_element(cls, *view.sub, e)) return false;
    queued.elements.push_back(std::move(e));
  }
  const auto put = [&](const char *tail, Value v, bool is_default) {
    return put_dialogue_prop(pc, pc_shadow, tail, std::move(v), is_default);
  };
  const bool good = put("PreviousNode.Guid", Value::i64(view.previous_node), view.previous_node == 0) &&
                    put("PreviousSelectedChoice.SourceNodeId.Guid", Value::i64(view.previous_choice_node),
                        view.previous_choice_node == 0) &&
                    put("PreviousSelectedChoice.ChoiceIndex", Value::i64(view.previous_choice_index),
                        view.previous_choice_index == 0) &&
                    put("CurrentNode.Guid", Value::i64(view.current_node), view.current_node == 0) &&
                    put("CurrentChoices", Value(std::move(lines)), no_lines) &&
                    put("SourceHandle.ObjectSource", Value(view.source), view.source == emit::FIntrepidNetworkGUID{}) &&
                    put("ResponseText.DialogueNodeId.Guid", Value::i64(view.shown_node), view.shown_node == 0) &&
                    put("bIsInGreeting", Value::boolean(view.greeting), !view.greeting) &&
                    put("QueuedSubDialogues", Value(std::move(queued)), !view.sub);
  return good && (!view.exhaust || put("ExhaustBehavior", Value::u64(0), false));
}

const char *attach_item_event_bookends(const repl::RpcRequest *first, const repl::RpcRequest *middle, size_t middle_at,
                                       const repl::RpcRequest *last, std::vector<repl::RpcRequest> &calls) {
  if (middle) {
    const size_t pos = std::min(middle_at, calls.size());
    calls.insert(calls.begin() + static_cast<std::ptrdiff_t>(pos), *middle);
  }
  if (first) calls.insert(calls.begin(), *first);
  if (last) calls.push_back(*last);
  if (first || middle) return "purchase answer";
  return last ? "sale answer" : "ClientReceiveStorageEvent";
}

}  // namespace

std::optional<Send> PlayerBootstrap::storage_payload(StorageKind kind, Role target) {
  if (!ok_ || (target != Role::Pawn && target != Role::PlayerController)) return std::nullopt;
  const schema::ClassSchema &cls = target == Role::Pawn ? *pawn_.obj.schema : *pc_.obj.schema;
  const schema::RpcSchema *rpc = cls.rpc("ClientReceiveStoragePayload");
  if (!rpc) return std::nullopt;
  repl::StoragePayload payload;
  if (!fill_storage_payload(kind, setup_, items_, storage_guid(kind), payload)) return std::nullopt;
  BunchWriter field;
  if (!pack_storage_rpc_field(cls, *rpc, payload, field)) return std::nullopt;

  Emission e;
  const StorageInfo &info = storage_info(kind);
  std::vector<ExportChain> chains;
  if (!is_resource_kind(kind)) {
    chains.push_back(
        repl::component_export(setup_.guids.pawn, storage_guid(kind), info.component_name, kChecksumItemStorage));
  }
  repl::PackedExports packed = repl::pack_exports(chains, acks_);
  if (!packed.ok) return std::nullopt;
  e.exports = std::move(packed.bunches);
  e.exported = std::move(packed.exported);
  e.exported_by_bunch = std::move(packed.exported_by_bunch);
  repl::BlockHeader header;
  if (repl::write_content_block(e.payload, header, field) == 0) return std::nullopt;
  return finish(target, false, "ClientReceiveStoragePayload", std::move(e));
}

std::optional<Send> PlayerBootstrap::build_item_events(std::span<const ItemEvent> events, const repl::RpcRequest *first,
                                                       const repl::RpcRequest *last, const repl::RpcRequest *middle,
                                                       size_t middle_at) {
  if (!ok_ || events.empty() || events.size() > kMaxEventsPerChain) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientReceiveStorageEvent");
  if (!rpc) return std::nullopt;
  const repl::GuidValue character{setup_.character_guid[0], setup_.character_guid[1], setup_.character_guid[2],
                                  setup_.character_guid[3]};
  std::vector<repl::RpcRequest> calls;
  calls.reserve(events.size() + 2);
  for (const ItemEvent &e : events) {
    const FIntrepidNetworkGUID container = e.container.value_or(item_storage_guid(e.storage));
    auto call = make_item_event_call(*rpc, character, container, e);
    if (!call) return std::nullopt;
    calls.push_back(std::move(*call));
  }
  const char *label = attach_item_event_bookends(first, middle, middle_at, last, calls);
  return rpcs(Role::PlayerController, label, calls);
}

std::optional<Send> PlayerBootstrap::loot_payload(const emit::FIntrepidNetworkGUID &loot_actor,
                                                  const repl::StoragePayload &data) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientReceiveLootStoragePayload");
  if (!rpc) return std::nullopt;
  repl::LootPayload p;
  p.actor = loot_actor;
  p.data = data;
  auto params = repl::loot_payload_params(*rpc, p);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientReceiveLootStoragePayload";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientReceiveLootStoragePayload", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::loot_removed(const emit::FIntrepidNetworkGUID &container,
                                                  std::span<const LootRemoval> removed) {
  if (!ok_ || !second_state_added_ || removed.empty() || removed.size() > kMaxEventsPerChain) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientReceiveStorageEvent");
  if (!rpc) return std::nullopt;
  const repl::GuidValue character{setup_.character_guid[0], setup_.character_guid[1], setup_.character_guid[2],
                                  setup_.character_guid[3]};
  std::vector<repl::RpcRequest> calls;
  for (const LootRemoval &r : removed) {
    if (!r.item.has_item || r.slot < 0) return std::nullopt;
    repl::ItemStorageEvent ev;
    ev.type = static_cast<uint8_t>(repl::StorageEventType::Remove);
    ev.item = r.item;
    ev.instigator = character;
    ev.move_source.status = repl::kStorageNoError;
    repl::StorageEventContainer target;
    target.object = container;
    target.slots.push_back({r.slot, static_cast<int32_t>(r.item.quantity), r.item.guid});
    ev.targets.push_back(std::move(target));
    auto params = repl::storage_event_params(*rpc, ev);
    if (!params) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveStorageEvent";
    call.params = std::move(*params);
    calls.push_back(std::move(call));
  }
  return rpcs(Role::PlayerController, "ClientReceiveStorageEvent", calls);
}

bool PlayerBootstrap::set_dead(bool dead) {
  if (!ok_) return false;
  repl::Subobject *combat = component(setup_.guids.combat_info);
  if (!combat || !combat->obj.schema->command("bIsDead")) return false;
  return combat->obj.state.set(*combat->obj.schema, "bIsDead", Value::boolean(dead));
}

std::optional<Send> PlayerBootstrap::death_location(double x, double y, double z) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pawn_.obj.schema->rpc("ClientSetLastDeathLocation");
  if (!rpc) return std::nullopt;
  repl::RpcParams params;
  if (!repl::set_rpc_param(*rpc, params, "Location", Value(repl::Vec3d{x, y, z}))) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientSetLastDeathLocation";
  call.params = std::move(params);
  return rpcs(Role::Pawn, "ClientSetLastDeathLocation", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::death_restart() {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::ClassSchema &cls = *pc_.obj.schema;
  const schema::RpcSchema *restart = cls.rpc("ClientRestart");
  const schema::RpcSchema *camera = cls.rpc("ClientSetCameraMode");
  if (!restart || !camera) return std::nullopt;
  std::vector<repl::RpcRequest> calls(4);
  for (size_t i = 0; i < 4; i += 2) {
    calls[i].rpc = "ClientRestart";
    calls[i + 1].rpc = "ClientSetCameraMode";
    if (!repl::set_rpc_param(*restart, calls[i].params, "NewPawn", object(setup_.guids.pawn)) ||
        !repl::set_rpc_param(*camera, calls[i + 1].params, "NewCamMode",
                             Value(repl::NameValue::hardcoded_name(kCameraModeEName)))) {
      return std::nullopt;
    }
  }
  return rpcs(Role::PlayerController, "ClientRestart and camera mode (death)", calls);
}

std::optional<Send> PlayerBootstrap::respawn_choices(std::span<const int64_t> closest, int64_t home,
                                                     bool previous_choice_invalid, bool home_on_cooldown) {
  if (!ok_ || !second_state_added_ || closest.size() > 64) return std::nullopt;
  const schema::RpcSchema *rpc = pawn_.obj.schema->rpc("SendClientRespawnChoices");
  if (!rpc) return std::nullopt;
  repl::RpcParams params;
  repl::ArrayValue list;
  for (const int64_t id : closest) {
    repl::ElementState e;
    e.values[1] = Value::i64(id);
    list.elements.push_back(std::move(e));
  }
  if (!repl::set_rpc_param(*rpc, params, "RespawnChoices.ClosestLegalRespawns", Value(std::move(list))) ||
      !repl::set_rpc_param(*rpc, params, "RespawnChoices.HomeRespawn", Value::i64(home)) ||
      !repl::set_rpc_param(*rpc, params, "RespawnChoices.Selection", Value::i64(0)) ||
      !repl::set_rpc_param(*rpc, params, "RespawnChoices.bPreviousChoiceInvalid",
                           Value::boolean(previous_choice_invalid)) ||
      !repl::set_rpc_param(*rpc, params, "RespawnChoices.bHomeAttuneOnCooldown", Value::boolean(home_on_cooldown))) {
    return std::nullopt;
  }
  repl::RpcRequest call;
  call.rpc = "SendClientRespawnChoices";
  call.params = std::move(params);
  return rpcs(Role::Pawn, "SendClientRespawnChoices", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::loot_listener_removed(const emit::FIntrepidNetworkGUID &loot_actor) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientRemoveListenerFromStorage");
  if (!rpc) return std::nullopt;
  auto params = repl::remove_listener_params(*rpc, loot_actor);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientRemoveListenerFromStorage";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientRemoveListenerFromStorage", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::open_vendor(int64_t merch_set, const emit::FIntrepidNetworkGUID &vendor,
                                                 const repl::UnavailableList &unavailable) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientOpenVendor");
  if (!rpc) return std::nullopt;
  auto params = repl::open_vendor_params(*rpc, merch_set, vendor, unavailable);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientOpenVendor";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientOpenVendor", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::open_station(const emit::FIntrepidNetworkGUID &station,
                                                  const repl::StationTax &tax,
                                                  const std::vector<repl::CraftItemState> &jobs) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientOpenCraftingStationUI");
  if (!rpc) return std::nullopt;
  auto params = repl::open_station_params(*rpc, station, tax, jobs);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientOpenCraftingStationUI";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientOpenCraftingStationUI", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::craft_interrupted() {
  if (!ok_ || !second_state_added_) return std::nullopt;
  if (!pc_.obj.schema->rpc("ClientInterruptCraft")) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientInterruptCraft";
  return rpcs(Role::PlayerController, "ClientInterruptCraft", std::span<const repl::RpcRequest>(&call, 1));
}

bool PlayerBootstrap::set_known_recipes(const std::vector<int64_t> &recipes) {
  if (!ok_ || recipes.size() > kMaxKnownRecipes) return false;
  repl::Subobject *owner = component(setup_.guids.owner_info);
  if (!owner) return false;
  repl::ArrayValue list;
  list.elements.reserve(recipes.size());
  for (const int64_t id : recipes) {
    repl::ElementState e;
    e.values[1] = Value::i64(id);
    list.elements.push_back(std::move(e));
  }
  return owner->obj.state.set(*owner->obj.schema, "PlayerKnownRecipeIds", Value(std::move(list)));
}

bool PlayerBootstrap::set_active_stations(const std::vector<std::array<int32_t, 4>> &stations) {
  if (!ok_) return false;
  repl::Subobject *owner = component(setup_.guids.owner_info);
  if (!owner) return false;
  repl::ArrayValue list;
  for (const std::array<int32_t, 4> &g : stations) {
    repl::ElementState e;
    for (size_t i = 0; i < 4; ++i) e.values[static_cast<uint16_t>(i + 1)] = Value::i64(g[i]);
    list.elements.push_back(std::move(e));
  }
  return owner->obj.state.set(*owner->obj.schema, "ActiveProcessingStations", Value(std::move(list)));
}

std::optional<Send> PlayerBootstrap::dialogue_update(const DialogueView &view) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  if (!write_dialogue_view_props(view, *pc_.obj.schema, pc_, pc_shadow_)) return std::nullopt;
  return update(Role::PlayerController, "dialogue", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::node_tax_table(const repl::NodeTaxDetails &details) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientSetNodeTaxTable");
  if (!rpc) return std::nullopt;
  auto params = repl::node_tax_table_params(*rpc, details);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientSetNodeTaxTable";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientSetNodeTaxTable", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::node_maps_update(const repl::NodeStateMaps &maps) {
  if (!ok_ || mode_ != GameStateMode::Tier3) return std::nullopt;
  size_t changed = 0;
  repl::FastArrays before = gs_.obj.arrays;
  if (!repl::sync_node_maps(*gs_.obj.schema, gs_.obj.arrays, maps, &changed)) {
    gs_.obj.arrays = before;
    fail("the node maps do not fit the GameState's fast arrays");
    return std::nullopt;
  }
  if (changed == 0) return std::nullopt;
  auto send = update(Role::GameState, "GameState node maps", gs_, gs_shadow_);
  if (!send)
    gs_.obj.arrays = before;
  else
    node_maps_before_ = std::move(before);
  return send;
}

std::optional<Send> PlayerBootstrap::node_summary(const repl::NodeSummary &summary) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientReceiveNodeSummary");
  if (!rpc) return std::nullopt;
  auto params = repl::node_summary_params(*rpc, summary);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientReceiveNodeSummary";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientReceiveNodeSummary", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::siege_summary(const repl::SiegeSummary &summary) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientReceiveSiegeSummary");
  if (!rpc) return std::nullopt;
  auto params = repl::siege_summary_params(*rpc, summary);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientReceiveSiegeSummary";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientReceiveSiegeSummary", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::siege_cooldowns() {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientReceiveAllCurrentNodeSiegeCooldowns");
  if (!rpc) return std::nullopt;
  auto params = repl::empty_siege_cooldowns_params(*rpc);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientReceiveAllCurrentNodeSiegeCooldowns";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientReceiveAllCurrentNodeSiegeCooldowns",
              std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::item_events(std::span<const ItemEvent> events) {
  auto send = build_item_events(events);
  if (!send) return std::nullopt;
  remember(events);
  return send;
}

void PlayerBootstrap::remember(std::span<const ItemEvent> events) {
  for (const ItemEvent &e : events) {
    if (e.container) continue;
    const std::pair<uint8_t, int32_t> key{static_cast<uint8_t>(e.storage), e.slot};
    if (e.type == repl::StorageEventType::Add) {
      items_[key] = e.item;
    } else if (e.count > 0 && e.count < static_cast<int32_t>(e.item.quantity)) {
      repl::ItemInstance rest = e.item;
      rest.quantity = e.item.quantity - static_cast<decltype(rest.quantity)>(e.count);
      items_[key] = std::move(rest);
    } else {
      items_.erase(key);
    }
  }
}

}  // namespace aoc::worldd::bootstrap
