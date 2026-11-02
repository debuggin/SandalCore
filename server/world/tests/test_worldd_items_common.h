// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/bootstrap.h"
#include "bootstrap/captured_setup.h"
#include "capture_blocks.h"
#include "capture_equip.h"
#include "capture_items.h"
#include "content_db_fixture.h"
#include "world_stats.h"
// ~ C_Y_B - C O A ~

#include <sqlite3.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace bs = aoc::worldd::bootstrap;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace emit = aoc::protocol::emit;
namespace cap = capture_items;
namespace fs = std::filesystem;
using aoc::worldd::svc::ItemContainer;
using emit::BunchWriter;
using Inv = aoc::worldd::items::Inventory;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      std::printf("  [ok ] %s\n", msg);                                        \
      g_pass++;                                                                \
    } else {                                                                   \
      std::printf("  [FAIL] %s\n", msg);                                       \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

namespace cf = content_fixture;
namespace fi = content_fixture::item;

inline const schema::ClassSchema &class_named(const char *name) {
  const schema::ClassSchema *c = gen::find_class_schema(name);
  if (!c) {
    std::printf("  [FAIL] no class %s\n", name);
    std::exit(2);
  }
  return *c;
}

inline std::string guid_n(uint32_t n) {
  char buf[33];
  std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n,
                0x1000u + n, 0x2000u + n, n);
  return buf;
}

inline aoc::worldd::svc::InventoryItem make_item(ItemContainer c, int slot,
                                                 int64_t record, uint32_t n,
                                                 int32_t quantity = 1) {
  aoc::worldd::svc::InventoryItem it;
  it.container = c;
  it.slot = slot;
  it.item_record_id = record;
  it.instance_guid = guid_n(n);
  it.quantity = quantity;
  it.quality = 1;
  it.durability = 11;
  it.created_at_ms = 1'771'299'474'000ll;
  return it;
}
// -- C.Y.B x C.O.A --

struct FakeCatalog : aoc::worldd::items::ItemCatalog {
  std::map<int64_t, aoc::worldd::items::ItemInfo> m;
  mutable size_t asked = 0;
  std::optional<aoc::worldd::items::ItemInfo>
  info(int64_t record) const override {
    ++asked;
    const auto it = m.find(record);
    return it == m.end()
               ? std::nullopt
               : std::optional<aoc::worldd::items::ItemInfo>(it->second);
  }
  void add(int64_t record, std::vector<uint8_t> slots,
           bool two_handed = false) {
    m[record] = aoc::worldd::items::ItemInfo{std::move(slots), two_handed};
  }
};

inline FakeCatalog fixture_catalog() {
  FakeCatalog c;
  c.add(fi::kHelmet, {1});
  c.add(fi::kChest, {2});
  c.add(fi::kLegs, {5});
  c.add(fi::kFeet, {6});
  c.add(fi::kScepter, {15});
  c.add(fi::kFocus, {16, 18});
  c.add(fi::kBook, {17, 18}, true);
  c.add(fi::kBag, {23, 24, 25});
  c.add(fi::kPickaxe, {26});
  c.add(fi::kAxe, {29});
  c.add(fi::kSickle, {33});
  c.add(fi::kFishingRod, {35});
  c.add(fi::kBow, {38});
  c.add(fi::kSword2H, {15, 16}, true);
  c.add(fi::kMace1H, {15});
  c.add(fi::kShield, {16});
  c.add(fi::kHeavyChest, {2});
  c.add(fi::kRingA, {10, 11});
  c.add(fi::kRingB, {10, 11});
  c.add(fi::kEarLeftFirst, {14, 13});
  c.add(fi::kEarRightFirst, {13, 14});
  c.add(fi::kGatherCape, {8});
  c.add(fi::kCostume, {71});
  c.add(fi::kCloak, {79});
  c.add(fi::kShirt, {19});
  return c;
}

