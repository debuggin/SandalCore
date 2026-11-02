// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "bootstrap/captured_character.h"
#include "protocol/repl/item_codec.h"
#include "protocol/schema/generated/index.h"

#include "test_worldd_items_common.h"

using namespace aoc::worldd;
// [C O A x C.Y.B]

static void test_guids_and_plain_instances() {
  const auto g = items::guid_of("9cb881dd7fbb492eb9e6b0e19677c0f7");
  CHECK(g && g->a == 0x9cb881ddu && g->b == 0x7fbb492eu &&
            g->c == 0xb9e6b0e1u && g->d == 0x9677c0f7u,
        "the capture's first instance guid reads as its four words");
  CHECK(g && items::guid_hex(*g) == "9cb881dd7fbb492eb9e6b0e19677c0f7",
        "and writes back to the same 32 digits");
  CHECK(items::guid_of("9CB881DD7FBB492EB9E6B0E19677C0F7").has_value(),
        "capitals are read (the table keeps lowercase)");
  CHECK(
      !items::guid_of("") && !items::guid_of("abc") &&
          !items::guid_of(std::string(32, '0')) &&
          !items::guid_of(std::string(31, 'a') + "g") &&
          !items::guid_of(std::string(33, 'a')),
      "nothing else is a guid: empty, short, long, a non-digit, the zero guid");
  CHECK(items::ms_of_ticks(639068962740000000ll) == 1'771'299'474'000ll &&
            items::ticks_of_ms(1'771'299'474'000ll) == 639068962740000000ll,
        "FDateTime ticks and Unix milliseconds convert both ways (the Bard's "
        "639068962740000000)");
  svc::InventoryItem it = make_item(Worn, 2, fi::kChest, 7);
  it.durability = 11;
  const repl::ItemInstance inst = items::to_instance(it);
  repl::ItemInstance plain;
  plain.has_item = true;
  plain.record_id = static_cast<uint64_t>(fi::kChest);
  plain.quantity = 1;
  plain.quality = 1;
  plain.durability = 11;
  plain.guid = *items::guid_of(it.instance_guid);
  plain.creation_time = items::ticks_of_ms(it.created_at_ms);
  CHECK(inst == plain,
        "an item is the plain instance: record, quantity, quality, durability, "
        "guid and creation time, the rest at its default");
  const auto back = items::item_of(inst, Worn, 2);
  CHECK(back && *back == it, "and the instance gives the item back");
  CHECK(!items::item_of(repl::ItemInstance{}, Worn, 2),
        "the empty instance is no item");
  repl::ItemInstance bad = inst;
  bad.guid = {};
  CHECK(!items::item_of(bad, Worn, 2),
        "an instance with the zero guid is no item the table can hold");
  bad = inst;
  bad.quantity = 0;
  CHECK(!items::item_of(bad, Worn, 2), "nor one with quantity 0");
}

static void test_hostile_mount_instances(const repl::ItemInstance &mi,
                                         const svc::InventoryItem &mount) {
  svc::InventoryItem locked = make_item(Bag, 6, 6064634099174146054ll, 21);
  locked.lock_expiration = 1790000123;
  const repl::ItemInstance li = items::to_instance(locked);
  const auto locked_back = items::item_of(li, Bag, 6);
  CHECK(li.stats.empty() && locked_back && *locked_back == locked &&
            !locked_back->persisted_health && !locked_back->persisted_mana,
        "an item with a lock and no persisted stats gives no stats, and reads "
        "back as it was");
  CHECK(
      !(make_item(Bag, 5, 6064631121016127508ll, 20) == mount),
      "an item that keeps none differs from the mount item of the same record");
  repl::ItemInstance hostile = mi;
  hostile.stats[0].value = -1.0f;
  CHECK(!items::item_of(hostile, Bag, 5),
        "a negative persisted Health is no item the table can hold");
  hostile = mi;
  hostile.stats[1].value = std::numeric_limits<float>::quiet_NaN();
  CHECK(!items::item_of(hostile, Bag, 5), "nor a Mana that is not a number");
  hostile = mi;
  hostile.lock_expiration = -5;
  CHECK(!items::item_of(hostile, Bag, 5), "nor a negative lock expiration");
  hostile = mi;
  hostile.lock_expiration = 253402300800ll;
  CHECK(!items::item_of(hostile, Bag, 5), "nor one past the year 9999");
  hostile = mi;
  hostile.stats.push_back({6064630000000000000ull, 5.0f});
  const auto extra = items::item_of(hostile, Bag, 5);
  CHECK(extra && *extra == mount,
        "a persisted stat that is neither Health nor Mana is ignored (the "
        "table keeps those two)");
}

