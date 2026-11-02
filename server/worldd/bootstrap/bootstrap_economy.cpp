// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#include <algorithm>
#include <bit>
#include <set>
#include <string>
#include <vector>

#include "bootstrap/bootstrap.h"
#include "bootstrap/bootstrap_internal.h"

namespace aoc::worldd::bootstrap {

using namespace ::aoc::protocol;
using namespace ::aoc::protocol::emit;
using namespace ::aoc::protocol::schema;
using repl::Emission;
using repl::Value;

namespace {

std::optional<repl::RpcRequest> currency_callout_call(const schema::ClassSchema &pc, int64_t tier, int64_t delta) {
  const schema::RpcSchema *rpc = pc.rpc("ClientShowCurrencyCallout");
  if (!rpc) return std::nullopt;
  repl::RpcParams params;
  if (!repl::set_rpc_param(*rpc, params, "CurrencyTierDefGuid", Value::i64(tier)) ||
      !repl::set_rpc_param(*rpc, params, "Delta", Value::i64(delta))) {
    return std::nullopt;
  }
  repl::RpcRequest call;
  call.rpc = "ClientShowCurrencyCallout";
  call.params = std::move(params);
  return call;
}

repl::ElementState held_item_state(const HeldItem &h) {
  repl::ElementState e;
  e.values[1] = Value::u64(h.slot);
  e.values[2] = Value::i64(h.item_guid);
  e.values[3] = Value::u64(h.enchantment);
  return e;
}

bool validate_held_items(const repl::FastArrayState &array, std::span<const HeldItem> held, std::string &err) {
  if (array.items().empty()) {
    std::set<int32_t> ids;
    for (const HeldItem &h : held) {
      if (h.id <= 0 || !ids.insert(h.id).second) {
        err = "HeldItems: an id that is not positive and unique";
        return false;
      }
    }
    return true;
  }
  for (const HeldItem &h : held) {
    if (!array.find(h.id)) {
      err = "HeldItems: the item with the id " + std::to_string(h.id) + " is not in the array";
      return false;
    }
  }
  return true;
}

bool populate_warehouse_tab_props(const WarehouseComponent &c, const schema::ClassSchema &sc, repl::RepState &st) {
  using Kind = WarehouseComponent::Kind;
  bool ok = st.set(sc, "WarehouseId.Area", Value::u64(repl::kWarehouseAreaNode)) &&
            st.set(sc, "WarehouseId.AreaRecordGuid", Value::i64(c.node)) &&
            st.set(sc, "ResourceStorageComponentIndex", Value::i64(c.index)) &&
            st.set(sc, "WarehouseStorageMaterialTabRecordGuid", Value::i64(c.record)) &&
            st.set(sc, "bReadOnly", Value::boolean(c.read_only));
  if (c.kind == Kind::CargoTab) ok = ok && st.set(sc, "StorageType", Value::u64(repl::kStorageCrates));
  if (c.member) ok = ok && st.set(sc, "bIsMemberStorage", Value::boolean(true));
  return ok;
}

bool make_warehouse_component(const WarehouseComponent &c, const std::array<uint32_t, 4> &character,
                              repl::Subobject &out) {
  using Kind = WarehouseComponent::Kind;
  out = repl::Subobject{};
  out.guid = c.guid;
  const bool chunk = c.kind == Kind::ItemChunk;
  out.dynamic_class = chunk ? warehouse_chunk_class_export() : warehouse_tab_class_export();
  out.obj.schema = gen::find_class_schema(chunk ? "UNodeItemStorageComponent" : "UWarehouseResourceStorageComponent");
  if (!out.obj.schema) return false;
  repl::RepState &st = out.obj.state;
  const schema::ClassSchema &sc = *out.obj.schema;
  bool ok = st.set(sc, "ItemLocationType", Value::u64(chunk ? 15 : c.kind == Kind::CargoTab ? 17 : 14));
  static constexpr const char *kWords[4] = {"A", "B", "C", "D"};
  for (size_t i = 0; i < 4; ++i)
    ok = ok && st.set(sc, std::string("PlayerGuid.") + kWords[i], Value::i64(static_cast<int32_t>(character[i])));
  if (chunk) return ok && st.set(sc, "WarehouseStorageItemChunkRecordId", Value::i64(c.record));
  return ok && populate_warehouse_tab_props(c, sc, st);
}

std::optional<repl::ItemStorageEvent> make_batched_storage_event(const ItemEvent &e, const repl::GuidValue &character,
                                                                 const FIntrepidNetworkGUID &container) {
  if ((e.type != repl::StorageEventType::Add && e.type != repl::StorageEventType::Remove) || !e.item.has_item ||
      e.slot < 0)
    return std::nullopt;
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
  return ev;
}

bool update_existing_currency_entry(repl::FastArrayState &wallet, int64_t currency, uint64_t amount,
                                    repl::ElementState &values, bool &updated) {
  updated = false;
  for (const repl::FastArrayItem &it : wallet.items()) {
    const auto key = it.values.values.find(2);
    if (key == it.values.values.end() || !(key->second == Value::i64(currency))) continue;
    updated = true;
    const auto now = it.values.values.find(1);
    if (now != it.values.values.end() && now->second == Value::u64(amount)) return true;
    return wallet.change(it.id, std::move(values));
  }
  return true;
}

bool insert_initial_held_items(repl::FastArrayState &array, std::span<const HeldItem> held, std::string &err) {
  for (const HeldItem &h : held) {
    if (!array.insert(h.id, held_item_state(h))) {
      err = "HeldItems: an item does not fit";
      return false;
    }
  }
  array.mark_array_dirty();
  return true;
}

bool sync_held_item_mutations(repl::FastArrayState &array, std::span<const HeldItem> held, size_t marks,
                              std::string &err) {
  if (array.items().empty()) return insert_initial_held_items(array, held, err);
  for (size_t m = 0; m < marks; ++m) {
    for (const HeldItem &h : held) {
      if (!array.change(h.id, held_item_state(h))) {
        err = "HeldItems: an item does not take its change";
        return false;
      }
    }
  }
  return true;
}

repl::Subobject *find_pawn_subobject(repl::Actor &pawn, const emit::FIntrepidNetworkGUID &guid) {
  for (repl::Subobject &sub : pawn.subobjects) {
    if (sub.guid == guid) return &sub;
  }
  return nullptr;
}

const char *ensure_resource_subobject(size_t i, const emit::FIntrepidNetworkGUID &guid, PlayerSetup &setup,
                                      repl::Actor &pawn) {
  if (guid.ObjectId == 0) return "a resource container needs a guid";
  if (!(setup.guids.resource_storage[i] == guid)) {
    const emit::FIntrepidNetworkGUID old = setup.guids.resource_storage[i];
    auto &subs = pawn.subobjects;
    if (old.ObjectId != 0) {
      subs.erase(std::remove_if(subs.begin(), subs.end(), [&](const repl::Subobject &s) { return s.guid == old; }),
                 subs.end());
    }
    setup.guids.resource_storage[i] = guid;
  }
  if (find_pawn_subobject(pawn, guid)) return nullptr;
  repl::Subobject resource;
  if (!make_resource_component(guid, i, resource)) return "no schema for UResourceStorageComponent";
  pawn.subobjects.push_back(std::move(resource));
  return nullptr;
}

const char *sync_warehouse_subobject(const WarehouseComponent &c, const std::array<uint32_t, 4> &character_guid,
                                     repl::Actor &pawn) {
  if (c.guid.ObjectId == 0) return "a warehouse component needs a guid";
  if (repl::Subobject *have = find_pawn_subobject(pawn, c.guid)) {
    if (c.kind != WarehouseComponent::Kind::ItemChunk &&
        !have->obj.state.set(*have->obj.schema, "bReadOnly", Value::boolean(c.read_only))) {
      return "bReadOnly of a warehouse tab refused";
    }
    return nullptr;
  }
  repl::Subobject made;
  if (!make_warehouse_component(c, character_guid, made)) {
    return "no schema for a warehouse component, or a value of it is refused";
  }
  pawn.subobjects.push_back(std::move(made));
  return nullptr;
}

bool rebuild_resource_container_list(const PlayerSetup &setup, repl::Subobject &owner) {
  repl::ArrayValue containers;
  containers.elements.resize(3);
  for (size_t j = 0; j < setup.resource_bags.size(); ++j) {
    if (setup.resource_bags[j].present) containers.elements[j].values[1] = object(setup.guids.resource_storage[j]);
  }
  return owner.obj.state.set(*owner.obj.schema, "ResourceContainers", Value(std::move(containers)));
}

bool rebuild_warehouse_guid_lists(const std::vector<WarehouseComponent> &components, repl::Subobject &owner) {
  using Kind = WarehouseComponent::Kind;
  const auto list_of = [&](Kind kind) {
    repl::ArrayValue list;
    for (const WarehouseComponent &c : components) {
      if (c.kind != kind || !c.listed) continue;
      repl::ElementState e;
      e.values[1] = object(c.guid);
      list.elements.push_back(std::move(e));
    }
    return Value(std::move(list));
  };
  return owner.obj.state.set(*owner.obj.schema, "NodeItemContainers", list_of(Kind::ItemChunk)) &&
         owner.obj.state.set(*owner.obj.schema, "NodeResourceWarehouse.ActiveComponents", list_of(Kind::MaterialTab)) &&
         owner.obj.state.set(*owner.obj.schema, "NodeCrateWarehouse.ActiveComponents", list_of(Kind::CargoTab));
}

struct CalloutSpec {
  const std::string &message;
  int32_t type;
  int32_t point_size;
  int64_t creation_ticks;
  uint64_t key;
  const std::string &table;
  const std::string &message_key;
  uint64_t chat_category;
};

repl::ElementState callout_state(const CalloutSpec &s) {
  repl::ElementState es;
  auto &v = es.values;
  for (uint16_t i : {1, 5, 6, 12}) v[i] = Value::str("");
  v[2] = Value::str(s.message);
  v[3] = Value(repl::TextValue{});
  v[4] = Value(repl::LocalizedMessage{s.table, s.message_key});
  v[7] = Value::i64(s.point_size);
  v[8] = Value::u64(static_cast<uint64_t>(s.type));
  v[9] = Value::u64(s.chat_category);
  v[10] = Value::u64(0);
  v[11] = Value::boolean(false);
  v[13] = Value(repl::ItemInstance{});
  for (uint16_t i : {14, 15, 16, 17, 18, 19, 22, 23, 24}) v[i] = Value::i64(0);
  v[20] = Value(repl::Vec3d{0, 0, 0});
  v[21] = Value(repl::Ticks{s.creation_ticks});
  v[25] = Value::i64(static_cast<int32_t>(s.key & 0xFFFFFFFFu));
  v[26] = Value::i64(static_cast<int32_t>((s.key >> 32) & 0xFFFFFFFFu));
  v[27] = Value::i64(static_cast<int32_t>(((s.key * 0x9E3779B97F4A7C15ull) >> 32) & 0xFFFFFFFFu));
  v[28] = Value::i64(static_cast<int32_t>(~s.key & 0xFFFFFFFFu));
  return es;
}

}  // namespace

std::optional<Send> PlayerBootstrap::purchase_answer(int64_t currency_tier, int64_t delta,
                                                     std::span<const ItemEvent> events) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const auto callout = currency_callout_call(*pc_.obj.schema, currency_tier, delta);
  if (!callout) return std::nullopt;
  auto send = build_item_events(events, &*callout);
  if (!send) return std::nullopt;
  remember(events);
  return send;
}

