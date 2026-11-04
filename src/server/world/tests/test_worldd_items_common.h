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

inline int g_pass = 0, g_fail = 0;
void run_items_equip_tests();
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