inline std::vector<aoc::worldd::svc::InventoryItem> bard_kit() {
  const std::pair<int, int64_t> kit[] = {
      {1, fi::kHelmet}, {2, fi::kChest},    {5, fi::kLegs},
      {6, fi::kFeet},   {15, fi::kScepter}, {16, fi::kFocus},
      {17, fi::kBook},  {23, fi::kBag},     {26, fi::kPickaxe},
      {29, fi::kAxe},   {33, fi::kSickle},  {35, fi::kFishingRod},
      {38, fi::kBow}};
  std::vector<aoc::worldd::svc::InventoryItem> out;
  uint32_t n = 100;
  for (const auto &[slot, record] : kit) {
    out.push_back(make_item(ItemContainer::Equipment, slot, record, ++n));
  }
  return out;
}

inline Inv inventory_of(std::vector<aoc::worldd::svc::InventoryItem> v) {
  std::string error;
  auto inv = Inv::from_items(v, error);
  if (!inv) {
    std::printf("  [FAIL] test setup: %s\n", error.c_str());
    std::exit(2);
  }
  return *inv;
}

inline bool same_event_fields(const aoc::worldd::items::Event &e,
                              repl::StorageEventType type, ItemContainer c,
                              int slot, int64_t record) {
  return e.type == type && e.container == c && e.slot == slot &&
         e.item.item_record_id == record && e.item.slot == slot &&
         e.item.container == c;
}

inline bool same_instigated(const aoc::worldd::items::Event &e,
                            repl::StorageEventType type, ItemContainer c,
                            int slot, int64_t record) {
  return same_event_fields(e, type, c, slot, record) && e.instigated;
}

inline bool same_uninstigated(const aoc::worldd::items::Event &e,
                              repl::StorageEventType type, ItemContainer c,
                              int slot, int64_t record) {
  return same_event_fields(e, type, c, slot, record) && !e.instigated;
}

inline constexpr auto Add = repl::StorageEventType::Add;
inline constexpr auto Remove = repl::StorageEventType::Remove;
inline constexpr ItemContainer Bag = ItemContainer::Backpack;
inline constexpr ItemContainer Worn = ItemContainer::Equipment;
inline constexpr uint32_t kAny = 0xFFFFFFFFu;

inline bool unchanged_by(const Inv &before, const Inv &after) {
  return before == after;
}

inline const capture_equip::Record *record_of(int64_t record) {
  for (const auto &r : capture_equip::kRecords) {
    if (r.record == record)
      return &r;
  }
  return nullptr;
}

inline FakeCatalog catalog_of_captures() {
  FakeCatalog c;
  for (const auto &r : capture_equip::kRecords) {
    std::vector<uint8_t> slots;
    for (int i = 0; i < r.nslots; ++i) {
      slots.push_back(static_cast<uint8_t>(r.slots[i]));
    }
    c.add(r.record, slots, r.two_handed);
  }
  return c;
}

inline Inv inventory_before(const capture_equip::Scenario &sc) {
  std::vector<aoc::worldd::svc::InventoryItem> v;
  uint32_t n = 0;
  for (size_t i = 0; i < sc.nbefore; ++i) {
    const auto &p = sc.before[i];
    aoc::worldd::svc::InventoryItem it = make_item(
        p.container == 0 ? Bag : Worn, p.slot, p.record ? p.record : 5, ++n);
    it.instance_guid = p.guid;
    v.push_back(it);
  }
  return inventory_of(v);
}
// (C.Y.B | C-O-A)

inline std::optional<repl::RpcCall> decode_captured(const cap::Rpc &c,
                                                    const char *cls) {
  const schema::RpcSchema *rpc = class_named(cls).rpc(c.rpc);
  if (!rpc)
    return std::nullopt;
  const auto bytes = capture_blocks::bytes_of(c.hex);
  repl::CodecOptions opts;
  opts.inline_objects = true;
  size_t pos = 0;
  auto params = repl::read_rpc_params(*rpc, bytes.data(), c.bits, pos, opts);
  if (!params || pos != c.bits)
    return std::nullopt;
  repl::RpcCall call;
  call.rpc = rpc;
  call.params = std::move(*params);
  call.decoded = true;
  return call;
}