static void test_mount_and_locked_instances() {
  svc::InventoryItem mount = make_item(Bag, 5, 6064631121016127508ll, 20);
  mount.persisted_health = 3572.0f;
  mount.persisted_mana = 1.0f;
  mount.lock_expiration = 1770131735;
  const repl::ItemInstance mi = items::to_instance(mount);
  CHECK(mi.stats.size() == 2 && mi.stats[0].stat_id == 108986352686712ull &&
            mi.stats[0].value == 3572.0f &&
            mi.stats[1].stat_id == 109183576135288ull &&
            mi.stats[1].value == 1.0f && mi.lock_expiration == 1770131735 &&
            mi.cooldowns.empty() && mi.custom_data.empty(),
        "a mount item is the plain instance plus its two persisted stats "
        "(Health, Mana) and its LockExpiration");
  const auto mount_back = items::item_of(mi, Bag, 5);
  CHECK(mount_back == mount && mount_back->persisted_health == 3572.0f &&
            mount_back->lock_expiration == 1770131735,
        "and the instance gives the mount item back, state and all");
  {
    aoc::protocol::emit::BunchWriter w;
    const bool wrote = aoc::protocol::repl::write_item_instance(w, mi);
    CHECK(wrote && w.bit_pos() == 1096 + 2 * 96,
          "it writes as 1,288 bits, the smallest retail mount instance's size");
  }
  svc::InventoryItem dead = mount;
  dead.persisted_health = 0.0f;
  const auto dead_back = items::item_of(items::to_instance(dead), Bag, 5);
  CHECK(dead_back && dead_back->persisted_health == 0.0f &&
            dead_back->persisted_mana.has_value() && !(*dead_back == mount),
        "a persisted Health of 0 is a Health (a dead mount), and differs from "
        "the live one");
  test_hostile_mount_instances(mi, mount);
}

static void test_guids_and_ticks() {
  std::printf("guids and times as the wire has them\n");
  test_guids_and_plain_instances();
  test_mount_and_locked_instances();
}

static void check_from_items_lookup_and_list(const Inv &inv) {
  CHECK(inv.at(Worn, 2) && inv.at(Worn, 2)->item_record_id == fi::kChest &&
            !inv.at(Worn, 3) && !inv.at(Worn, -1) && !inv.at(Worn, 83) &&
            !inv.at(Bag, 70) && !inv.at(ItemContainer::Quest, 100),
        "at() finds an item by storage and slot and nothing outside a storage");
  CHECK(inv.first_free(Bag) == 0 && inv.first_free(Worn) == 0,
        "the lowest free slot");
  const auto all = inv.items();
  CHECK(all.size() == 3 && all[0].container == Bag &&
            all[1].container == Worn &&
            all[2].container == ItemContainer::Quest,
        "items() lists the Backpack's, the Equipment's, then the Quest "
        "storage's");
}

static void test_from_items_valid() {
  std::string error;
  auto inv = Inv::from_items({make_item(Bag, 3, fi::kPotion, 1),
                              make_item(Worn, 2, fi::kChest, 2),
                              make_item(ItemContainer::Quest, 99, 9, 3)},
                             error);
  CHECK(inv && inv->size() == 3 && inv->count(Bag) == 1 &&
            inv->count(Worn) == 1 && inv->count(ItemContainer::Quest) == 1,
        "three items in three storages");
  if (inv)
    check_from_items_lookup_and_list(*inv);
  auto other = Inv::from_items({make_item(ItemContainer::Quest, 99, 9, 3),
                                make_item(Worn, 2, fi::kChest, 2),
                                make_item(Bag, 3, fi::kPotion, 1)},
                               error);
  CHECK(inv && other && *other == *inv,
        "the order of the list does not matter");
}
// C Y B - C | O | A