std::optional<Send> PlayerBootstrap::sale_answer(int64_t currency_tier, int64_t delta,
                                                 std::span<const ItemEvent> events) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const auto callout = currency_callout_call(*pc_.obj.schema, currency_tier, delta);
  if (!callout) return std::nullopt;
  auto send = build_item_events(events, nullptr, &*callout);
  if (!send) return std::nullopt;
  remember(events);
  return send;
}

std::optional<Send> PlayerBootstrap::item_payment_answer(int64_t currency_tier, int64_t delta,
                                                         std::span<const ItemEvent> paid,
                                                         std::span<const ItemEvent> bought) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const auto callout = currency_callout_call(*pc_.obj.schema, currency_tier, delta);
  if (!callout) return std::nullopt;
  std::vector<ItemEvent> all(paid.begin(), paid.end());
  all.insert(all.end(), bought.begin(), bought.end());
  auto send = build_item_events(all, nullptr, nullptr, &*callout, paid.size());
  if (!send) return std::nullopt;
  remember(all);
  return send;
}

std::optional<Send> PlayerBootstrap::repair_answer(ItemStorage storage, int32_t slot, float durability,
                                                   int64_t currency_tier, int64_t delta) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const std::pair<uint8_t, int32_t> key{static_cast<uint8_t>(storage), slot};
  const auto it = items_.find(key);
  if (it == items_.end() || !it->second.has_item) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientUpdateItemDurability");
  if (!rpc) return std::nullopt;
  const auto callout = currency_callout_call(*pc_.obj.schema, currency_tier, delta);
  if (!callout) return std::nullopt;
  repl::ItemDurabilityUpdate update;
  update.container = item_storage_guid(storage);
  update.items.push_back({it->second.guid, slot, durability});
  auto params = repl::item_durability_params(*rpc, update);
  if (!params) return std::nullopt;
  std::vector<repl::RpcRequest> calls(2);
  calls[0].rpc = "ClientUpdateItemDurability";
  calls[0].params = std::move(*params);
  calls[1] = *callout;
  auto send = rpcs(Role::PlayerController, "repair answer", calls);
  if (!send) return std::nullopt;
  it->second.durability = durability;
  return send;
}

