// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "protocol/schema/generated/index.h"
#include "session/player_world.h"
#include "spawns/area_loader.h"
#include "spawns/generic_npc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifndef AREA_CONTENT_DB_PATH
#define AREA_CONTENT_DB_PATH "dist/content/content.db"
#endif

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg)                 \
  do {                                   \
    if (cond) {                          \
      std::printf("  [ok ] %s\n", msg);  \
      g_pass++;                          \
    } else {                             \
      std::printf("  [FAIL] %s\n", msg); \
      g_fail++;                          \
    }                                    \
  } while (0)

inline uint64_t g_next = 4608;

inline aoc::protocol::emit::FIntrepidNetworkGUID mint() {
  aoc::protocol::emit::FIntrepidNetworkGUID g;
  g.ObjectId = g_next;
  g_next += 2;
  g.ServerId = 60;
  g.Randomizer = 0x6ee87ee4u;
  return g;
}

inline const char *chr_n() {
  return "\n";
}

inline std::string ms_since(std::chrono::steady_clock::time_point t) {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count()) + " ms";
}

inline std::string handle_path(const aoc::protocol::schema::ClassSchema &schema, uint16_t h) {
  for (const aoc::protocol::schema::RepCommand &c : schema.commands) {
    if (c.handle == h)
      return c.path;
  }
  return "#" + std::to_string(h);
}

/// Every handle whose value differs between two states, by name.
inline std::set<std::string> differing(const aoc::protocol::repl::ObjectState &a, const aoc::protocol::repl::ObjectState &b) {
  std::set<std::string> out;
  std::set<uint16_t> handles;
  for (const auto &[h, v] : a.state.values())
    handles.insert(h);
  for (const auto &[h, v] : b.state.values())
    handles.insert(h);
  for (const uint16_t h : handles) {
    const aoc::protocol::repl::Value *x = a.state.get(h);
    const aoc::protocol::repl::Value *y = b.state.get(h);
    if (x && y && *x == *y)
      continue;
    out.insert(handle_path(*a.schema, h));
  }
  return out;
}

inline void check_gm_mount_stock(const aoc::worldd::content::MountData &md, const aoc::worldd::vendor::GmVendor &mounts_vendor, const aoc::worldd::content::MountRow *&flyer, const aoc::worldd::content::MountRow *&gryphon) {
  bool all = true;
  for (const aoc::worldd::content::MountRow &m : md.mounts) {
    all = all && mounts_vendor.items.count(m.item) == 1;
    if (m.item_name == "Mount_Flying_Dawnbreaker_1")
      flyer = &m;
    if (m.item_name == "Mount_Flying_PTR_ONLY")
      gryphon = &m;
  }
  CHECK(all, "the mounts def's stock holds all 101 mount items (51 ground, 46 flying, 4 aquatic, the GM Eagle Gryphon among them)");
}

inline void check_gm_mount_refusals(const aoc::worldd::content::ContentDb &db, const aoc::worldd::vendor::GmVendors &gm, const aoc::worldd::vendor::GmVendor &mounts_vendor, const aoc::worldd::content::MountData &md, int64_t flyer_item) {
  const auto &defs = gm.vendors();
  const auto other = std::find_if(defs.begin(), defs.end(), [](const aoc::worldd::vendor::GmVendor &v) {
    return v.name == "PTR_ONLY_Vendor_Materials";
  });
  aoc::protocol::repl::VendorPurchaseCart c2{other->guid, {{flyer_item, 1, std::nullopt, std::nullopt}}};
  CHECK(other != defs.end() && !other->items.count(flyer_item) && !gm.plan_purchase(db, *other, c2, 70).ok, "the materials vendor does not sell a mount");
  aoc::protocol::repl::VendorPurchaseCart c3;
  c3.vendor = mounts_vendor.guid;
  for (size_t i = 0; i < 3; ++i)
    c3.items.push_back({md.mounts[i].item, 1, std::nullopt, std::nullopt});
  CHECK(!gm.plan_purchase(db, mounts_vendor, c3, 2).ok, "three mounts do not fit two free slots: the cart is refused whole");
}

inline void check_gm_mount_vendor(const aoc::worldd::content::ContentDb &db, const aoc::worldd::vendor::GmVendors &gm) {
  const auto &defs = gm.vendors();
  std::string merror;
  const auto md = db.mount_data(merror);
  const auto it = std::find_if(defs.begin(), defs.end(), [](const aoc::worldd::vendor::GmVendor &v) {
    return v.name == aoc::worldd::vendor::kMountsVendorName;
  });
  CHECK(md && md->mounts.size() == 101 && it != defs.end() && gm.mount_error().empty() && gm.mounts_total() == 101, "the mount tables are read and the mounts def is among the 26");
  if (!md || it == defs.end())
    return;
  const aoc::worldd::content::MountRow *flyer = nullptr;
  const aoc::worldd::content::MountRow *gryphon = nullptr;
  check_gm_mount_stock(*md, *it, flyer, gryphon);
  CHECK(gm.mounts_added() == 81, "81 of them are added to the 20 the def lists");
  if (!flyer || !gryphon)
    return;
  aoc::protocol::repl::VendorPurchaseCart c1;
  c1.vendor = it->guid;
  c1.items.push_back({flyer->item, 1, std::nullopt, std::nullopt});
  c1.items.push_back({gryphon->item, 1, std::nullopt, std::nullopt});
  const auto p1 = gm.plan_purchase(db, *it, c1, 70);
  CHECK(p1.ok && p1.stacks.size() == 2 && p1.stacks[0].record == flyer->item && p1.stacks[0].quantity == 1 && p1.stacks[1].record == gryphon->item, "a cart of a Dawnbreaker and the GM Eagle Gryphon: two stacks of one");
  check_gm_mount_refusals(db, gm, *it, *md, flyer->item);
}