static void test_from_items_slot_and_full_refusals() {
  std::string err;
  CHECK(!Inv::from_items({make_item(Bag, 3, 1, 1), make_item(Bag, 3, 1, 2)},
                         err) &&
            err.find("two items in Backpack slot 3") != std::string::npos,
        "two items in a slot are refused");
  CHECK(!Inv::from_items({make_item(Bag, 3, 1, 1), make_item(Bag, 4, 1, 1)},
                         err) &&
            err.find("instance guid") != std::string::npos,
        "one guid on two items is refused");
  CHECK(Inv::from_items({make_item(Bag, 5, 1, 1), make_item(Worn, 5, 1, 2),
                         make_item(ItemContainer::Quest, 5, 1, 3)},
                        err)
            .has_value(),
        "slot 5 of three storages are three slots");
  std::vector<svc::InventoryItem> full;
  uint32_t n = 0;
  for (const ItemContainer c : {Bag, Worn, ItemContainer::Quest}) {
    for (int s = 0; s < items::slot_count(c); ++s)
      full.push_back(make_item(c, s, 1, ++n));
  }
  CHECK(full.size() == 253 && Inv::from_items(full, err).has_value() &&
            Inv::from_items(full, err)->first_free(Bag) == -1,
        "every slot of every storage can be filled");
  full.push_back(make_item(Bag, 0, 1, ++n));
  CHECK(!Inv::from_items(full, err),
        "one item more than there are slots is refused");
}

static void test_from_items_refusals() {
  struct Bad {
    const char *name;
    svc::InventoryItem it;
    const char *want;
  };
  std::vector<Bad> bad;
  auto with = [&](const char *name, const char *want, auto mod) {
    auto it = make_item(Bag, 3, fi::kPotion, 1);
    mod(it);
    bad.push_back({name, it, want});
  };
  with("a bag slot 70", "slot 70", [](auto &it) { it.slot = 70; });
  with("a negative slot", "slot -1", [](auto &it) { it.slot = -1; });
  with("an equipment slot 83", "slot 83", [](auto &it) {
    it.container = Worn;
    it.slot = 83;
  });
  with("a quest slot 100", "slot 100", [](auto &it) {
    it.container = ItemContainer::Quest;
    it.slot = 100;
  });
  with("a storage that is none", "storage",
       [](auto &it) { it.container = static_cast<ItemContainer>(9); });
  with("record 0", "record", [](auto &it) { it.item_record_id = 0; });
  with("a guid in capitals", "lowercase", [](auto &it) {
    for (auto &c : it.instance_guid)
      c = static_cast<char>(std::toupper(c));
  });
  with("a short guid", "guid", [](auto &it) { it.instance_guid = "abc"; });
  with("the zero guid", "guid",
       [](auto &it) { it.instance_guid = std::string(32, '0'); });
  with("quantity 0", "quantity", [](auto &it) { it.quantity = 0; });
  with("quantity past a million", "quantity",
       [](auto &it) { it.quantity = 1000001; });
  with("quality 0", "quality", [](auto &it) { it.quality = 0; });
  with("quality 201", "quality", [](auto &it) { it.quality = 201; });
  with("negative durability", "durability",
       [](auto &it) { it.durability = -1; });
  with("NaN durability", "durability",
       [](auto &it) { it.durability = std::nanf(""); });
  with("infinite durability", "durability",
       [](auto &it) { it.durability = INFINITY; });
  with("a negative creation time", "creation time",
       [](auto &it) { it.created_at_ms = -1; });
  with("a creation time past the year 2255", "creation time",
       [](auto &it) { it.created_at_ms = 9'000'000'000'001ll; });
  for (const Bad &b : bad) {
    std::string err;
    const auto r = Inv::from_items({b.it}, err);
    CHECK(!r && err.find(b.want) != std::string::npos,
          (std::string(b.name) + " is refused, saying why").c_str());
  }
  test_from_items_slot_and_full_refusals();
}