inline std::vector<BunchWriter> param_bits_of(const bs::Send &s,
                                              const schema::ClassSchema &cls) {
  std::vector<BunchWriter> out;
  const auto blocks = repl::read_content_blocks(
      s.emission.payload.data(), s.emission.payload.bit_pos(), 0);
  if (!blocks)
    return out;
  for (const auto &b : *blocks) {
    size_t pos = b.payload_pos;
    const auto h = repl::read_field_header(cls, s.emission.payload.data(),
                                           pos + b.payload_bits, pos);
    if (!h)
      return {};
    BunchWriter w;
    w.write_bit_range(s.emission.payload.data(), pos, h->payload_bits);
    out.push_back(std::move(w));
  }
  return out;
}

inline bool bits_equal(const BunchWriter &w, const cap::Rpc &c) {
  const auto want = capture_blocks::bytes_of(c.hex);
  return w.bit_pos() == c.bits &&
         capture_blocks::same_bit_range(w.data(), want.data(), c.bits, nullptr);
}
// -- C | O | A . Cyb --

struct Corpus {
  std::vector<bs::StoredItem> items;
  std::map<int, repl::StoragePayload> payload;
  std::vector<const cap::Rpc *> events;
  std::vector<const cap::Rpc *> payloads;
  repl::GuidValue character;
  emit::FIntrepidNetworkGUID guids[3];
  std::map<int, repl::StoragePayload> resource_payload;
  std::map<int, const cap::Rpc *> resource_rpc;
  std::vector<const cap::Rpc *> resource_events[3];
  std::vector<bs::StoredItem> resource_items;
};

inline void store_corpus_resource_payload(Corpus &out, const cap::Rpc &c,
                                          const repl::StoragePayload &p) {
  const int index = p.maximum_items == 24   ? 0
                    : p.maximum_items == 40 ? 1
                    : p.maximum_items == 30 ? 2
                                            : -1;
  if (index < 0 || out.resource_payload.count(index))
    return;
  out.resource_payload[index] = p;
  out.resource_rpc[index] = &c;
  for (const repl::StorageSlot &s : p.storage) {
    if (!s.item.has_item)
      continue;
    bs::StoredItem si;
    si.storage = static_cast<bs::ItemStorage>(
        static_cast<int>(bs::ItemStorage::Resource0) + index);
    si.slot = s.slot;
    si.item = s.item;
    out.resource_items.push_back(std::move(si));
  }
}

inline bool read_corpus_payload_rpc(Corpus &out, const cap::Rpc &c) {
  if (std::string(c.rpc) != "ClientReceiveStoragePayload")
    return true;
  const auto call = decode_captured(c, "PlayerPawn_C");
  const auto p = call ? repl::parse_storage_payload(*call) : std::nullopt;
  if (!p)
    return false;
  int storage = -1;
  if (p->maximum_items == 70)
    storage = static_cast<int>(bs::ItemStorage::Backpack);
  else if (p->maximum_items == 83)
    storage = static_cast<int>(bs::ItemStorage::Equipment);
  else if (p->maximum_items == 100)
    storage = static_cast<int>(bs::ItemStorage::Quest);
  if (storage < 0) {
    store_corpus_resource_payload(out, c, *p);
    return true;
  }
  if (out.payload.count(storage))
    return true;
  out.payload[storage] = *p;
  out.payloads[static_cast<size_t>(storage)] = &c;
  out.guids[storage] = p->target;
  for (const repl::StorageSlot &s : p->storage) {
    if (!s.item.has_item)
      continue;
    bs::StoredItem si;
    si.storage = static_cast<bs::ItemStorage>(storage);
    si.slot = s.slot;
    si.item = s.item;
    out.items.push_back(std::move(si));
  }
  return true;
}