inline void check_gm_purchase_refusals(const aoc::worldd::content::ContentDb &db, const aoc::worldd::vendor::GmVendors &gm, const aoc::worldd::vendor::GmVendor &gear, const aoc::worldd::vendor::GmVendor &materials, int32_t max_stack) {
  aoc::protocol::repl::VendorPurchaseCart cart{gear.guid, {{*gear.items.begin(), 1, std::nullopt, std::nullopt}}};
  aoc::protocol::repl::VendorPurchaseCart wrong = cart;
  wrong.vendor = materials.guid;
  CHECK(!gm.plan_purchase(db, gear, wrong, 70).ok, "a cart naming another vendor is refused");
  aoc::protocol::repl::VendorPurchaseCart unlisted = cart;
  unlisted.items[0].item_record = 1;
  CHECK(!gm.plan_purchase(db, gear, unlisted, 70).ok, "an item no listing has is refused");
  for (const int32_t q : {0, -1, aoc::worldd::vendor::kMaxQuantity + 1, INT32_MIN, INT32_MAX}) {
    aoc::protocol::repl::VendorPurchaseCart c = cart;
    c.items[0].quantity = q;
    CHECK(!gm.plan_purchase(db, gear, c, 70).ok, ("a quantity of " + std::to_string(q) + " is refused").c_str());
  }
  aoc::protocol::repl::VendorPurchaseCart empty;
  empty.vendor = gear.guid;
  CHECK(!gm.plan_purchase(db, gear, empty, 70).ok, "an empty cart is refused");
  aoc::protocol::repl::VendorPurchaseCart many = cart;
  many.items.assign(aoc::worldd::vendor::kMaxCartItems + 1, cart.items[0]);
  CHECK(!gm.plan_purchase(db, gear, many, 70).ok, "a cart of 71 items is refused");
  int64_t stackable = 0;
  for (const int64_t item : materials.items) {
    std::string e;
    if (const auto rec = db.item(item, e); rec && rec->max_stack_size == max_stack) {
      stackable = item;
      break;
    }
  }
  aoc::protocol::repl::VendorPurchaseCart huge{materials.guid, {{stackable, aoc::worldd::vendor::kMaxQuantity, std::nullopt, std::nullopt}}};
  const auto big = gm.plan_purchase(db, materials, huge, 70);
  CHECK(big.ok == (static_cast<int64_t>(aoc::worldd::vendor::kMaxQuantity + max_stack - 1) / max_stack <= 70), "the largest quantity costs stacks, never a loop over the quantity");
  CHECK(!gm.plan_purchase(db, gear, cart, 0).ok, "no free slot at all refuses even one item");
}

inline void check_gm_purchase_plans(const aoc::worldd::content::ContentDb &db, const aoc::worldd::vendor::GmVendors &gm) {
  const auto &defs = gm.vendors();
  const aoc::worldd::vendor::GmVendor &gear = *std::find_if(defs.begin(), defs.end(), [](const aoc::worldd::vendor::GmVendor &v) {
    return v.name == "PTR_ONLY_Vendor_Gear_0-9";
  });
  const aoc::worldd::vendor::GmVendor &materials = *std::find_if(defs.begin(), defs.end(), [](const aoc::worldd::vendor::GmVendor &v) {
    return v.name == "PTR_ONLY_Vendor_Materials";
  });
  const int64_t gear_item = *gear.items.begin();
  aoc::protocol::repl::VendorPurchaseCart cart;
  cart.vendor = gear.guid;
  cart.items.push_back({gear_item, 1, std::nullopt, std::nullopt});
  auto plan = gm.plan_purchase(db, gear, cart, 70);
  CHECK(plan.ok && plan.stacks.size() == 1 && plan.stacks[0].record == gear_item && plan.stacks[0].quantity == 1, "one item of the gear def: one stack of one");
  int64_t stackable = 0;
  int32_t max_stack = 1;
  for (const int64_t item : materials.items) {
    std::string e;
    const auto rec = db.item(item, e);
    if (rec && rec->max_stack_size > 1) {
      stackable = item;
      max_stack = rec->max_stack_size;
      break;
    }
  }
  CHECK(stackable != 0, "the materials def lists an item that stacks");
  aoc::protocol::repl::VendorPurchaseCart mats;
  mats.vendor = materials.guid;
  mats.items.push_back({stackable, max_stack * 2 + 1, std::nullopt, std::nullopt});
  plan = gm.plan_purchase(db, materials, mats, 70);
  CHECK(plan.ok && plan.stacks.size() == 3 && plan.stacks[0].quantity == max_stack && plan.stacks[1].quantity == max_stack && plan.stacks[2].quantity == 1, "a quantity of two stacks and one is three stacks: MaxStackSize, MaxStackSize, 1");
  plan = gm.plan_purchase(db, materials, mats, 2);
  CHECK(!plan.ok && plan.stacks.empty() && plan.refusal.find("free slot") != std::string::npos, "a bag with two free slots refuses it whole");
  check_gm_purchase_refusals(db, gm, gear, materials, max_stack);
  size_t listed = 0;
  for (const aoc::worldd::vendor::GmVendor &v : defs)
    listed += v.listings;
  CHECK(listed > 5000, "the defs list thousands of listings");
}

void run_area_part2(const aoc::worldd::content::ContentDb &db, const aoc::worldd::spawns::CapturedData &captured);
