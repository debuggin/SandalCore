// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "bootstrap/captured_character.h"
#include "capture_resource.h"
#include "items/acquired.h"
#include "protocol/repl/item_codec.h"
#include "protocol/schema/generated/index.h"

#include "test_world_items_common.h"

using namespace aoc::worldd;

static ItemContainer pick_fuzz_container(std::mt19937_64 &rng) {
  if (rng() % 8 == 0)
    return static_cast<ItemContainer>(rng() % 5);
  return rng() % 2 ? Bag : Worn;
}

static int pick_fuzz_slot(std::mt19937_64 &rng, ItemContainer c) {
  const int limit = c == Bag ? 70 : c == Worn ? 83 : 100;
  if (rng() % 10 == 0)
    return static_cast<int>(rng() % 300) - 100;
  return static_cast<int>(rng() % limit);
}

static std::string fuzz_guid_of_slot(const Inv &inv, std::mt19937_64 &rng,
                                     ItemContainer c, int s) {
  const auto *it = inv.at(c, s);
  if (it && rng() % 6)
    return it->instance_guid;
  return rng() % 2 ? guid_n(static_cast<uint32_t>(rng() % 600))
                   : std::string("garbage");
}

static aoc::worldd::items::Outcome
step_fuzz_default_action(Inv &inv, std::mt19937_64 &rng,
                         const FakeCatalog &catalog) {
  const auto all = inv.items();
  if (all.empty())
    return {};
  const aoc::worldd::svc::InventoryItem &it = all[rng() % all.size()];
  if (it.container == Bag && rng() % 2)
    return inv.move(Bag, it.slot, it.quantity, it.instance_guid, Bag,
                    static_cast<int>(rng() % 70));
  if (it.container != ItemContainer::Quest)
    return inv.use(it.container, it.slot, it.instance_guid, kAny, catalog);
  return {};
}

static aoc::worldd::items::Outcome
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