inline void
append_corpus_resource_event(Corpus &out, const cap::Rpc &c,
                             const emit::FIntrepidNetworkGUID &obj) {
  for (const auto &[index, payload] : out.resource_payload) {
    if (obj == payload.target)
      out.resource_events[index].push_back(&c);
  }
}

inline bool read_corpus_event_rpc(Corpus &out, const cap::Rpc &c,
                                  bool &have_character) {
  if (std::string(c.rpc) != "ClientReceiveStorageEvent" || c.pc == 0)
    return true;
  const auto call = decode_captured(c, "AoCPlayerControllerBP_C");
  const auto e = call ? repl::parse_storage_event(*call) : std::nullopt;
  if (!e || e->targets.size() != 1)
    return false;
  const auto &obj = e->targets[0].object;
  if (obj != out.guids[0] && obj != out.guids[1] && obj != out.guids[2]) {
    append_corpus_resource_event(out, c, obj);
    return true;
  }
  out.events.push_back(&c);
  if (!have_character && e->type == 0) {
    out.character = e->instigator;
    have_character = true;
  }
  return true;
}

template <size_t N>
inline std::optional<Corpus> read_corpus(const cap::Rpc (&rpcs)[N]) {
  Corpus out;
  out.payloads.assign(3, nullptr);
  for (const cap::Rpc &c : rpcs) {
    if (!read_corpus_payload_rpc(out, c))
      return std::nullopt;
  }
  if (out.payload.size() != 3)
    return std::nullopt;
  bool have_character = false;
  for (const cap::Rpc &c : rpcs) {
    if (!read_corpus_event_rpc(out, c, have_character))
      return std::nullopt;
  }
  if (!have_character)
    return std::nullopt;
  return out;
}
// [C_Y_B ~ CoA]

inline bs::PlayerSetup setup_of(const Corpus &c) {
  auto s = bs::captured_character_setup();
  if (!s) {
    std::printf("  [FAIL] the captured character does not build\n");
    std::exit(2);
  }
  s->character_guid = {c.character.a, c.character.b, c.character.c,
                       c.character.d};
  s->guids.backpack = c.guids[0];
  s->guids.equipment = c.guids[1];
  s->guids.quest_storage = c.guids[2];
  s->items = c.items;
  for (const auto &[index, payload] : c.resource_payload) {
    s->guids.resource_storage[static_cast<size_t>(index)] = payload.target;
    bs::ResourceBagSetup &bag = s->resource_bags[static_cast<size_t>(index)];
    bag.present = true;
    bag.columns = static_cast<int>(payload.dimension.x);
    bag.rows = static_cast<int>(payload.dimension.y);
    bag.override_matrix.clear();
    for (const std::vector<int32_t> &column : payload.override_matrix) {
      bag.override_matrix.emplace_back(column.begin(), column.end());
    }
  }
  for (const bs::StoredItem &it : c.resource_items) {
    s->items.push_back(it);
  }
  return std::move(*s);
}

inline bool run_retail_equip_requests(const capture_equip::Scenario &sc,
                                      Inv &inv, const FakeCatalog &catalog,
                                      size_t &events_checked) {
  std::vector<aoc::worldd::items::Event> got;
  bool all_ok = true;
  for (size_t i = 0; i < sc.nrequests; ++i) {
    const auto &r = sc.requests[i];
    const aoc::worldd::items::Outcome o =
        inv.use(r.storage == 3 ? Bag : Worn, r.slot, r.guid, kAny, catalog);
    all_ok &= o.ok;
    got.insert(got.end(), o.events.begin(), o.events.end());
  }
  bool events_equal = got.size() == sc.nevents;
  for (size_t i = 0; events_equal && i < sc.nevents; ++i) {
    const auto &e = sc.events[i];
    const auto type = e.type == 0 ? Add : Remove;
    const auto c = e.container == 0 ? Bag : Worn;
    events_equal = same_event_fields(got[i], type, c, e.slot, e.record) &&
                   got[i].instigated == e.instigated &&
                   got[i].item.instance_guid == e.guid;
  }
  events_checked += sc.nevents;
  return all_ok && events_equal;
}