std::optional<Send> PlayerBootstrap::currency_callout(int64_t currency_tier, int64_t delta) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const auto callout = currency_callout_call(*pc_.obj.schema, currency_tier, delta);
  if (!callout) return std::nullopt;
  return rpcs(Role::PlayerController, "ClientShowCurrencyCallout", std::span<const repl::RpcRequest>(&*callout, 1));
}

std::optional<Send> PlayerBootstrap::craft_started_answer(const emit::FIntrepidNetworkGUID &station,
                                                          int64_t currency_tier, int64_t delta,
                                                          const repl::CraftItemState &state) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  auto callout = currency_callout_call(*pc_.obj.schema, currency_tier, delta);
  const schema::RpcSchema *update = pc_.obj.schema->rpc("ClientUpdateCraftingStationUI");
  if (!callout || !update) return std::nullopt;
  auto params = repl::update_station_params(*update, station, {state});
  if (!params) return std::nullopt;
  std::vector<repl::RpcRequest> calls(2);
  calls[0] = std::move(*callout);
  calls[1].rpc = "ClientUpdateCraftingStationUI";
  calls[1].params = std::move(*params);
  return rpcs(Role::PlayerController, "craft started", calls);
}

std::optional<Send> PlayerBootstrap::craft_states_update(const emit::FIntrepidNetworkGUID &station,
                                                         const std::vector<repl::CraftItemState> &states) {
  if (!ok_ || !second_state_added_ || states.empty() || states.size() > kMaxEventsPerChain) return std::nullopt;
  const schema::RpcSchema *update = pc_.obj.schema->rpc("ClientUpdateCraftingStationUI");
  if (!update) return std::nullopt;
  std::vector<repl::RpcRequest> calls(states.size());
  for (size_t i = 0; i < states.size(); ++i) {
    auto params = repl::update_station_params(*update, station, {states[i]});
    if (!params) return std::nullopt;
    calls[i].rpc = "ClientUpdateCraftingStationUI";
    calls[i].params = std::move(*params);
  }
  return rpcs(Role::PlayerController, "craft update", calls);
}

