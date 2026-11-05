// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "bootstrap/captured_character.h"
#include "protocol/schema/generated/index.h"
#include "test_worldd_items_common.h"

#include <array>

using namespace aoc::worldd;

static bool run_retail_equip_requests(const capture_equip::Scenario &sc,
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

static bool shown_slots_present(const capture_equip::Scenario &sc,
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

static bool slot_refilled_after(const capture_equip::Scenario &sc, size_t idx,
                                int slot) {
  for (size_t j = idx + 1; j < sc.nevents; ++j) {
    if (sc.events[j].type == 0 && sc.events[j].container == 1 &&
        sc.events[j].slot == slot)
      return true;
  }
  return false;
}

static bool app_has_equip_slot(const std::vector<repl::AppearanceId> &app,
                               int slot) {
  for (const auto &a : app) {
    if (a.item_equip_slot == slot)
      return true;
  }
  return false;
}

static bool is_unrefilled_shown_remove(const capture_equip::Scenario &sc,
                                       size_t idx) {
  const auto &e = sc.events[idx];
  if (e.type != 1 || e.container != 1)
    return false;
  if (!aoc::worldd::items::shows_in_appearance(e.slot))
    return false;
  return !slot_refilled_after(sc, idx, e.slot);
}

static bool check_retail_equip_appearance(const capture_equip::Scenario &sc,
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

static void test_equip_chest(const FakeCatalog &catalog) {
  const std::string chest_guid = guid_n(1);
  Inv inv = inventory_of({make_item(Bag, 18, fi::kHeavyChest, 1)});
  const auto o = inv.use(Bag, 18, chest_guid, kAny, catalog);
  CHECK(o.ok && o.refusal.empty() && o.events.size() == 2 &&
            o.equipment_changed,
        "a chest piece into the empty chest slot: two events and the equipment "
        "changed");
  CHECK(o.events.size() == 2 &&
            same_uninstigated(o.events[0], Remove, Bag, 18, fi::kHeavyChest) &&
            same_instigated(o.events[1], Add, Worn, 2, fi::kHeavyChest),
        "Remove it from the bag slot (no instigator), then Add it to Chest "
        "(the character instigates), as the retail capture");
  CHECK(o.events.size() == 2 && o.events[1].item.instance_guid == chest_guid &&
            o.events[1].item.slot == 2 && o.events[1].item.container == Worn,
        "the Add carries the same instance, now in the equipment");
  CHECK(!inv.at(Bag, 18) && inv.at(Worn, 2) &&
            inv.at(Worn, 2)->instance_guid == chest_guid && inv.size() == 1,
        "the item moved: it is worn, and the bag slot is empty");
}

static void test_unequip_chest(const FakeCatalog &catalog) {
  const std::string chest_guid = guid_n(1);
  Inv inv = inventory_of({make_item(Bag, 0, fi::kPotion, 50),
                          make_item(Bag, 1, fi::kPotion, 51),
                          make_item(Bag, 3, fi::kPotion, 52),
                          make_item(Worn, 2, fi::kHeavyChest, 1)});
  const auto o = inv.use(Worn, 2, chest_guid, kAny, catalog);
  CHECK(o.ok && o.events.size() == 2 && o.equipment_changed,
        "an unequip is two events and changes the equipment");
  CHECK(o.events.size() == 2 &&
            same_instigated(o.events[0], Add, Bag, 2, fi::kHeavyChest) &&
            same_uninstigated(o.events[1], Remove, Worn, 2, fi::kHeavyChest),
        "Add it to the lowest free bag slot (2), then Remove it from Chest, as "
        "the retail capture");
  CHECK(inv.at(Bag, 2) && inv.at(Bag, 2)->instance_guid == chest_guid &&
            !inv.at(Worn, 2),
        "the item is in the bag, and Chest is empty");
}

static void test_equip_replace_and_focus(const FakeCatalog &catalog) {
  Inv inv = inventory_of({make_item(Bag, 5, fi::kHeavyChest, 2),
                          make_item(Worn, 2, fi::kChest, 1)});
  auto o = inv.use(Bag, 5, guid_n(2), kAny, catalog);
  CHECK(o.ok && o.events.size() == 4,
        "a chest piece over a chest piece: four events");
  CHECK(o.events.size() == 4 &&
            same_uninstigated(o.events[0], Remove, Bag, 5, fi::kHeavyChest) &&
            same_instigated(o.events[1], Add, Bag, 0, fi::kChest) &&
            same_uninstigated(o.events[2], Remove, Worn, 2, fi::kChest) &&
            same_instigated(o.events[3], Add, Worn, 2, fi::kHeavyChest),
        "Remove the new one, Add the old one to the lowest free bag slot (0, "
        "below the freed 5), Remove it from Chest, Add the new one");
  CHECK(inv.at(Bag, 0)->instance_guid == guid_n(1) &&
            inv.at(Worn, 2)->instance_guid == guid_n(2) && inv.size() == 2,
        "the two changed places");

  inv = inventory_of({make_item(Bag, 0, fi::kFocus, 1)});
  o = inv.use(Bag, 0, guid_n(1), items::equip::RangedOffHand, catalog);
  CHECK(o.ok && o.events.size() == 2 &&
            same_instigated(o.events[1], Add, Worn, 18, fi::kFocus),
        "a Focus with RangedOffHand asked for goes there");
  inv = inventory_of({make_item(Bag, 0, fi::kFocus, 1)});
  o = inv.use(Bag, 0, guid_n(1), items::equip::Primary, catalog);
  CHECK(!o.ok &&
            o.refusal.find("not one of the item's slots") !=
                std::string::npos &&
            inv.at(Bag, 0),
        "a slot the record does not list is refused");
  o = inv.use(Bag, 0, guid_n(1), 300, catalog);
  CHECK(!o.ok, "and so is one past a byte");
}

static void test_equip_and_unequip() {
  std::printf("wearing an item and taking it off\n");
  const FakeCatalog catalog = fixture_catalog();
  test_equip_chest(catalog);
  test_unequip_chest(catalog);
  test_equip_replace_and_focus(catalog);
}

static void test_slot_choice_rings_and_ears(const FakeCatalog &catalog) {
  Inv inv = inventory_of(
      {make_item(Bag, 0, fi::kRingA, 1), make_item(Bag, 1, fi::kRingB, 2)});
  auto o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
  CHECK(o.ok && same_instigated(o.events.back(), Add, Worn, 10, fi::kRingA),
        "the first ring goes to RingRight");
  o = inv.use(Bag, 1, guid_n(2), kAny, catalog);
  CHECK(o.ok && o.events.size() == 2 &&
            same_instigated(o.events.back(), Add, Worn, 11, fi::kRingB),
        "a second ring goes to RingLeft, nothing is displaced");
  inv = inventory_of({make_item(Bag, 0, fi::kRingA, 3),
                      make_item(Worn, 10, fi::kRingA, 1),
                      make_item(Worn, 11, fi::kRingB, 2)});
  o = inv.use(Bag, 0, guid_n(3), kAny, catalog);
  CHECK(o.ok && o.events.size() == 4 &&
            same_instigated(o.events[1], Add, Bag, 0, fi::kRingA) &&
            same_uninstigated(o.events[2], Remove, Worn, 10, fi::kRingA) &&
            same_instigated(o.events[3], Add, Worn, 10, fi::kRingA) &&
            inv.at(Worn, 10)->instance_guid == guid_n(3) &&
            inv.at(Bag, 0)->instance_guid == guid_n(1),
        "with both ring slots taken the new ring takes the first slot of its "
        "record and the old ring goes to the bag");
  inv = inventory_of({make_item(Bag, 0, fi::kEarLeftFirst, 1),
                      make_item(Bag, 1, fi::kEarRightFirst, 2)});
  o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
  CHECK(o.ok &&
            same_instigated(o.events.back(), Add, Worn, 14, fi::kEarLeftFirst),
        "an earring whose record lists EarLeft first goes to EarLeft");
  o = inv.use(Bag, 1, guid_n(2), kAny, catalog);
  CHECK(o.ok && o.events.size() == 2 &&
            same_instigated(o.events.back(), Add, Worn, 13, fi::kEarRightFirst),
        "the next, whose record says EarRight first, finds EarRight free");
}

static void test_slot_choice_focus_and_offhand(const FakeCatalog &catalog) {
  Inv inv = inventory_of(
      {make_item(Bag, 0, fi::kFocus, 1), make_item(Worn, 16, fi::kShield, 2)});
  auto o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
  CHECK(o.ok && o.events.size() == 2 &&
            same_instigated(o.events.back(), Add, Worn, 18, fi::kFocus) &&
            inv.at(Worn, 16),
        "a Focus with a shield in Secondary goes to RangedOffHand, which is "
        "free");
  inv = inventory_of(
      {make_item(Bag, 0, fi::kFocus, 1), make_item(Worn, 17, fi::kBook, 2)});
  o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
  CHECK(o.ok && o.events.size() == 2 &&
            same_instigated(o.events.back(), Add, Worn, 16, fi::kFocus) &&
            inv.at(Worn, 17),
        "a Focus beside a worn Book takes Secondary: the Book stays");
  inv = inventory_of({make_item(Bag, 0, fi::kFocus, 1),
                      make_item(Worn, 17, fi::kBook, 2),
                      make_item(Worn, 16, fi::kShield, 3)});
  o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
  CHECK(o.ok && o.events.size() == 4 &&
            same_instigated(o.events[3], Add, Worn, 16, fi::kFocus) &&
            inv.at(Bag, 0)->instance_guid == guid_n(3) && inv.at(Worn, 17),
        "with Secondary taken and RangedOffHand covered the Focus takes "
        "Secondary and the shield goes to the bag");
}

static void test_slot_choice() {
  std::printf("the slot the server picks\n");
  const FakeCatalog catalog = fixture_catalog();
  test_slot_choice_rings_and_ears(catalog);
  test_slot_choice_focus_and_offhand(catalog);
}

static void test_two_hander_over_dual_wield(const FakeCatalog &catalog) {
  Inv inv = inventory_of(
      {make_item(Bag, 0, fi::kPotion, 10), make_item(Bag, 1, fi::kPotion, 11),
       make_item(Bag, 2, fi::kPotion, 12), make_item(Bag, 3, fi::kPotion, 13),
       make_item(Bag, 5, fi::kSword2H, 1), make_item(Worn, 15, fi::kMace1H, 2),
       make_item(Worn, 16, fi::kShield, 3)});
  auto o = inv.use(Bag, 5, guid_n(1), kAny, catalog);
  CHECK(o.ok && o.events.size() == 6,
        "a two-hander over a main hand and an off hand: six events");
  CHECK(o.events.size() == 6 &&
            same_uninstigated(o.events[0], Remove, Bag, 5, fi::kSword2H) &&
            same_instigated(o.events[1], Add, Bag, 4, fi::kMace1H) &&
            same_uninstigated(o.events[2], Remove, Worn, 15, fi::kMace1H) &&
            same_instigated(o.events[3], Add, Bag, 5, fi::kShield) &&
            same_uninstigated(o.events[4], Remove, Worn, 16, fi::kShield) &&
            same_instigated(o.events[5], Add, Worn, 15, fi::kSword2H),
        "Remove 2H, Add mace at bag slot 4, Remove mace, Add shield at bag "
        "slot 5, Remove shield, Add 2H at Primary");
  CHECK(inv.at(Worn, 15) && inv.at(Worn, 15)->item_record_id == fi::kSword2H &&
            !inv.at(Worn, 16) &&
            inv.at(Bag, 4)->item_record_id == fi::kMace1H &&
            inv.at(Bag, 5)->item_record_id == fi::kShield,
        "the two-hander is in Primary, Secondary stays empty, the two went to "
        "the bag");
  inv = inventory_of({make_item(Bag, 0, fi::kSword2H, 1),
                      make_item(Worn, 16, fi::kShield, 2)});
  o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
  CHECK(o.ok && o.events.size() == 4 &&
            same_instigated(o.events.back(), Add, Worn, 15, fi::kSword2H) &&
            !inv.at(Worn, 16),
        "a two-hander over a shield alone sits in Primary and sends the shield "
        "to the bag");
}

static void test_two_hander_shield_and_book(const FakeCatalog &catalog) {
  Inv inv = inventory_of({make_item(Bag, 0, fi::kShield, 1),
                          make_item(Worn, 15, fi::kSword2H, 2)});
  auto o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
  CHECK(o.ok && o.events.size() == 4 &&
            same_instigated(o.events[1], Add, Bag, 0, fi::kSword2H) &&
            same_uninstigated(o.events[2], Remove, Worn, 15, fi::kSword2H) &&
            same_instigated(o.events[3], Add, Worn, 16, fi::kShield) &&
            !inv.at(Worn, 15),
        "a shield into Secondary under a worn two-hander takes the two-hander "
        "off");
  inv = inventory_of({make_item(Bag, 0, fi::kBook, 1),
                      make_item(Worn, 18, fi::kFocus, 2),
                      make_item(Worn, 16, fi::kShield, 3)});
  o = inv.use(Bag, 0, guid_n(1), kAny, catalog);
  CHECK(o.ok && o.events.size() == 4 &&
            same_instigated(o.events[1], Add, Bag, 0, fi::kFocus) &&
            same_instigated(o.events[3], Add, Worn, 17, fi::kBook) &&
            inv.at(Worn, 16),
        "a Book over a Focus in RangedOffHand sends the Focus to the bag and "
        "leaves the melee set alone");
  inv = inventory_of(bard_kit());
  CHECK(inv.at(Worn, 15) && inv.at(Worn, 16) && inv.at(Worn, 17) &&
            !inv.at(Worn, 18),
        "the Bard wears Scepter, Focus and Book, and RangedOffHand is empty "
        "(the Book covers it)");
}

static void test_two_handers_displacement(const FakeCatalog &catalog) {
  test_two_hander_over_dual_wield(catalog);
  test_two_hander_shield_and_book(catalog);
}

static void test_two_handers_crowded_bag(const FakeCatalog &catalog) {
  std::vector<svc::InventoryItem> crowded;
  uint32_t n = 100;
  for (int s = 0; s < 70; ++s)
    crowded.push_back(make_item(Bag, s, fi::kPotion, ++n));
  crowded[5] = make_item(Bag, 5, fi::kSword2H, 1);
  crowded.push_back(make_item(Worn, 15, fi::kMace1H, 2));
  crowded.push_back(make_item(Worn, 16, fi::kShield, 3));
  Inv inv = inventory_of(crowded);
  const Inv before = inv;
  auto o = inv.use(Bag, 5, guid_n(1), kAny, catalog);
  CHECK(!o.ok && o.events.empty() &&
            o.refusal.find("free slot") != std::string::npos &&
            unchanged_by(before, inv),
        "a full bag with two items to take off: refused, nothing changed");
  crowded.pop_back();
  inv = inventory_of(crowded);
  o = inv.use(Bag, 5, guid_n(1), kAny, catalog);
  CHECK(o.ok && o.events.size() == 4 &&
            same_instigated(o.events[1], Add, Bag, 5, fi::kMace1H) &&
            inv.at(Bag, 5)->instance_guid == guid_n(2),
        "a full bag with one item to take off: the freed slot takes it");
  std::vector<svc::InventoryItem> full;
  for (int s = 0; s < 70; ++s)
    full.push_back(make_item(Bag, s, fi::kPotion, ++n));
  full.push_back(make_item(Worn, 2, fi::kChest, 1));
  inv = inventory_of(full);
  const Inv before2 = inv;
  o = inv.use(Worn, 2, guid_n(1), kAny, catalog);
  CHECK(!o.ok && o.refusal.find("bag is full") != std::string::npos &&
            unchanged_by(before2, inv),
        "taking off into a full bag is refused");
}

static void test_two_handers() {
  std::printf("two-handed items\n");
  const FakeCatalog catalog = fixture_catalog();
  test_two_handers_displacement(catalog);
  test_two_handers_crowded_bag(catalog);
}

static void test_visible_held_items(const Inv &bard) {
  CHECK(items::kHeldSlots.size() == 23 && items::kHeldSlots[0] == 15 &&
            items::kHeldSlots[3] == 18 && items::kHeldSlots[4] == 26 &&
            items::kHeldSlots[18] == 41 && items::kHeldSlots[19] == 75 &&
            items::kHeldSlots[22] == 78,
        "HeldItems has 23 keys: the weapons 15-18, the tools 26-39 and 41, the "
        "cosmetic weapons 75-78");
  bool keys = true;
  for (size_t i = 0; i < 23; ++i) {
    keys = keys && bs::captured::kHeldItems[i].key == items::kHeldSlots[i] &&
           bs::captured::kHeldItems[i].id == static_cast<int32_t>(i + 1);
  }
  CHECK(keys,
        "they are the captured Bard's keys with its ReplicationIDs 1 to 23");
  const auto held = items::held_records(bard);
  bool same = true;
  for (size_t i = 0; i < 23; ++i)
    same = same && held[i] == bs::captured::kHeldItems[i].item_guid;
  CHECK(same, "the Bard's kit shows the captured HeldItems: Scepter, Focus and "
              "Book, the five tools, nothing else");
}

static void test_visible_bard_appearance(const Inv &bard) {
  const auto app = items::appearance_ids(bard);
  bool app_same = app.size() == 4;
  for (size_t i = 0; i < app.size() && i < 4; ++i) {
    app_same &=
        (static_cast<int64_t>(app[i].record_guid) ==
             bs::captured::kAppearanceIds[i].record_guid &&
         static_cast<int64_t>(app[i].type_id) ==
             bs::captured::kAppearanceIds[i].type_id &&
         app[i].enchantment_level ==
             bs::captured::kAppearanceIds[i].enchantment &&
         app[i].item_equip_slot == bs::captured::kAppearanceIds[i].equip_slot);
  }
  CHECK(app_same, "and the captured AppearanceIDs: the helmet, chest, legs and "
                  "feet by slot, TypeId of an ItemRecord, enchantment 0");
  const std::array<bool, 10> got_shown = {
      items::shows_in_appearance(1),  items::shows_in_appearance(9),
      items::shows_in_appearance(71), items::shows_in_appearance(79),
      items::shows_in_appearance(0),  items::shows_in_appearance(10),
      items::shows_in_appearance(12), items::shows_in_appearance(19),
      items::shows_in_appearance(21), items::shows_in_appearance(74)};
  const std::array<bool, 10> want_shown = {true,  true,  true,  true,  false,
                                           false, false, false, false, false};
  CHECK(got_shown == want_shown,
        "armor (1-9), the costume and the cloak are shown; rings, necklace, "
        "earrings, the artisan shirt, the tool belt and the lure are not");
}

static void test_visible_cosmetic_and_empty() {
  const auto none = items::appearance_ids(inventory_of(
      {make_item(Worn, 10, fi::kRingA, 1), make_item(Worn, 19, fi::kShirt, 2),
       make_item(Worn, 23, fi::kBag, 3)}));
  CHECK(none.empty(),
        "a ring, the artisan shirt and a bag add nothing to AppearanceIDs");
  const auto cosmetic = items::appearance_ids(inventory_of(
      {make_item(Worn, 79, fi::kCloak, 1), make_item(Worn, 71, fi::kCostume, 2),
       make_item(Worn, 8, fi::kGatherCape, 3)}));
  CHECK(cosmetic.size() == 3 && cosmetic[0].item_equip_slot == 8 &&
            cosmetic[1].item_equip_slot == 71 &&
            cosmetic[2].item_equip_slot == 79,
        "armor, costume and cloak come by ascending slot");
  CHECK(items::held_records(Inv())[0] == 0 &&
            items::appearance_ids(Inv()).empty(),
        "nothing worn: all 23 are 0 and nothing is shown");
  const auto only_book =
      items::held_records(inventory_of({make_item(Worn, 17, fi::kBook, 1)}));
  CHECK(only_book[2] == fi::kBook && only_book[3] == 0,
        "a two-hander is in its own slot only: Ranged has the Book, "
        "RangedOffHand 0");
}

static void test_visible_gear() {
  std::printf("the visible gear\n");
  const Inv bard = inventory_of(bard_kit());
  test_visible_held_items(bard);
  test_visible_bard_appearance(bard);
  test_visible_cosmetic_and_empty();
}

static void test_retail_equips() {
  std::printf(
      "the retail server's answers to equip and unequip (capture_equip.h)\n");
  const FakeCatalog catalog = catalog_of_captures();
  size_t events_checked = 0;
  for (const auto &sc : capture_equip::kScenarios) {
    const std::string name =
        "capture " + std::to_string(sc.capture) + ": " + sc.title;
    Inv inv = inventory_before(sc);
    const bool ev_ok =
        run_retail_equip_requests(sc, inv, catalog, events_checked);
    CHECK(ev_ok,
          (name + ": the same events in the same order, instigators included")
              .c_str());
    const auto held = items::held_records(inv);
    bool held_same = true;
    for (size_t i = 0; i < 23; ++i) {
      held_same = held_same && held[i] == sc.held[i].record &&
                  items::kHeldSlots[i] == sc.held[i].key;
    }
    const bool app_same = check_retail_equip_appearance(sc, inv);
    CHECK(held_same && app_same,
          (name + ": HeldItems and AppearanceIDs after equal the capture's")
              .c_str());
  }
  std::printf("    %zu retail events compared\n", events_checked);
  CHECK(events_checked > 80, "more than eighty retail events compared");
  (void)&record_of;
}

void run_items_equip_tests() {
  test_equip_and_unequip();
  test_slot_choice();
  test_two_handers();
  test_visible_gear();
  test_retail_equips();
}