static void test_from_items() {
  std::printf("building an inventory from the character's items\n");
  test_from_items_valid();
  test_from_items_refusals();
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
// built with C | Y | B and C O A

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

static void test_refusals() {
  std::printf("what is refused changes nothing\n");
  const FakeCatalog catalog = fixture_catalog();
  Inv inv = inventory_of(
      {make_item(Bag, 3, fi::kHeavyChest, 1), make_item(Bag, 4, fi::kPotion, 2),
       make_item(Bag, 5, 424242, 3), make_item(Worn, 2, fi::kChest, 4),
       make_item(ItemContainer::Quest, 0, fi::kHeavyChest, 5)});
  const Inv before = inv;
  struct Case {
    const char *name;
    ItemContainer storage;
    int slot;
    std::string guid;
    uint32_t requested;
    const char *why;
  };
  const Case cases[] = {
      {"an empty slot", Bag, 9, guid_n(1), kAny, "no item in Backpack slot 9"},
      {"another instance's guid", Bag, 3, guid_n(2), kAny,
       "not the instance the client named"},
      {"the zero guid", Bag, 3, std::string(32, '0'), kAny, "not the instance"},
      {"a guid that is no guid", Bag, 3, "xyz", kAny, "not the instance"},
      {"an empty guid", Bag, 3, "", kAny, "not the instance"},
      {"a negative slot", Bag, -1, guid_n(1), kAny, "no item"},
      {"a slot past the bag", Bag, 70, guid_n(1), kAny, "no item"},
      {"a slot past the equipment", Worn, 83, guid_n(4), kAny, "no item"},
      {"an item that is not worn", Bag, 4, guid_n(2), kAny,
       "is not in content.db"},
      {"a record content.db does not know", Bag, 5, guid_n(3), kAny,
       "is not in content.db"},
      {"the Quest storage", ItemContainer::Quest, 0, guid_n(5), kAny,
       "Quest storage are not used"},
      {"a storage that is none", static_cast<ItemContainer>(7), 0, guid_n(5),
       kAny, "unknown storage"},
  };
  for (const Case &c : cases) {
    const items::Outcome o =
        inv.use(c.storage, c.slot, c.guid, c.requested, catalog);
    CHECK(!o.ok && o.events.empty() && !o.equipment_changed &&
              o.refusal.find(c.why) != std::string::npos &&
              unchanged_by(before, inv),
          (std::string(c.name) + ": refused (" + o.refusal + ")").c_str());
  }
  FakeCatalog plain = fixture_catalog();
  plain.add(fi::kPotion, {});
  const items::Outcome o = inv.use(Bag, 4, guid_n(2), kAny, plain);
  CHECK(!o.ok && o.refusal.find("not worn in any slot") != std::string::npos &&
            unchanged_by(before, inv),
        "a record that lists no slot is not worn");
}
// C Y B x CoA

static void test_move_and_swap_refusals(Inv &inv, const Inv &before) {
  struct Case {
    const char *name;
    items::Outcome o;
  };
  const Case cases[] = {
      {"a move onto an occupied slot",
       inv.move(Bag, 32, 1, guid_n(1), Bag, 55)},
      {"a move onto itself", inv.move(Bag, 32, 1, guid_n(1), Bag, 32)},
      {"a move from an empty slot", inv.move(Bag, 9, 1, guid_n(1), Bag, 10)},
      {"a move to a slot past the bag",
       inv.move(Bag, 32, 1, guid_n(1), Bag, 70)},
      {"a move to a negative slot", inv.move(Bag, 32, 1, guid_n(1), Bag, -1)},
      {"a move of another instance", inv.move(Bag, 32, 1, guid_n(9), Bag, 10)},
      {"a split", inv.move(Bag, 55, 5, guid_n(2), Bag, 10)},
      {"a count of 0", inv.move(Bag, 32, 0, guid_n(1), Bag, 10)},
      {"a move out of the bag", inv.move(Bag, 32, 1, guid_n(1), Worn, 10)},
      {"a move from the equipment", inv.move(Worn, 2, 1, guid_n(4), Bag, 10)},
      {"a swap with an empty slot",
       inv.swap(Bag, 32, 1, guid_n(1), Bag, 10, guid_n(3), 1)},
      {"a swap with itself",
       inv.swap(Bag, 32, 1, guid_n(1), Bag, 32, guid_n(1), 1)},
      {"a swap naming the wrong instance",
       inv.swap(Bag, 32, 1, guid_n(1), Bag, 55, guid_n(9), 20)},
      {"a swap naming the wrong count",
       inv.swap(Bag, 32, 1, guid_n(1), Bag, 55, guid_n(2), 3)},
      {"a swap of half a stack",
       inv.swap(Bag, 55, 10, guid_n(2), Bag, 32, guid_n(1), 1)},
      {"a swap into the equipment",
       inv.swap(Bag, 32, 1, guid_n(1), Worn, 2, guid_n(4), 1)},
  };
  for (const Case &c : cases) {
    CHECK(!c.o.ok && c.o.events.empty() && !c.o.refusal.empty() &&
              unchanged_by(before, inv),
          (std::string(c.name) + " is refused").c_str());
  }
}

static void test_move_in_bag(Inv &inv) {
  auto o = inv.move(Bag, 40, 1, guid_n(1), Bag, 32);
  CHECK(o.ok && o.events.size() == 2 &&
            same_instigated(o.events[0], Remove, Bag, 40, fi::kPotion) &&
            same_instigated(o.events[1], Add, Bag, 32, fi::kPotion) &&
            !o.equipment_changed && inv.at(Bag, 32) && !inv.at(Bag, 40),
        "a move is Remove, then Add, both instigated by the character");
  CHECK(inv.at(Bag, 32)->slot == 32 &&
            inv.at(Bag, 32)->instance_guid == guid_n(1),
        "the item is in its new slot");
  o = inv.move(Bag, 62, 20, guid_n(2), Bag, 63);
  CHECK(o.ok && o.events[1].item.quantity == 20 &&
            inv.at(Bag, 63)->quantity == 20,
        "a stack of 20 moves whole");
}

static void test_swap_in_bag(Inv &inv) {
  const auto o = inv.swap(Bag, 63, 20, guid_n(2), Bag, 55, guid_n(3), 1);
  CHECK(o.ok && o.events.size() == 4 &&
            same_instigated(o.events[0], Remove, Bag, 63, fi::kPotion) &&
            same_instigated(o.events[1], Remove, Bag, 55, fi::kRingA) &&
            same_instigated(o.events[2], Add, Bag, 63, fi::kRingA) &&
            same_instigated(o.events[3], Add, Bag, 55, fi::kPotion),
        "a swap is Remove A, Remove B, Add B where A was, Add A where B was "
        "(the retail capture's pair)");
  CHECK(inv.at(Bag, 63)->instance_guid == guid_n(3) &&
            inv.at(Bag, 55)->instance_guid == guid_n(2) &&
            inv.at(Bag, 55)->quantity == 20,
        "and the two changed places with their quantities");
}

static void test_move_and_swap() {
  std::printf("moving and swapping inside the bag\n");
  Inv inv = inventory_of({make_item(Bag, 40, fi::kPotion, 1, 1),
                          make_item(Bag, 62, fi::kPotion, 2, 20),
                          make_item(Bag, 55, fi::kRingA, 3, 1),
                          make_item(Worn, 2, fi::kChest, 4)});
  test_move_in_bag(inv);
  test_swap_in_bag(inv);
  const Inv before = inv;
  test_move_and_swap_refusals(inv, before);
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
}

int main() {
  test_guids_and_ticks();
  test_from_items();
  test_equip_and_unequip();
  test_slot_choice();
  test_two_handers();
  test_refusals();
  test_move_and_swap();
  test_visible_gear();
  test_retail_equips();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