std::optional<repl::RpcRequest> PlayerBootstrap::batched_events_call(std::span<const ItemEvent> events) {
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientRecieveBatchedStorageEvents");
  if (!rpc || events.empty() || events.size() > 256) return std::nullopt;
  const repl::GuidValue character{setup_.character_guid[0], setup_.character_guid[1], setup_.character_guid[2],
                                  setup_.character_guid[3]};
  std::vector<repl::ItemStorageEvent> list;
  list.reserve(events.size());
  for (const ItemEvent &e : events) {
    auto ev = make_batched_storage_event(e, character, item_storage_guid(e.storage));
    if (!ev) return std::nullopt;
    list.push_back(std::move(*ev));
  }
  auto params = repl::batched_events_params(*rpc, list);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientRecieveBatchedStorageEvents";
  call.params = std::move(*params);
  return call;
}

std::optional<Send> PlayerBootstrap::craft_claimed_answer(const emit::FIntrepidNetworkGUID &station,
                                                          const repl::CraftItemState &claimed,
                                                          std::span<const ItemEvent> events) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *update = pc_.obj.schema->rpc("ClientUpdateCraftingStationUI");
  if (!update) return std::nullopt;
  auto params = repl::update_station_params(*update, station, {claimed});
  if (!params) return std::nullopt;
  std::vector<repl::RpcRequest> calls(1);
  calls[0].rpc = "ClientUpdateCraftingStationUI";
  calls[0].params = std::move(*params);
  if (!events.empty()) {
    auto batch = batched_events_call(events);
    if (!batch) return std::nullopt;
    calls.push_back(std::move(*batch));
  }
  auto send = rpcs(Role::PlayerController, "craft claimed", calls);
  if (!send) return std::nullopt;
  remember(events);
  return send;
}