static void apply_fuzz_replay_event(
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

static void check_fuzz_step_invariants(const Inv &before, const Inv &inv,
                                       const items::Outcome &o,
                                       size_t &invariant_breaks) {
  std::string why;
  if (!Inv::from_items(inv.items(), why))
    ++invariant_breaks;
  if (inv.size() != before.size())
    ++invariant_breaks;
  std::map<std::pair<int, int>, svc::InventoryItem> replay;
  for (const auto &it : before.items()) {
    replay[{static_cast<int>(it.container), it.slot}] = it;
  }
  for (const items::Event &e : o.events) {
    apply_fuzz_replay_event(replay, e, invariant_breaks);
  }
  std::vector<svc::InventoryItem> replayed;
  for (const auto &[k, it] : replay)
    replayed.push_back(it);
  if (!(inventory_of(replayed) == inv))
    ++invariant_breaks;
}

static Inv make_hostile_fuzz_start(std::mt19937_64 &rng) {
  const int64_t pool[] = {
      fi::kPotion,        fi::kRingA,      fi::kRingB,   fi::kEarLeftFirst,
      fi::kEarRightFirst, fi::kSword2H,    fi::kMace1H,  fi::kShield,
      fi::kHeavyChest,    fi::kGatherCape, fi::kCostume, fi::kCloak,
      fi::kShirt,         fi::kFocus,      fi::kBook,    99999,
      fi::kChest};
  std::vector<svc::InventoryItem> start = bard_kit();
  uint32_t n = 500;
  const int bag_items = static_cast<int>(rng() % 70);
  std::set<int> used;
  while (static_cast<int>(used.size()) < bag_items) {
    used.insert(static_cast<int>(rng() % 70));
  }
  for (const int s : used) {
    start.push_back(make_item(Bag, s,
                              pool[rng() % (sizeof pool / sizeof pool[0])], ++n,
                              1 + static_cast<int32_t>(rng() % 3)));
  }
  return inventory_of(start);
}

struct HostileFuzzTallies {
  size_t applied = 0;
  size_t refused = 0;
  size_t invariant_breaks = 0;
  size_t lost = 0;
};

static void run_hostile_fuzz_step(Inv &inv, std::mt19937_64 &rng,
                                  const FakeCatalog &catalog,
                                  const std::multiset<std::string> &guids,
                                  HostileFuzzTallies &t) {
  const Inv before = inv;
  const items::Outcome o = step_fuzz_action(inv, rng, catalog);
  if (!o.ok) {
    ++t.refused;
    if (!o.events.empty() || o.equipment_changed || !(before == inv))
      ++t.invariant_breaks;
    return;
  }
  ++t.applied;
  std::multiset<std::string> now;
  for (const auto &it : inv.items())
    now.insert(it.instance_guid);
  if (now != guids)
    ++t.lost;
  check_fuzz_step_invariants(before, inv, o, t.invariant_breaks);
}

static void test_fuzz() {
  std::printf("hostile requests\n");
  const FakeCatalog catalog = fixture_catalog();
  std::mt19937_64 rng(0xB2B2);
  HostileFuzzTallies t;
  for (int round = 0; round < 60; ++round) {
    Inv inv = make_hostile_fuzz_start(rng);
    std::multiset<std::string> guids;
    for (const auto &it : inv.items())
      guids.insert(it.instance_guid);
    for (int step = 0; step < 300; ++step) {
      run_hostile_fuzz_step(inv, rng, catalog, guids, t);
    }
  }
  CHECK(t.applied > 1000 && t.refused > 1000,
        (std::to_string(t.applied) + " requests applied and " +
         std::to_string(t.refused) + " refused")
            .c_str());
  CHECK(t.invariant_breaks == 0 && t.lost == 0,
        "never a broken invariant: the items stay valid, one per slot and "
        "guid; none is made or lost; the events replayed on the earlier "
        "inventory give the later one; a refusal changes and says nothing");
}

namespace {

struct CachedCatalog : items::ItemCatalog {
  const items::ItemCatalog &source;
  mutable std::map<int64_t, std::optional<items::ItemInfo>> cache;
  explicit CachedCatalog(const items::ItemCatalog &s) : source(s) {}
  std::optional<items::ItemInfo> info(int64_t record) const override {
    auto it = cache.find(record);
    if (it == cache.end())
      it = cache.emplace(record, source.info(record)).first;
    return it->second;
  }
};

struct FuzzStats {
  size_t applied = 0, refused = 0, equips = 0, displacing = 0, unequips = 0,
         max_events = 0, violations = 0;
  std::vector<std::string> first;
  void violation(const std::string &what) {
    if (++violations <= 8)
      first.push_back(what);
  }
};

struct FuzzPools {
  std::vector<int64_t> wearable;
  std::vector<int64_t> twoh;
  std::vector<int64_t> plain;
};

size_t pick_idx(std::mt19937_64 &rng, uint64_t n) {
  return static_cast<size_t>(rng() % n);
}

bool can_place_fuzz_footprint(const std::vector<int> &foot, int place,
                              const std::map<int, int64_t> &worn,
                              const std::set<int> &covered) {
  if (worn.count(place))
    return false;
  for (const int s : foot) {
    if (covered.count(s) || worn.count(s))
      return false;
  }
  return true;
}

int64_t pick_fuzz_worn_record(const FuzzPools &pools, std::mt19937_64 &rng) {
  if (pick_idx(rng, 5) == 0 && !pools.twoh.empty())
    return pools.twoh[pick_idx(rng, pools.twoh.size())];
  return pools.wearable[pick_idx(rng, pools.wearable.size())];
}

void populate_fuzz_worn(const items::ItemCatalog &catalog,
                        const FuzzPools &pools, std::mt19937_64 &rng,
                        uint32_t &n, std::vector<svc::InventoryItem> &start) {
  std::map<int, int64_t> worn;
  std::set<int> covered;
  const int want = 1 + static_cast<int>(pick_idx(rng, 18));
  for (int k = 0; k < want; ++k) {
    const int64_t r = pick_fuzz_worn_record(pools, rng);
    const auto info = *catalog.info(r);
    const int slot = info.equip_slots[pick_idx(rng, info.equip_slots.size())];
    const std::vector<int> foot =
        info.two_handed
            ? std::vector<int>(info.equip_slots.begin(), info.equip_slots.end())
            : std::vector<int>{slot};
    const int place = info.two_handed ? info.equip_slots.front() : slot;
    if (!can_place_fuzz_footprint(foot, place, worn, covered))
      continue;
    worn[place] = r;
    covered.insert(foot.begin(), foot.end());
    start.push_back(make_item(Worn, place, r, ++n));
  }
}

std::vector<svc::InventoryItem>
make_fuzz_round_items(const items::ItemCatalog &catalog, const FuzzPools &pools,
                      std::mt19937_64 &rng, int round) {
  std::vector<svc::InventoryItem> start;
  uint32_t n = 1000u * static_cast<uint32_t>(round + 1);
  populate_fuzz_worn(catalog, pools, rng, n, start);
  const int levels[] = {0, 5, 30, 60, 66, 68, 69, 70, 70, 70};
  const int fill = levels[pick_idx(rng, sizeof levels / sizeof levels[0])];
  std::set<int> used;
  while (static_cast<int>(used.size()) < fill) {
    used.insert(static_cast<int>(pick_idx(rng, 70)));
  }
  for (const int s : used) {
    const size_t k = pick_idx(rng, 10);
    const int64_t r = k < 6
                          ? pools.wearable[pick_idx(rng, pools.wearable.size())]
                      : k < 8 && !pools.twoh.empty()
                          ? pools.twoh[pick_idx(rng, pools.twoh.size())]
                          : pools.plain[pick_idx(rng, pools.plain.size())];
    start.push_back(
        make_item(Bag, s, r, ++n, 1 + static_cast<int32_t>(pick_idx(rng, 3))));
  }
  std::set<int> quest_slots;
  for (int q = static_cast<int>(pick_idx(rng, 4)); q > 0; --q) {
    const int slot = static_cast<int>(pick_idx(rng, 100));
    if (quest_slots.insert(slot).second) {
      start.push_back(make_item(ItemContainer::Quest, slot,
                                pools.plain[pick_idx(rng, pools.plain.size())],
                                ++n));
    }
  }
  return start;
}
// C | Y | B.C | O | A

items::Outcome step_review_legal_use(Inv &inv,
                                     const items::ItemCatalog &catalog,
                                     std::mt19937_64 &rng, bool &is_equip,
                                     bool &is_unequip) {
  const auto all = inv.items();
  if (all.empty())
    return {};
  const auto &it = all[pick_idx(rng, all.size())];
  if (it.container == ItemContainer::Quest)
    return {};
  uint32_t requested = kAny;
  if (it.container == Bag && pick_idx(rng, 5) == 0) {
    const auto info = catalog.info(it.item_record_id);
    if (info && info->wearable()) {
      requested = info->equip_slots[pick_idx(rng, info->equip_slots.size())];
    }
  }
  is_equip = it.container == Bag;
  is_unequip = it.container == Worn;
  return inv.use(it.container, it.slot, it.instance_guid, requested, catalog);
}

items::Outcome step_review_bag_move_or_swap(Inv &inv, std::mt19937_64 &rng) {
  const auto all = inv.items();
  std::vector<const svc::InventoryItem *> in_bag;
  for (const auto &it : all) {
    if (it.container == Bag)
      in_bag.push_back(&it);
  }
  if (in_bag.empty())
    return {};
  const auto *a = in_bag[pick_idx(rng, in_bag.size())];
  const int to = static_cast<int>(pick_idx(rng, 70));
  const auto *target = inv.at(Bag, to);
  if (target) {
    return inv.swap(Bag, a->slot, a->quantity, a->instance_guid, Bag, to,
                    target->instance_guid, target->quantity);
  }
  return inv.move(Bag, a->slot, a->quantity, a->instance_guid, Bag, to);
}

items::Outcome step_review_hostile_move_or_swap(Inv &inv,
                                                std::mt19937_64 &rng) {
  const auto a = static_cast<ItemContainer>(pick_idx(rng, 4));
  const auto b = static_cast<ItemContainer>(pick_idx(rng, 4));
  const int sa = static_cast<int>(pick_idx(rng, 100)) - 10;
  const int sb = static_cast<int>(pick_idx(rng, 100)) - 10;
  const auto *ia = inv.at(a, sa);
  const auto *ib = inv.at(b, sb);
  const std::string ga =
      ia && pick_idx(rng, 5)
          ? ia->instance_guid
          : guid_n(static_cast<uint32_t>(pick_idx(rng, 500000)));
  const std::string gb =
      ib && pick_idx(rng, 5)
          ? ib->instance_guid
          : guid_n(static_cast<uint32_t>(pick_idx(rng, 500000)));
  if (pick_idx(rng, 2))
    return inv.move(a, sa, ia ? ia->quantity : 1, ga, b, sb);
  return inv.swap(a, sa, ia ? ia->quantity : 1, ga, b, sb, gb,
                  ib ? ib->quantity : 1);
}

items::Outcome step_review_fuzz_action(Inv &inv,
                                       const items::ItemCatalog &catalog,
                                       std::mt19937_64 &rng, bool &is_equip,
                                       bool &is_unequip) {
  switch (pick_idx(rng, 6)) {
  case 0:
  case 1:
  case 2:
    return step_review_legal_use(inv, catalog, rng, is_equip, is_unequip);
  case 3:
    return step_review_bag_move_or_swap(inv, rng);
  case 4: {
    const auto c = static_cast<ItemContainer>(pick_idx(rng, 5));
    const int slot = static_cast<int>(pick_idx(rng, 300)) - 100;
    std::string g = guid_n(static_cast<uint32_t>(pick_idx(rng, 500000)));
    if (const auto *it = inv.at(c, slot); it && pick_idx(rng, 2))
      g = it->instance_guid;
    const uint32_t req = pick_idx(rng, 3) == 0
                             ? static_cast<uint32_t>(pick_idx(rng, 400))
                             : kAny;
    return inv.use(c, slot, g, req, catalog);
  }
  default:
    return step_review_hostile_move_or_swap(inv, rng);
  }
}

void check_one_worn_slot_invariant(
    int slot, const svc::InventoryItem &it,
    const std::map<int, const svc::InventoryItem *> &in_slot,
    const items::ItemCatalog &catalog, FuzzStats &st) {
  const auto info = catalog.info(it.item_record_id);
  const bool listed =
      info && std::find(info->equip_slots.begin(), info->equip_slots.end(),
                        static_cast<uint8_t>(slot)) != info->equip_slots.end();
  if (!listed) {
    st.violation("an item is worn in a slot its record does not list: slot " +
                 std::to_string(slot) + " record " +
                 std::to_string(it.item_record_id));
  }
  if (!info || !info->two_handed)
    return;
  for (const uint8_t s : info->equip_slots) {
    if (s != slot && in_slot.count(s)) {
      st.violation("a two-hander in slot " + std::to_string(slot) +
                   " covers slot " + std::to_string(s) +
                   ", which another item holds");
    }
  }
}

void check_review_worn_invariants(const Inv &inv,
                                  const items::ItemCatalog &catalog,
                                  FuzzStats &st) {
  std::map<int, const svc::InventoryItem *> in_slot;
  const auto after = inv.items();
  for (const auto &it : after) {
    if (it.container == Worn)
      in_slot[it.slot] = &it;
  }
  for (const auto &[slot, it] : in_slot) {
    check_one_worn_slot_invariant(slot, *it, in_slot, catalog, st);
  }
}

void apply_review_replay_event(
    std::map<std::pair<int, int>, svc::InventoryItem> &replay,
    const items::Event &e, FuzzStats &st) {
  const std::pair<int, int> key{static_cast<int>(e.container), e.slot};
  if (e.type == Add) {
    if (replay.count(key)) {
      st.violation("an Add into an occupied slot (the client would overwrite "
                   "without a Remove)");
    }
    if (e.slot < 0 || e.slot >= items::slot_count(e.container))
      st.violation("an Add outside the storage");
    replay[key] = e.item;
    return;
  }
  const auto f = replay.find(key);
  if (f == replay.end() || f->second.instance_guid != e.item.instance_guid) {
    st.violation("a Remove of an item that is not there");
    return;
  }
  replay.erase(f);
}

void check_review_replay_invariants(const Inv &before, const Inv &inv,
                                    const items::Outcome &o, FuzzStats &st) {
  std::map<std::pair<int, int>, svc::InventoryItem> replay;
  for (const auto &it : before.items()) {
    replay[{static_cast<int>(it.container), it.slot}] = it;
  }
  for (const items::Event &e : o.events) {
    apply_review_replay_event(replay, e, st);
  }
  std::vector<svc::InventoryItem> replayed;
  for (const auto &[k, it] : replay)
    replayed.push_back(it);
  std::string replay_error;
  const auto replayed_inv = Inv::from_items(replayed, replay_error);
  if (!replayed_inv) {
    st.violation(
        "the replay is invalid (two items in a slot or one guid twice): " +
        replay_error);
  } else if (!(*replayed_inv == inv)) {
    st.violation("the events replayed on the earlier inventory do not give the "
                 "later one");
  }
  (void)items::held_records(inv);
  (void)items::appearance_ids(inv);
}

void record_applied_fuzz_outcome(const items::Outcome &o, bool is_equip,
                                 bool is_unequip, FuzzStats &st) {
  ++st.applied;
  if (is_equip) {
    ++st.equips;
    if (o.events.size() > 2)
      ++st.displacing;
  }
  if (is_unequip)
    ++st.unequips;
  st.max_events = std::max(st.max_events, o.events.size());
  if (o.events.size() > 16) {
    st.violation("an outcome has more than 16 events (kMaxEventsPerChain): "
                 "the client would get none");
  }
}

void verify_post_step_inventory(const Inv &inv,
                                const std::multiset<std::string> &guids,
                                FuzzStats &st) {
  std::multiset<std::string> now;
  for (const auto &it : inv.items())
    now.insert(it.instance_guid);
  if (now != guids || inv.size() != guids.size())
    st.violation("items made or lost");
  std::string why;
  if (!Inv::from_items(inv.items(), why))
    st.violation("invalid inventory after an applied request: " + why);
}

void run_review_fuzz_round(Inv &inv, const items::ItemCatalog &catalog,
                           std::mt19937_64 &rng, FuzzStats &st) {
  std::multiset<std::string> guids;
  for (const auto &it : inv.items())
    guids.insert(it.instance_guid);
  for (int step = 0; step < 300; ++step) {
    const Inv before = inv;
    bool is_equip = false, is_unequip = false;
    const items::Outcome o =
        step_review_fuzz_action(inv, catalog, rng, is_equip, is_unequip);
    if (!o.ok) {
      if (!o.events.empty() || o.equipment_changed || !(before == inv))
        st.violation("a refusal changed something or said something");
      ++st.refused;
      continue;
    }
    record_applied_fuzz_outcome(o, is_equip, is_unequip, st);
    verify_post_step_inventory(inv, guids, st);
    check_review_worn_invariants(inv, catalog, st);
    check_review_replay_invariants(before, inv, o, st);
  }
}

FuzzPools build_fuzz_pools(const items::ItemCatalog &catalog,
                           const std::vector<int64_t> &pool) {
  FuzzPools pools;
  for (const int64_t r : pool) {
    const auto info = catalog.info(r);
    if (!info)
      continue;
    if (!info->wearable()) {
      pools.plain.push_back(r);
      continue;
    }
    pools.wearable.push_back(r);
    if (info->two_handed)
      pools.twoh.push_back(r);
  }
  return pools;
}

void fuzz_inventories(const items::ItemCatalog &catalog,
                      const std::vector<int64_t> &pool, uint64_t seed,
                      int rounds, FuzzStats &st) {
  const FuzzPools pools = build_fuzz_pools(catalog, pool);
  if (pools.wearable.empty() || pools.plain.empty()) {
    st.violation("the pool has no wearable or no plain record");
    return;
  }
  std::mt19937_64 rng(seed);
  for (int round = 0; round < rounds; ++round) {
    const auto start = make_fuzz_round_items(catalog, pools, rng, round);
    std::string setup_error;
    const auto first = Inv::from_items(start, setup_error);
    if (!first) {
      st.violation("the setup is invalid: " + setup_error);
      return;
    }
    Inv inv = *first;
    run_review_fuzz_round(inv, catalog, rng, st);
  }
}

} // namespace

static void report_fuzz(const char *what, const FuzzStats &st,
                        size_t min_applied, double seconds) {
  std::printf("    %s: %zu applied, %zu refused (equips %zu, displacing %zu, "
              "unequips %zu), at most %zu events, %.2f s\n",
              what, st.applied, st.refused, st.equips, st.displacing,
              st.unequips, st.max_events, seconds);
  for (const std::string &v : st.first)
    std::printf("      [VIOLATION] %s\n", v.c_str());
  CHECK(st.violations == 0 && st.applied >= min_applied && st.refused > 0,
        (std::string(what) +
         ": never a violated invariant (no item made or lost, valid and worn "
         "only where the record lists, events replay to the later inventory)")
            .c_str());
}
// -- C:Y:B & C.O.A --

static void step_sampled_item_guids(sqlite3_stmt *q,
                                    std::vector<int64_t> &pool) {
  for (size_t i = 0; sqlite3_step(q) == SQLITE_ROW; ++i) {
    if (i % 5 == 0)
      pool.push_back(sqlite3_column_int64(q, 0));
  }
}

static std::vector<int64_t> read_real_item_pool(const char *real) {
  std::vector<int64_t> pool;
  sqlite3 *raw = nullptr;
  if (sqlite3_open_v2(real, &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(raw);
    return pool;
  }
  sqlite3_stmt *q = nullptr;
  if (sqlite3_prepare_v2(raw, "SELECT guid FROM item ORDER BY guid", -1, &q,
                         nullptr) == SQLITE_OK) {
    step_sampled_item_guids(q, pool);
  }
  sqlite3_finalize(q);
  sqlite3_close(raw);
  return pool;
}

static void test_review_fuzz() {
  std::printf(
      "the review's fuzz: long sequences with the strongest invariants\n");
  {
    FakeCatalog fixture = fixture_catalog();
    fixture.add(fi::kPotion, {});
    std::vector<int64_t> pool;
    for (const auto &[record, info] : fixture.m)
      pool.push_back(record);
    pool.push_back(99999);
    FuzzStats st;
    const auto started = std::chrono::steady_clock::now();
    for (const uint64_t seed : {0xB2C0FFEEull, 1ull, 2ull}) {
      fuzz_inventories(fixture, pool, seed, 40, st);
    }
    report_fuzz("the fixture's catalog, three seeds", st, 5000,
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              started)
                    .count());
  }
  const char *real = std::getenv("AOC_CONTENT_DB");
  if (!real || !*real) {
    std::printf(
        "  (AOC_CONTENT_DB is not set: the real catalog's half is skipped)\n");
    return;
  }
  std::string error;
  auto db = content::ContentDb::open(real, error);
  CHECK(db.has_value(), "the real content.db opens");
  if (!db)
    return;
  const std::vector<int64_t> pool = read_real_item_pool(real);
  items::ContentItemCatalog real_catalog(*db);
  CachedCatalog cached(real_catalog);
  CHECK(pool.size() > 1000, "the real file's records are read for the pool");
  FuzzStats st;
  const auto started = std::chrono::steady_clock::now();
  for (const uint64_t seed : {0xB2C0FFEEull, 7ull}) {
    fuzz_inventories(cached, pool, seed, 50, st);
  }
  report_fuzz(
      "the real catalog, two seeds", st, 5000,
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count());
}

static void test_review_codec_noise() {
  std::printf("random bytes through the item readers\n");
  size_t read_ok = 0;
  std::mt19937_64 rng(99);
  for (int i = 0; i < 40000; ++i) {
    std::vector<uint8_t> bytes(1 + rng() % 300);
    for (auto &b : bytes)
      b = static_cast<uint8_t>(rng());
    if (rng() % 3 == 0)
      bytes[0] |= 1;
    aoc::protocol::wire::PacketReader a(bytes.data(), bytes.size());
    if (repl::read_item_instance(a))
      ++read_ok;
    aoc::protocol::wire::PacketReader b(bytes.data(), bytes.size());
    (void)repl::read_item_storage_event(b);
  }
  std::printf("    %zu instances read from 40,000 buffers of noise\n", read_ok);
  CHECK(true, "no read ran past its buffer (a build with AddressSanitizer "
              "would have stopped it)");
}

static void check_harvest_event(const schema::RpcSchema &rpc,
                                const capture_resource::Harvest &h,
                                size_t &same_text, size_t &same_bits,
                                size_t &bands) {
  const auto bytes = capture_blocks::bytes_of(h.full_hex);
  repl::CodecOptions opts;
  opts.inline_objects = true;
  size_t pos = 0;
  auto params =
      repl::read_rpc_params(rpc, bytes.data(), h.full_bits, pos, opts);
  if (!params || pos != h.full_bits)
    return;
  repl::RpcCall call;
  call.rpc = &rpc;
  call.params = std::move(*params);
  call.decoded = true;
  const auto retail = repl::parse_storage_event(call);
  if (!retail || !retail->callout)
    return;
  const std::string &said = retail->callout->message;
  const size_t open = said.find("> [");
  const size_t close = said.find(']', open == std::string::npos ? 0 : open);
  if (open == std::string::npos || close == std::string::npos)
    return;
  const std::string name = said.substr(open + 3, close - open - 3);
  repl::ItemStorageEvent mine = *retail;
  mine.callout =
      items::acquired_callout(mine.item, name, items::default_rarity_facts(),
                              retail->callout->creation_time.ticks);
  same_text += mine.callout->message == said ? 1 : 0;
  const size_t item_tag = said.find("Item_");
  const std::string_view retail_band = std::string_view(said).substr(
      item_tag + 5, said.find('"', item_tag) - item_tag - 5);
  bands +=
      items::rarity_name(items::default_rarity_facts().rarity_of(h.quality)) ==
              retail_band
          ? 1
          : 0;
  const auto mine_params = repl::storage_event_params(rpc, mine);
  BunchWriter w;
  bool failed = false;
  if (mine_params)
    repl::write_rpc_params(rpc, *mine_params, w, opts, &failed);
  if (mine_params && !failed && w.bit_pos() == h.full_bits &&
      capture_blocks::same_bit_range(w.data(), bytes.data(), h.full_bits,
                                     nullptr)) {
    ++same_bits;
  }
}

static void check_acquired_bands_and_quantity() {
  const auto band = [](double quality) {
    return items::rarity_name(items::default_rarity_facts().rarity_of(quality));
  };
  const bool lower_bands =
      std::array<std::string_view, 6>{band(5),  band(6),  band(15),
                                      band(16), band(30), band(31)} ==
      std::array<std::string_view, 6>{"Common", "Uncommon", "Uncommon",
                                      "Rare",   "Rare",     "Heroic"};
  const bool upper_bands =
      std::array<std::string_view, 6>{band(50),  band(51),  band(71),
                                      band(101), band(200), band(0)} ==
      std::array<std::string_view, 6>{"Heroic",   "Epic",     "Legendary",
                                      "Artifact", "Artifact", "Common"};
  CHECK(lower_bands && upper_bands,
        "the rarity bands: Common 1-5, Uncommon 6-15, Rare 16-30, Heroic "
        "31-50, Epic 51-70, Legendary 71-100, Artifact 101-200");
  repl::ItemInstance one;
  one.has_item = true;
  one.record_id = 42;
  one.quality = 1;
  one.quantity = 1;
  repl::ItemInstance two = one;
  two.quantity = 2;
  const items::ItemRarityFacts &common = items::default_rarity_facts();
  CHECK(items::acquired_text(one, "A", common).find("[A]</>") !=
                std::string::npos &&
            items::acquired_text(one, "A", common).find("</>x") ==
                std::string::npos &&
            items::acquired_text(two, "A", common).find("[A] </>x2") !=
                std::string::npos,
        "a quantity of 1 ends [Name]</> and of 2 ends [Name] </>x2");
}

static void test_acquired_popup() {
  std::printf("the pick-up popup: items/acquired.h against the 24 retail "
              "harvest events\n");
  const schema::RpcSchema *rpc =
      class_named("AoCPlayerControllerBP_C").rpc("ClientReceiveStorageEvent");
  CHECK(rpc != nullptr, "the PlayerController has ClientReceiveStorageEvent");
  if (!rpc)
    return;
  size_t n = 0, same_text = 0, same_bits = 0, bands = 0;
  for (const capture_resource::Harvest &h : capture_resource::kHarvests) {
    ++n;
    check_harvest_event(*rpc, h, same_text, same_bits, bands);
  }
  CHECK(n == 24 && same_text == n,
        "the text acquired_text makes from the record, quality, quantity and "
        "name is the retail text, 24 of 24");
  CHECK(bands == n, "the style is the band of the quality (Common 1, Uncommon "
                    "6, Rare 16, Heroic 31) in all 24");
  CHECK(same_bits == n,
        "the event with acquired_callout's callout is the retail event to the "
        "last bit, 24 of 24 (the creation time is the capture's)");
  check_acquired_bands_and_quantity();
}

int main() {
  test_acquired_popup();
  test_fuzz();
  test_review_fuzz();
  test_review_codec_noise();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