inline bool shown_slots_present(const capture_equip::Scenario &sc,
                                const std::vector<repl::AppearanceId> &app) {
  for (size_t i = 0; i < sc.nshown; ++i) {
    bool found = false;
    for (const auto &a : app) {
      if (a.item_equip_slot == sc.shown[i].slot &&
          static_cast<int64_t>(a.record_guid) == sc.shown[i].record) {
        found = true;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

inline bool slot_refilled_after(const capture_equip::Scenario &sc, size_t idx,
                                int slot) {
  for (size_t j = idx + 1; j < sc.nevents; ++j) {
    if (sc.events[j].type == 0 && sc.events[j].container == 1 &&
        sc.events[j].slot == slot)
      return true;
  }
  return false;
}

inline bool app_has_equip_slot(const std::vector<repl::AppearanceId> &app,
                               int slot) {
  for (const auto &a : app) {
    if (a.item_equip_slot == slot)
      return true;
  }
  return false;
}

inline bool is_unrefilled_shown_remove(const capture_equip::Scenario &sc,
                                       size_t idx) {
  const auto &e = sc.events[idx];
  if (e.type != 1 || e.container != 1)
    return false;
  if (!aoc::worldd::items::shows_in_appearance(e.slot))
    return false;
  return !slot_refilled_after(sc, idx, e.slot);
}

inline bool check_retail_equip_appearance(const capture_equip::Scenario &sc,
                                          const Inv &inv) {
  if (!sc.has_appearance)
    return true;
  const auto app = aoc::worldd::items::appearance_ids(inv);
  if (!shown_slots_present(sc, app))
    return false;
  for (size_t i = 0; i < sc.nevents; ++i) {
    if (is_unrefilled_shown_remove(sc, i) &&
        app_has_equip_slot(app, sc.events[i].slot))
      return false;
  }
  return true;
}

inline int held_array_key(const bs::PlayerBootstrap &pb) {
  const repl::Subobject *sub =
      pb.pawn().find(pb.setup().guids.base_character_info);
  const schema::NetField *f =
      sub && sub->obj.schema ? sub->obj.schema->field("HeldItems") : nullptr;
  const repl::FastArrayState *a = f ? sub->obj.arrays.find(f->index) : nullptr;
  return a ? a->array_key() : -1;
}

inline std::vector<repl::ItemStorageEvent>
parse_send_storage_events(const bs::Send &send,
                          const schema::ClassSchema &pc_cls) {
  std::vector<repl::ItemStorageEvent> out;
  const auto blocks = repl::read_content_blocks(
      send.emission.payload.data(), send.emission.payload.bit_pos(), 0);
  if (!blocks)
    return out;
  const schema::RpcSchema *rpc = pc_cls.rpc("ClientReceiveStorageEvent");
  if (!rpc)
    return {};
  for (const auto &b : *blocks) {
    size_t pos = b.payload_pos;
    const auto h = repl::read_field_header(pc_cls, send.emission.payload.data(),
                                           pos + b.payload_bits, pos);
    if (!h)
      return {};
    const auto params = repl::read_rpc_params(
        *rpc, send.emission.payload.data(), pos + h->payload_bits, pos);
    const auto ev =
        params ? repl::parse_storage_event(*rpc, *params) : std::nullopt;
    if (!ev)
      return {};
    out.push_back(*ev);
  }
  return out;
}

inline bool events_match_scenario(
    const capture_equip::Scenario &sc, const bs::PlayerBootstrap &pb,
    const std::vector<repl::ItemStorageEvent> &rpcs, size_t event_index) {
  for (size_t i = 0; i < rpcs.size() && event_index + i < sc.nevents; ++i) {
    const auto &want = sc.events[event_index + i];
    const auto &got = rpcs[i];
    const bool inst_ok = !want.instigated
                             ? got.instigator == repl::GuidValue{}
                             : !(got.instigator == repl::GuidValue{});
    const emit::FIntrepidNetworkGUID want_obj =
        want.container == 0 ? pb.setup().guids.backpack
                            : pb.setup().guids.equipment;
    const bool one_ok =
        got.type == want.type && got.targets.size() == 1 &&
        got.targets[0].slots.size() == 1 &&
        got.targets[0].slots[0].slot == want.slot &&
        got.targets[0].object == want_obj &&
        static_cast<int64_t>(got.item.record_id) == want.record &&
        aoc::worldd::items::guid_hex(got.item.guid) == want.guid && inst_ok;
    if (!one_ok)
      return false;
  }
  return true;
}

inline bs::PlayerSetup
make_bootstrap_setup_from_inv(const bs::PlayerSetup &base, const Inv &inv) {
  bs::PlayerSetup setup = base;
  setup.items.clear();
  for (const aoc::worldd::svc::InventoryItem &it : inv.items()) {
    bs::StoredItem si;
    si.storage = it.container == Bag    ? bs::ItemStorage::Backpack
                 : it.container == Worn ? bs::ItemStorage::Equipment
                                        : bs::ItemStorage::Quest;
    si.slot = it.slot;
    si.item = aoc::worldd::items::to_instance(it);
    setup.items.push_back(si);
  }
  setup.held_items.array_key = bs::kInitialHeldItemsKey;
  setup.held_items.items = bs::held_items_for(inv);
  setup.appearance_ids = aoc::worldd::items::appearance_ids(inv);
  return setup;
}

inline std::vector<bs::ItemEvent>
build_bootstrap_item_events(const std::vector<aoc::worldd::items::Event> &src,
                            size_t &marks) {
  std::vector<bs::ItemEvent> events;
  marks = 0;
  for (const aoc::worldd::items::Event &e : src) {
    bs::ItemEvent ev;
    ev.type = e.type;
    ev.storage = e.container == Bag ? bs::ItemStorage::Backpack
                                    : bs::ItemStorage::Equipment;
    ev.slot = e.slot;
    ev.item = aoc::worldd::items::to_instance(e.item);
    ev.instigated = e.instigated;
    events.push_back(ev);
    marks += e.container == Worn ? 1 : 0;
  }
  return events;
}

inline bool apply_bootstrap_scenario_requests(const capture_equip::Scenario &sc,
                                              const FakeCatalog &catalog,
                                              Inv &inv, bs::PlayerBootstrap &pb,
                                              bool &applied) {
  const schema::ClassSchema &pc_cls = *pb.player_controller().obj.schema;
  size_t event_index = 0;
  bool sends_match = true;
  applied = true;
  for (size_t r = 0; r < sc.nrequests; ++r) {
    const aoc::worldd::items::Outcome o =
        inv.use(sc.requests[r].storage == 3 ? Bag : Worn, sc.requests[r].slot,
                sc.requests[r].guid, kAny, catalog);
    size_t marks = 0;
    const auto events = build_bootstrap_item_events(o.events, marks);
    const auto send = pb.item_events(events);
    if (!send)
      return false;
    const auto rpcs = parse_send_storage_events(*send, pc_cls);
    sends_match = sends_match && rpcs.size() == events.size() &&
                  events_match_scenario(sc, pb, rpcs, event_index);
    event_index += rpcs.size();
    if (o.equipment_changed) {
      applied =
          applied && pb.apply_equipment(bs::held_items_for(inv),
                                        aoc::worldd::items::appearance_ids(inv),
                                        marks ? marks : 1);
    }
  }
  return sends_match;
}

inline ItemContainer pick_fuzz_container(std::mt19937_64 &rng) {
  if (rng() % 8 == 0)
    return static_cast<ItemContainer>(rng() % 5);
  return rng() % 2 ? Bag : Worn;
}

inline int pick_fuzz_slot(std::mt19937_64 &rng, ItemContainer c) {
  const int limit = c == Bag ? 70 : c == Worn ? 83 : 100;
  if (rng() % 10 == 0)
    return static_cast<int>(rng() % 300) - 100;
  return static_cast<int>(rng() % limit);
}

inline std::string fuzz_guid_of_slot(const Inv &inv, std::mt19937_64 &rng,
                                     ItemContainer c, int s) {
  const auto *it = inv.at(c, s);
  if (it && rng() % 6)
    return it->instance_guid;
  return rng() % 2 ? guid_n(static_cast<uint32_t>(rng() % 600))
                   : std::string("garbage");
}

inline aoc::worldd::items::Outcome
step_fuzz_default_action(Inv &inv, std::mt19937_64 &rng,
                         const FakeCatalog &catalog) {
  const auto all = inv.items();
  if (all.empty())
    return {};
  const aoc::worldd::svc::InventoryItem &it = all[rng() % all.size()];
  if (it.container == Bag && rng() % 2) {
    return inv.move(Bag, it.slot, it.quantity, it.instance_guid, Bag,
                    static_cast<int>(rng() % 70));
  }
  if (it.container != ItemContainer::Quest) {
    return inv.use(it.container, it.slot, it.instance_guid, kAny, catalog);
  }
  return {};
}

inline aoc::worldd::items::Outcome
step_fuzz_action(Inv &inv, std::mt19937_64 &rng, const FakeCatalog &catalog) {
  switch (rng() % 4) {
  case 0: {
    const ItemContainer c = pick_fuzz_container(rng);
    const int s = pick_fuzz_slot(rng, c);
    const uint32_t req =
        rng() % 4 == 0 ? static_cast<uint32_t>(rng() % 100) : kAny;
    return inv.use(c, s, fuzz_guid_of_slot(inv, rng, c, s), req, catalog);
  }
  case 1: {
    const ItemContainer a = pick_fuzz_container(rng),
                        b = pick_fuzz_container(rng);
    const int sa = pick_fuzz_slot(rng, a), sb = pick_fuzz_slot(rng, b);
    const auto *it = inv.at(a, sa);
    const int32_t qty =
        it && rng() % 6 ? it->quantity : static_cast<int32_t>(rng() % 5);
    return inv.move(a, sa, qty, fuzz_guid_of_slot(inv, rng, a, sa), b, sb);
  }
  case 2: {
    const ItemContainer a = pick_fuzz_container(rng),
                        b = pick_fuzz_container(rng);
    const int sa = pick_fuzz_slot(rng, a), sb = pick_fuzz_slot(rng, b);
    const auto *ia = inv.at(a, sa);
    const auto *ib = inv.at(b, sb);
    const int32_t qa = ia && rng() % 6 ? ia->quantity : 1;
    const int32_t qb = ib && rng() % 6 ? ib->quantity : 1;
    return inv.swap(a, sa, qa, fuzz_guid_of_slot(inv, rng, a, sa), b, sb,
                    fuzz_guid_of_slot(inv, rng, b, sb), qb);
  }
  default:
    return step_fuzz_default_action(inv, rng, catalog);
  }
}

inline void apply_fuzz_replay_event(
    std::map<std::pair<int, int>, aoc::worldd::svc::InventoryItem> &replay,
    const aoc::worldd::items::Event &e, size_t &invariant_breaks) {
  const std::pair<int, int> key{static_cast<int>(e.container), e.slot};
  if (e.type == Add) {
    replay[key] = e.item;
    return;
  }
  const auto f = replay.find(key);
  if (f == replay.end() || f->second.instance_guid != e.item.instance_guid) {
    ++invariant_breaks;
    return;
  }
  replay.erase(f);
}