std::optional<Send> PlayerBootstrap::craft_batched_events(std::span<const ItemEvent> events) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  auto batch = batched_events_call(events);
  if (!batch) return std::nullopt;
  auto send = rpcs(Role::PlayerController, "craft items", std::span<const repl::RpcRequest>(&*batch, 1));
  if (!send) return std::nullopt;
  remember(events);
  return send;
}

bool PlayerBootstrap::set_currency(int64_t currency, int64_t amount) {
  if (!ok_ || amount < 0) return false;
  repl::Subobject *owner = component(setup_.guids.owner_info);
  if (!owner) return false;
  repl::FastArrayState *wallet = owner->obj.arrays.get(*owner->obj.schema, "Currencies");
  if (!wallet) return false;
  repl::ElementState values;
  const uint64_t uamount = static_cast<uint64_t>(amount);
  values.values[1] = Value::u64(uamount);
  values.values[2] = Value::i64(currency);
  bool updated = false;
  if (!update_existing_currency_entry(*wallet, currency, uamount, values, updated)) return false;
  if (updated) return true;
  return wallet->add(std::move(values)).has_value();
}

std::optional<Send> PlayerBootstrap::currencies_update() {
  if (!ok_ || !second_state_sent_) return std::nullopt;
  return update(Role::Pawn, "Currencies", pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::item_durability(ItemStorage storage, int32_t slot, float durability) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const std::pair<uint8_t, int32_t> key{static_cast<uint8_t>(storage), slot};
  const auto it = items_.find(key);
  if (it == items_.end() || !it->second.has_item) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientUpdateItemDurability");
  if (!rpc) return std::nullopt;
  repl::ItemDurabilityUpdate update;
  update.container = item_storage_guid(storage);
  update.items.push_back({it->second.guid, slot, durability});
  auto params = repl::item_durability_params(*rpc, update);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientUpdateItemDurability";
  call.params = std::move(*params);
  auto send = rpcs(Role::PlayerController, "ClientUpdateItemDurability", std::span<const repl::RpcRequest>(&call, 1));
  if (!send) return std::nullopt;
  it->second.durability = durability;
  return send;
}

std::optional<std::vector<Send>> PlayerBootstrap::storage_adds(ItemStorage storage) {
  if (!ok_) return std::nullopt;
  std::vector<ItemEvent> adds;
  for (auto it = items_.lower_bound({static_cast<uint8_t>(storage), 0});
       it != items_.end() && it->first.first == static_cast<uint8_t>(storage); ++it) {
    ItemEvent e;
    e.type = repl::StorageEventType::Add;
    e.storage = storage;
    e.slot = it->first.second;
    e.item = it->second;
    adds.push_back(std::move(e));
  }
  std::vector<Send> out;
  for (size_t at = 0; at < adds.size(); at += kMaxEventsPerChain) {
    const size_t n = std::min(kMaxEventsPerChain, adds.size() - at);
    auto send = build_item_events(std::span<const ItemEvent>(adds.data() + at, n));
    if (!send) return std::nullopt;
    out.push_back(std::move(*send));
  }
  return out;
}

bool PlayerBootstrap::apply_equipment(std::span<const HeldItem> held, std::span<const repl::AppearanceId> appearance,
                                      size_t marks) {
  if (!ok_ || !second_state_added_ || marks == 0 || marks > 64) return false;
  repl::Subobject *base = component(setup_.guids.base_character_info);
  repl::Subobject *app = component(setup_.guids.appearance);
  if (!base || !app) return false;
  repl::FastArrayState *array = base->obj.arrays.get(*base->obj.schema, "HeldItems");
  if (!array) return fail("BaseCharacterInfo has no HeldItems");
  std::string err;
  if (!validate_held_items(*array, held, err)) return fail(std::move(err));
  repl::ArrayValue ids;
  for (const repl::AppearanceId &id : appearance) {
    repl::ElementState e;
    e.values[1] = Value(id);
    ids.elements.push_back(std::move(e));
  }
  if (!app->obj.state.set(*app->obj.schema, "AppearanceIDs", Value(std::move(ids))))
    return fail("Character Appearance.AppearanceIDs refused");
  if (!sync_held_item_mutations(*array, held, marks, err)) return fail(std::move(err));
  return true;
}

std::optional<Send> PlayerBootstrap::equipment_update() {
  if (!ok_ || !second_state_sent_) return std::nullopt;
  return update(Role::Pawn, "equipment", pawn_, pawn_shadow_);
}

bool PlayerBootstrap::set_resource_container(int index, const ResourceBagSetup &bag,
                                             const emit::FIntrepidNetworkGUID &guid) {
  if (!ok_ || !second_state_added_ || index < 0 || index >= static_cast<int>(setup_.resource_bags.size())) return false;
  const size_t i = static_cast<size_t>(index);
  if (bag.present) {
    if (const char *err = ensure_resource_subobject(i, guid, setup_, pawn_)) return fail(err);
  }
  setup_.resource_bags[i] = bag.present ? bag : ResourceBagSetup{};
  repl::Subobject *owner = component(setup_.guids.owner_info);
  if (!owner) return fail("OwnerInfo is not on the pawn");
  if (!rebuild_resource_container_list(setup_, *owner)) return fail("OwnerInfo.ResourceContainers refused");
  return true;
}

std::optional<Send> PlayerBootstrap::resource_containers_update() {
  if (!ok_ || !second_state_sent_) return std::nullopt;
  return update(Role::Pawn, "resource containers", pawn_, pawn_shadow_);
}

bool PlayerBootstrap::set_warehouse_components(const std::vector<WarehouseComponent> &components) {
  if (!ok_ || !second_state_added_) return false;
  for (const WarehouseComponent &c : components) {
    if (const char *err = sync_warehouse_subobject(c, setup_.character_guid, pawn_)) return fail(err);
  }
  repl::Subobject *owner = component(setup_.guids.owner_info);
  if (!owner) return fail("OwnerInfo is not on the pawn");
  if (!rebuild_warehouse_guid_lists(components, *owner)) return fail("OwnerInfo's warehouse lists refused");
  return true;
}

bool PlayerBootstrap::set_warehouse_nodes(const std::vector<int64_t> &material_nodes,
                                          const std::vector<int64_t> &crate_nodes) {
  if (!ok_ || !second_state_added_) return false;
  repl::Subobject *owner = component(setup_.guids.owner_info);
  if (!owner) return fail("OwnerInfo is not on the pawn");
  const auto ids_of = [](const std::vector<int64_t> &nodes) {
    repl::ArrayValue list;
    for (const int64_t node : nodes) {
      repl::ElementState e;
      e.values[1] = Value::u64(repl::kWarehouseAreaNode);
      e.values[2] = Value::i64(node);
      list.elements.push_back(std::move(e));
    }
    return Value(std::move(list));
  };
  if (!owner->obj.state.set(*owner->obj.schema, "WarehouseMaterialTabsByIds", ids_of(material_nodes)) ||
      !owner->obj.state.set(*owner->obj.schema, "WarehouseCrateTabsByIds", ids_of(crate_nodes))) {
    return fail("OwnerInfo's warehouse node lists refused");
  }
  return true;
}

std::optional<Send> PlayerBootstrap::warehouse_update() {
  if (!ok_ || !second_state_sent_) return std::nullopt;
  return update(Role::Pawn, "warehouse", pawn_, pawn_shadow_);
}

std::optional<Send> PlayerBootstrap::open_warehouse_window(int64_t node, uint8_t access_config) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientOpenWarehouseWindow");
  if (!rpc) return std::nullopt;
  auto params =
      repl::open_warehouse_window_params(*rpc, repl::WarehouseId{repl::kWarehouseAreaNode, node}, access_config);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientOpenWarehouseWindow";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientOpenWarehouseWindow", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::warehouse_summary(const repl::WarehouseSummary &summary) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::RpcSchema *rpc = pc_.obj.schema->rpc("ClientReceiveWarehouseSummary");
  if (!rpc) return std::nullopt;
  auto params = repl::warehouse_summary_params(*rpc, summary);
  if (!params) return std::nullopt;
  repl::RpcRequest call;
  call.rpc = "ClientReceiveWarehouseSummary";
  call.params = std::move(*params);
  return rpcs(Role::PlayerController, "ClientReceiveWarehouseSummary", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::container_payload(const repl::StoragePayload &data) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const schema::ClassSchema &cls = *pawn_.obj.schema;
  const schema::RpcSchema *rpc = cls.rpc("ClientReceiveStoragePayload");
  if (!rpc) return std::nullopt;
  const auto params = repl::storage_payload_params(*rpc, data);
  if (!params) return std::nullopt;
  BunchWriter bits;
  bool failed = false;
  if (repl::write_rpc_params(*rpc, *params, bits, {}, &failed) == 0 || failed) return std::nullopt;
  BunchWriter field;
  if (repl::write_field(cls, rpc->field_index, bits, field) == 0) return std::nullopt;
  Emission e;
  repl::BlockHeader header;
  if (repl::write_content_block(e.payload, header, field) == 0) return std::nullopt;
  return finish(Role::Pawn, false, "ClientReceiveStoragePayload", std::move(e));
}

std::optional<Send> PlayerBootstrap::verbatim_block(Role role, const char *what, const char *hex, size_t bits) {
  Emission e;
  const auto bytes = captured::bytes_of(hex);
  e.payload.write_bit_range(bytes.data(), 0, bits);
  return finish(role, false, what, std::move(e));
}

std::optional<Send> PlayerBootstrap::empty_group() {
  if (!ok_) return std::nullopt;
  return verbatim_block(Role::PlayerController, "ClientUpdateGroup (empty)", captured::kEmptyGroupBlock.hex,
                        captured::kEmptyGroupBlock.bits);
}

std::optional<Send> PlayerBootstrap::default_action_bar() {
  if (!ok_) return std::nullopt;
  return verbatim_block(Role::PlayerController, "ClientReceiveActionbarSlotPayload", captured::kActionBarBlock.hex,
                        captured::kActionBarBlock.bits);
}

std::optional<Send> PlayerBootstrap::action_bar(const repl::ActionbarPayload &payload) {
  if (!ok_) return std::nullopt;
  return finish(Role::PlayerController, false, "ClientReceiveActionbarSlotPayload",
                repl::client_receive_actionbar_slot_payload(pc_, payload, acks_));
}

bool PlayerBootstrap::mount_bar_calls(int64_t dash_ability, int32_t slot_index,
                                      std::vector<repl::RpcRequest> &calls) const {
  const schema::RpcSchema *clear = pc_.obj.schema->rpc("ClientClearActionbarsForInstigators");
  const schema::RpcSchema *slot = pc_.obj.schema->rpc("ClientReceiveActionbarSlotPayload");
  if (!clear || !slot) return false;
  const size_t first = calls.size();
  calls.resize(first + 2);
  calls[first].rpc = "ClientClearActionbarsForInstigators";
  calls[first + 1].rpc = "ClientReceiveActionbarSlotPayload";
  repl::ArrayValue instigators;
  for (const uint8_t who : {repl::ActionbarInstigator::Vehicle, repl::ActionbarInstigator::Mount}) {
    repl::ElementState e;
    e.values[1] = Value::u64(who);
    instigators.elements.push_back(std::move(e));
  }
  if (!repl::set_rpc_param(*clear, calls[first].params, "ActionbarInstigators", Value(std::move(instigators))) ||
      !repl::set_rpc_param(*clear, calls[first].params, "ActionBar",
                           Value::u64(static_cast<uint64_t>(repl::ActionbarBank::Vehicle)))) {
    return false;
  }
  repl::ActionbarPayload payload;
  repl::ActionbarSlot dash;
  dash.bank_index = repl::ActionbarBank::Vehicle;
  dash.slot_index = slot_index;
  dash.use.use_type = repl::ActionbarUseType::Ability;
  dash.use.instigator = repl::ActionbarInstigator::Mount;
  dash.use.ability_id = dash_ability;
  payload.slots.push_back(dash);
  auto params = repl::actionbar_payload_params(*slot, payload);
  if (!params) return false;
  calls[first + 1].params = std::move(*params);
  return true;
}

std::optional<Send> PlayerBootstrap::mount_bar(int64_t dash_ability, int32_t slot_index) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  std::vector<repl::RpcRequest> calls;
  if (!mount_bar_calls(dash_ability, slot_index, calls)) return std::nullopt;
  return rpcs(Role::PlayerController, "the mount action bar", calls);
}

std::optional<Send> PlayerBootstrap::callout_add(const std::string &message, int32_t type, int32_t point_size,
                                                 int64_t creation_ticks, uint64_t key, const std::string &table,
                                                 const std::string &message_key, uint64_t chat_category) {
  if (!ok_ || !second_state_added_ || key == 0 || callouts_removed_.count(key) != 0) return std::nullopt;
  if (callout_ids_.count(key) == 0) {
    repl::FastArrayState *array = pc_.obj.arrays.get(*pc_.obj.schema, "CalloutQueueReplication");
    if (!array) return std::nullopt;
    const CalloutSpec spec{message, type, point_size, creation_ticks, key, table, message_key, chat_category};
    const auto id = array->add(callout_state(spec));
    if (!id) return std::nullopt;
    callout_ids_[key] = *id;
  }
  return update(Role::PlayerController, "a callout", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::callout_remove(uint64_t key) {
  if (!ok_ || !second_state_added_) return std::nullopt;
  const auto it = callout_ids_.find(key);
  if (it != callout_ids_.end()) {
    repl::FastArrayState *array = pc_.obj.arrays.get(*pc_.obj.schema, "CalloutQueueReplication");
    if (!array || !array->remove(it->second)) return std::nullopt;
    callout_ids_.erase(it);
    callouts_removed_.insert(key);
  } else if (callouts_removed_.count(key) == 0) {
    return std::nullopt;
  }
  return update(Role::PlayerController, "a callout is taken off", pc_, pc_shadow_);
}

std::optional<Send> PlayerBootstrap::good_move_ack(float time_stamp) const {
  if (!ok_) return std::nullopt;
  const schema::ClassSchema &cls = *pawn_.obj.schema;
  const schema::NetField *f = cls.field("ClientMoveResponsePacked");
  if (!f) return std::nullopt;
  BunchWriter params;
  params.write_bits(645, 10);
  params.write_bits(std::bit_cast<uint32_t>(time_stamp), 32);
  BunchWriter field;
  if (repl::write_field(cls, f->index, params, field) == 0) return std::nullopt;
  Emission e;
  e.reliable = false;
  repl::BlockHeader header;
  if (repl::write_content_block(e.payload, header, field) == 0) return std::nullopt;
  Send s;
  s.role = Role::Pawn;
  s.what = "ClientMoveResponsePacked";
  s.emission = std::move(e);
  return s;
}

}  // namespace aoc::worldd::bootstrap
