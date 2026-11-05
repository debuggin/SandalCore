// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_items_session_common.h"
using namespace aoc::worldd;
static BunchWriter with_bits(const BunchWriter &src, size_t bits) {
    BunchWriter w;
    if (bits > 0) w.write_bit_range(src.data(), 0, std::min(bits, src.bit_pos()));
    return w;
}
static BunchWriter flipped_bits(const BunchWriter &src, const std::vector<size_t> &at) {
    std::vector<uint8_t> bytes(src.data(), src.data() + src.byte_size());
    for (const size_t b : at)
        if (b / 8 < bytes.size()) bytes[b / 8] ^= static_cast<uint8_t>(1u << (b % 8));
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, src.bit_pos());
    return w;
}
static bool inventory_is_sane(const items::Inventory &inv, const std::set<std::string> &guids, size_t destroyed) {
    std::string error;
    const auto again = items::Inventory::from_items(inv.items(), error);
    if (!again || !(*again == inv)) return false;
    std::set<std::string> now;
    for (const auto &it : inv.items()) now.insert(it.instance_guid);
    if (inv.size() != now.size() || !std::includes(guids.begin(), guids.end(), now.begin(), now.end())) return false;
    return guids.size() - now.size() <= destroyed;
}
static bool same_items(std::vector<svc::InventoryItem> a, std::vector<svc::InventoryItem> b) {
    const auto less = [](const svc::InventoryItem &x, const svc::InventoryItem &y) { return std::make_pair(static_cast<int>(x.container), x.slot) < std::make_pair(static_cast<int>(y.container), y.slot); };
    std::sort(a.begin(), a.end(), less);
    std::sort(b.begin(), b.end(), less);
    return a == b;
}
static BunchWriter bag_swap_potions_and_heavy_chest(Harness &h) {
    return bag_move_or_swap(h, repl::StorageEventType::Swap, {32, 5}, {guid_n(201), guid_n(203)}, {3, 1});
}
static MoveSpec potion_move(const Harness &h) {
    MoveSpec m;
    m.from_object = h.s->bootstrap().setup().guids.backpack;
    m.to_object = m.from_object;
    return m;
}
static bool refused_cleanly(Harness &h, const items::Inventory &before, const char *what, const BunchWriter &bunch, uint32_t channel) {
    h.step(milliseconds(150));
    const size_t refused = h.s->item_requests_refused(), applied = h.s->item_requests_applied(), events = h.mirror.events.size();
    h.request(bunch, channel);
    const bool ok =
        bunch.bit_pos() > 0 && h.s->item_requests_refused() == refused + 1 && h.s->item_requests_applied() == applied && h.mirror.events.size() == events && h.s->inventory() == before && !h.s->inventory_dirty() && !h.s->inventory_changed();
    if (!ok) std::printf("    not refused cleanly: %s\n", what);
    return ok;
}
static svc::Character cyb_with_a_full_bag() {
    svc::Character c = cyb_record();
    std::vector<svc::InventoryItem> items;
    for (const auto &it : c.items)
        if (it.container != Bag) items.push_back(it);
    items.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    for (int slot = 1; slot < 70; ++slot) items.push_back(make_item(Bag, slot, fi::kPotion, 300 + slot, 1 + slot % 7));
    c.items = items;
    return c;
}
static svc::Position pos_of(double x) {
    return {x, 0, 0, 0};
}
static std::vector<svc::InventoryItem> first_items(size_t n) {
    auto all = bard_items();
    all.resize(std::min(n, all.size()));
    return all;
}
struct HostileFuzzer {
    Harness h;
    std::mt19937_64 rng{0xB2B2B2B2ull};
    std::set<std::string> guids;
    size_t applied_by_use = 0, applied_by_move = 0;
    HostileFuzzer() {
        for (const auto &it : h.s->inventory().items()) guids.insert(it.instance_guid);
    }
    size_t pick(uint64_t n) { return static_cast<size_t>(rng() % n); }
    int number() {
        static const int64_t extremes[] = {0, 1, -1, 2, 3, 69, 70, 71, 82, 83, 99, 100, 255, 256, 65535, INT32_MAX, INT32_MIN, INT32_MAX - 1};
        if (pick(3) == 0) return static_cast<int>(extremes[pick(sizeof extremes / sizeof extremes[0])]);
        return static_cast<int>(pick(90));
    }
    std::string some_guid(const std::vector<svc::InventoryItem> &items) {
        if (!items.empty() && pick(3) != 0) return items[pick(items.size())].instance_guid;
        return guid_n(static_cast<uint32_t>(900 + pick(40)));
    }
    void step_use(const std::vector<svc::InventoryItem> &items, size_t applied) {
        const svc::InventoryItem *it = items.empty() ? nullptr : &items[pick(items.size())];
        const bool real = it && pick(4) != 0;
        const int storage = real ? (it->container == Bag ? 3 : it->container == Worn ? 4 : 18) : static_cast<int>(pick(4) == 0 ? number() & 0xFF : (pick(2) ? 3 : 4));
        const int slot = real ? it->slot : number();
        const std::string guid = real && pick(5) != 0 ? it->instance_guid : some_guid(items);
        const uint32_t requested = pick(3) == 0 ? static_cast<uint32_t>(number()) : kAny;
        h.client_sends({h.client.actor(h.pawn_ch, item_used(storage, slot, guid, requested), true)});
        if (h.s->item_requests_applied() > applied) ++applied_by_use;
    }
    void step_move_source(MoveSpec &m, const std::vector<svc::InventoryItem> &items) {
        const svc::InventoryItem *it = items.empty() ? nullptr : &items[pick(items.size())];
        const bool real = it && pick(4) != 0;
        m.from_slot = real ? it->slot : number();
        m.from_count = real && pick(5) != 0 ? it->quantity : number();
        m.source_guid = m.event_guid = real && pick(6) != 0 ? it->instance_guid : some_guid(items);
        if (pick(10) == 0) m.event_guid = some_guid(items);
    }
    void step_move_target(MoveSpec &m, const std::vector<svc::InventoryItem> &items) {
        m.to_slot = pick(2) ? static_cast<int>(pick(40)) : number();
        const svc::InventoryItem *other = items.empty() ? nullptr : &items[pick(items.size())];
        if (other && pick(2)) {
            m.to_slot = other->slot;
            m.target_guid = pick(4) ? other->instance_guid : some_guid(items);
            m.to_count = pick(4) ? other->quantity : number();
        }
        m.has_source = pick(20) != 0;
        m.targets = pick(25) == 0 ? pick(3) : 1;
    }
    void step_move(const std::vector<svc::InventoryItem> &items, size_t applied) {
        const bs::SessionGuids &g = h.s->bootstrap().setup().guids;
        MoveSpec m;
        m.from_object = m.to_object = g.backpack;
        if (pick(10) == 0) m.to_object = pick(2) ? g.equipment : g.quest_storage;
        if (pick(12) == 0) m.from_object = g.equipment;
        m.type = pick(3) == 0 ? static_cast<uint8_t>(pick(8)) : static_cast<uint8_t>(pick(2) ? 2 : 4);
        step_move_source(m, items);
        step_move_target(m, items);
        h.client_sends({h.client.actor(h.pc_ch, storage_move(m), true)});
        if (h.s->item_requests_applied() > applied) ++applied_by_move;
    }
    BunchWriter corrupt_bunch(const BunchWriter &base) {
        BunchWriter bad;
        switch (pick(3)) {
        case 0:
            return with_bits(base, pick(base.bit_pos() + 1));
        case 1: {
            std::vector<size_t> at;
            for (size_t k = 0, n = 1 + pick(3); k < n; ++k) at.push_back(pick(std::max<size_t>(1, base.bit_pos())));
            return flipped_bits(base, at);
        }
        default: {
            for (size_t k = 0, n = pick(300); k < n; ++k) bad.write_bit(pick(2) != 0);
            return bad;
        }
        }
    }
    void step_damaged(const std::vector<svc::InventoryItem> &items) {
        BunchWriter base;
        switch (pick(3)) {
        case 0:
            base = item_used(pick(2) ? 3 : 4, static_cast<int>(pick(4)), some_guid(items));
            break;
        case 1:
            base = item_activate(pick(2) ? 3 : 4, static_cast<int>(pick(4)), some_guid(items));
            break;
        default:
            base = bag_move(h, 1, 5, guid_n(201), static_cast<int>(pick(40)));
            break;
        }
        const uint32_t channel = base.bit_pos() > 0 && pick(2) ? h.pawn_ch : h.pc_ch;
        const BunchWriter bad = corrupt_bunch(base);
        h.client_sends({h.client.actor(channel, bad, pick(2) != 0)});
    }
};
static void test_join() {
    std::printf("the join: the client has the character's storages and gear\n");
    Harness h;
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
    CHECK(h.s->inventory().size() == 17 && h.s->inventory().count(Worn) == 13 && h.s->inventory().count(Bag) == 4, "the session plays 17 items: 13 worn, 4 in the bag");
    CHECK(mirror_matches(h), "the client's storages are the inventory: every item as the instance the server sent, nothing more");
    CHECK(gear_matches(h), "HeldItems and AppearanceIDs show what is worn: the Scepter, Focus and Book, the five tools; the helmet, chest, legs and feet");
    CHECK(h.mirror.held_updates.size() >= 1 && h.mirror.held.size() == 23, "HeldItems arrived with its 23 items");
    CHECK(h.mirror.events.size() == 17 && h.mirror.payloads >= 3, "17 ClientReceiveStorageEvent (an Add for each item) and the storages' payloads");
    bool adds = true;
    for (const auto &e : h.mirror.events) adds = adds && e.type == static_cast<uint8_t>(repl::StorageEventType::Add) && !(e.instigator == repl::GuidValue{});
    CHECK(adds, "all Adds, the character as the instigator");
    CHECK(!h.s->inventory_changed() && !h.s->inventory_dirty() && h.s->item_requests_applied() == 0, "nothing has changed yet");
}
static void verify_unequip_chest_events(const Harness &h, size_t events_before) {
    if (h.mirror.events.size() >= events_before + 2) {
        const auto &add = h.mirror.events[events_before];
        const auto &remove = h.mirror.events[events_before + 1];
        const auto &g = h.s->bootstrap().setup().guids;
        CHECK(add.type == 0 && add.targets[0].object == g.backpack && add.targets[0].slots[0].slot == 4 && add.item.record_id == static_cast<uint64_t>(fi::kChest) && !(add.instigator == repl::GuidValue{}),
              "an Add to the lowest free bag slot (4), the character as the instigator");
        CHECK(remove.type == 1 && remove.targets[0].object == g.equipment && remove.targets[0].slots[0].slot == 2 && remove.instigator == repl::GuidValue{} && items::guid_hex(remove.item.guid) == guid_n(102),
              "then a Remove from Chest, with no instigator: as the retail server's");
    }
}
static void verify_unequip_chest_updates(const Harness &h, size_t from, size_t held_before, int key_before, size_t stat_updates_before) {
    CHECK(h.mirror.held_updates.size() == held_before + 1 && h.mirror.held_updates.back().second == 23 && h.mirror.held_key == key_before + 23, "one HeldItems update with all 23 items and the array key 23 higher");
    CHECK(h.mirror.appearance_updates >= 1 && h.mirror.appearance.size() == 3, "AppearanceIDs now lists three pieces: the chest is gone from the look");
    CHECK(h.mirror.stat_updates.size() == stat_updates_before + 1, "the stats the chest piece changes are sent: one update of the StatsComponent (plan B3)");
    size_t pc_packet = 0, pawn_packet = 0;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel == h.pc_ch && h.got[i].reliable) pc_packet = h.packet_of[i];
        if (h.got[i].channel == h.pawn_ch && h.got[i].reliable) pawn_packet = h.packet_of[i];
    }
    CHECK(pc_packet != 0 && pawn_packet >= pc_packet && pawn_packet <= pc_packet + 1, "the storage events and the pawn's update leave in the same flush: one packet or two in a row");
}
static void verify_unequip_chest_final_state(const Harness &h) {
    CHECK(!h.mirror.stat_updates.empty() && !h.mirror.held_chains.empty() && h.mirror.stat_updates.back().chain == h.mirror.held_chains.back(), "...and the stats' deltas are in the one chain that carries the HeldItems and AppearanceIDs");
    CHECK(mirror_matches(h) && gear_matches(h), "the client's view is the inventory's, storages and gear");
    CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->item_record_id == fi::kChest && !h.s->inventory().at(Worn, 2), "the chest is in the bag");
    CHECK(h.s->inventory_dirty() && h.s->inventory_changed() && h.s->item_requests_applied() == 1, "the inventory is marked changed");
}
static void test_unequip_and_equip_the_chest() {
    std::printf("taking the chest piece off and putting it on again\n");
    Harness h;
    const std::vector<repl::AppearanceId> look = items::appearance_ids(h.s->inventory());
    const size_t from = h.got.size();
    const size_t events_before = h.mirror.events.size();
    const size_t held_before = h.mirror.held_updates.size();
    const int key_before = h.mirror.held_key;
    const size_t stat_updates_before = h.mirror.stat_updates.size();
    h.request_both(item_used(4, 2, guid_n(102)), item_activate(4, 2, guid_n(102)));
    CHECK(h.mirror.events.size() == events_before + 2, "two ClientReceiveStorageEvent (ServerActivateItem is not answered a second time)");
    verify_unequip_chest_events(h, events_before);
    verify_unequip_chest_updates(h, from, held_before, key_before, stat_updates_before);
    verify_unequip_chest_final_state(h);
    h.request_both(item_used(3, 4, guid_n(102)), item_activate(3, 4, guid_n(102)));
    CHECK(h.s->inventory().at(Worn, 2) && !h.s->inventory().at(Bag, 4), "the chest is worn again, in Chest");
    CHECK(mirror_matches(h) && gear_matches(h), "the client's view is the inventory's again");
    bool same_look = h.mirror.appearance.size() == look.size();
    for (size_t i = 0; same_look && i < look.size(); ++i) same_look = h.mirror.appearance[i] && *h.mirror.appearance[i] == look[i];
    CHECK(same_look && h.mirror.held_key == key_before + 46, "AppearanceIDs are the original four again, and the HeldItems key moved 23 more");
}
static void test_two_hander_and_slots() {
    std::printf("a two-hander over a main hand and an off hand; rings\n");
    Harness h;
    const size_t events_before = h.mirror.events.size();
    const int key_before = h.mirror.held_key;
    h.request(item_used(3, 0, guid_n(200)), h.pawn_ch);
    CHECK(h.mirror.events.size() == events_before + 6, "six events: the new item out of the bag, and the Scepter and the Focus each added to the bag and taken off");
    CHECK(h.s->inventory().at(Worn, 15) && h.s->inventory().at(Worn, 15)->item_record_id == fi::kSword2H && !h.s->inventory().at(Worn, 16) && h.s->inventory().at(Worn, 17), "the sword is in Primary, Secondary is empty, the Book stays");
    CHECK(h.mirror.held_key == key_before + 69, "the HeldItems key moved by 69: three changes of the Equipment storage");
    CHECK(mirror_matches(h) && gear_matches(h), "the client's view is the inventory's");
    CHECK(h.mirror.held_by_key()[15] == fi::kSword2H && h.mirror.held_by_key()[16] == 0, "HeldItems: the sword in Primary, nothing in Secondary");
    h.request(item_used(3, 2, guid_n(202)), h.pawn_ch);
    CHECK(h.s->inventory().at(Worn, 10) && h.s->inventory().at(Worn, 10)->instance_guid == guid_n(202), "a ring goes to RingRight");
    CHECK(mirror_matches(h) && gear_matches(h), "and the client agrees; a ring changes no AppearanceIDs");
}
static void test_moves_and_swaps() {
    std::printf("moving and swapping inside the bag\n");
    Harness h;
    const size_t events_before = h.mirror.events.size();
    const size_t held_updates = h.mirror.held_updates.size();
    h.request(bag_move(h, 1, 5, guid_n(201), 32), h.pc_ch);
    CHECK(h.mirror.events.size() == events_before + 2 && h.mirror.events[events_before].type == 1 && h.mirror.events[events_before + 1].type == 0 && h.mirror.events[events_before + 1].targets[0].slots[0].slot == 32 &&
              h.mirror.events[events_before + 1].targets[0].slots[0].count == 5,
          "a move is a Remove and an Add, the stack's 5 kept");
    bool both_instigated = !(h.mirror.events[events_before].instigator == repl::GuidValue{}) && !(h.mirror.events[events_before + 1].instigator == repl::GuidValue{});
    CHECK(both_instigated, "both events carry the character as instigator, as the retail moves'");
    CHECK(h.s->inventory().at(Bag, 32) && !h.s->inventory().at(Bag, 1) && mirror_matches(h), "the item is in slot 32 and the client agrees");
    CHECK(h.mirror.held_updates.size() == held_updates, "the pawn's gear is not touched by a move in the bag");
    const size_t before_swap = h.mirror.events.size();
    h.request(bag_swap_potions_and_heavy_chest(h), h.pc_ch);
    CHECK(h.mirror.events.size() == before_swap + 4, "a swap is four events");
    const auto types = [&] {
        std::vector<int> v;
        for (size_t i = before_swap; i < h.mirror.events.size(); ++i) v.push_back(h.mirror.events[i].type);
        return v;
    }();
    CHECK(types == std::vector<int>({1, 1, 0, 0}), "Remove, Remove, Add, Add");
    CHECK(h.s->inventory().at(Bag, 3) && h.s->inventory().at(Bag, 3)->instance_guid == guid_n(201) && h.s->inventory().at(Bag, 32)->instance_guid == guid_n(203) && mirror_matches(h), "the two changed places and the client agrees");
}
struct RefusalRun {
    Harness h;
    items::Inventory before = h.s->inventory();
    size_t payloads_before = h.mirror.payloads;
    size_t held_before = h.mirror.held_updates.size();
    size_t stats_before = h.mirror.stat_blocks;
    int bad = 0, n = 0;
    void use(const char *what, int storage, int slot, const std::string &guid, uint32_t req = kAny) {
        ++n;
        bad += refused_cleanly(h, before, what, item_used(storage, slot, guid, req), h.pawn_ch) ? 0 : 1;
    }
    void move(const char *what, const MoveSpec &m) {
        ++n;
        bad += refused_cleanly(h, before, what, storage_move(m), h.pc_ch) ? 0 : 1;
    }
};
static void test_refusals_use(RefusalRun &r) {
    r.use("an empty bag slot", 3, 50, guid_n(900));
    r.use("a bag slot with another instance's guid", 3, 0, guid_n(999));
    r.use("the zero guid", 3, 0, "00000000000000000000000000000000");
    r.use("a potion (not wearable)", 3, 1, guid_n(201));
    r.use("the quest storage", 18, 0, guid_n(900));
    r.use("storage 0", 0, 0, guid_n(200));
    r.use("storage 255", 255, 0, guid_n(200));
    r.use("the vanity storage", 5, 0, guid_n(200));
    r.use("bag slot -1", 3, -1, guid_n(200));
    r.use("bag slot 70", 3, 70, guid_n(200));
    r.use("bag slot INT32_MAX", 3, INT32_MAX, guid_n(200));
    r.use("bag slot INT32_MIN", 3, INT32_MIN, guid_n(200));
    r.use("equipment slot 83", 4, 83, guid_n(102));
    r.use("equipment slot 2 with a bag item's guid", 4, 2, guid_n(200));
    r.use("an empty equipment slot", 4, 3, guid_n(102));
    r.use("a slot the item cannot be worn in (the two-hander into Chest)", 3, 0, guid_n(200), 2);
    r.use("requested slot 0", 3, 0, guid_n(200), 0);
    r.use("requested slot INT32_MAX", 3, 0, guid_n(200), 0x7FFFFFFFu);
}
static void test_refusals_move_bounds(RefusalRun &r) {
    Harness &h = r.h;
    const auto move = [&](const char *what, const MoveSpec &m) { r.move(what, m); };
    {
        MoveSpec m = potion_move(h);
        m.to_slot = 2;
        move("a Move onto an occupied slot", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 0;
        move("an Add", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 1;
        move("a Remove", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 3;
        move("a Fill", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 4;
        move("a Swap onto an empty slot", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_count = 2;
        move("a partial stack (a split)", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_count = 9;
        move("more than the stack holds", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.source_guid = m.event_guid = guid_n(998);
        move("an instance that is not in the source slot", m);
    }
}
static void test_refusals_move_slots(RefusalRun &r) {
    Harness &h = r.h;
    const auto move = [&](const char *what, const MoveSpec &m) { r.move(what, m); };
    {
        MoveSpec m = potion_move(h);
        m.event_guid = guid_n(200);
        move("an event whose item is not the source slot's", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_slot = 60;
        move("an empty source slot", m);
    }
    for (const int slot : {-1, 70, 71, INT32_MAX, INT32_MIN}) {
        MoveSpec m = potion_move(h);
        m.to_slot = slot;
        move("a target slot outside the bag", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.to_object = h.s->bootstrap().setup().guids.equipment;
        move("a move into the Equipment storage", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_object = h.s->bootstrap().setup().guids.equipment;
        m.from_slot = 2;
        m.source_guid = m.event_guid = guid_n(102);
        m.from_count = 1;
        move("a move out of the Equipment storage", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.from_object = m.to_object = h.s->bootstrap().setup().guids.quest_storage;
        move("a move in the quest storage", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.to_object = emit::FIntrepidNetworkGUID{777777, 60, 5};
        move("a container that is no storage of the character", m);
    }
}
static void test_refusals_move_stacking(RefusalRun &r) {
    Harness &h = r.h;
    const auto move = [&](const char *what, const MoveSpec &m) { r.move(what, m); };
    {
        MoveSpec m = potion_move(h);
        m.from_object = emit::FIntrepidNetworkGUID{};
        move("a source container that is the null object", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.has_source = false;
        move("a move with no source slot", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.targets = 2;
        move("a move with two targets", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.targets = 0;
        move("a move with no target", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 4;
        m.to_slot = 3;
        m.target_guid = guid_n(998);
        m.to_count = 1;
        move("a Swap with a wrong guid for the target", m);
    }
    {
        MoveSpec m = potion_move(h);
        m.type = 4;
        m.to_slot = 3;
        m.target_guid = guid_n(203);
        m.to_count = 4;
        move("a Swap with a wrong count for the target", m);
    }
}
static void test_refusals() {
    std::printf("refusals: nothing changes, only the storages' payloads go out, and only a few times\n");
    RefusalRun r;
    test_refusals_use(r);
    test_refusals_move_bounds(r);
    test_refusals_move_slots(r);
    test_refusals_move_stacking(r);
    Harness &h = r.h;
    const int bad = r.bad, n = r.n;
    const size_t payloads_before = r.payloads_before, held_before = r.held_before, stats_before = r.stats_before;
    CHECK(bad == 0, (std::to_string(n) + " requests of every kind: each is refused, counted, sends no storage event and leaves the inventory and the saving flags as they were").c_str());
    CHECK(h.mirror.held_updates.size() == held_before && h.mirror.stat_blocks == stats_before, "the pawn's gear and stats are not touched by refusals");
    CHECK(h.mirror.payloads > payloads_before, "the client's storages were sent again (the correction)");
    CHECK(mirror_matches(h) && gear_matches(h) && h.mirror.unreadable == 0 && !h.s->failed(), "and the client's view is still the inventory's; the session is up");
    h.s->finish("test");
    CHECK(h.sink.inventories.empty(), "a session whose requests were all refused saves no inventory");
}
static void test_refusals_that_need_a_state() {
    std::printf("requests that depend on the inventory\n");
    {
        Harness h(cyb_with_a_full_bag());
        const items::Inventory before = h.s->inventory();
        CHECK(h.s->inventory().count(Bag) == 70 && h.s->inventory().first_free(Bag) == -1, "(the bag is full)");
        int bad = 0;
        bad += refused_cleanly(h, before, "unequip into a full bag", item_used(4, 2, guid_n(102)), h.pawn_ch) ? 0 : 1;
        bad += refused_cleanly(h, before, "a two-hander displacing two items", item_used(3, 0, guid_n(200)), h.pawn_ch) ? 0 : 1;
        CHECK(bad == 0 && mirror_matches(h) && gear_matches(h), "a full bag: both are refused, nothing changes, the client is told where everything is");
    }
    {
        svc::Character c = cyb_record();
        c.items.push_back(make_item(Bag, 10, 123456789ll, 400));
        Harness h(c);
        const items::Inventory before = h.s->inventory();
        CHECK(refused_cleanly(h, before, "an item of an unknown record", item_used(3, 10, guid_n(400)), h.pawn_ch), "an item whose record content.db does not know is not worn");
    }
}
static void test_budgets_refusals() {
    {
        Harness h;
        const size_t payloads = h.mirror.payloads;
        std::vector<fake::Bunch> bunches;
        for (int i = 0; i < 12; ++i) bunches.push_back(h.client.actor(h.pawn_ch, item_used(3, 50, guid_n(900 + i)), true));
        h.client_sends(std::move(bunches));
        h.settle();
        CHECK(h.s->item_requests_refused() == 8 && h.s->requests_dropped() == 4, "12 requests in an instant: 8 refused, 4 dropped for the budget and counted");
        CHECK(h.mirror.payloads == payloads + 12, "and the client was corrected four times (a Backpack, an Equipment and, since Gathering stage 3, a Resource0 payload each): the resync budget");
        h.step(milliseconds(2100));
        h.request(item_used(3, 50, guid_n(900)), h.pawn_ch);
        CHECK(h.mirror.payloads == payloads + 15 && h.s->item_requests_refused() == 9, "two seconds later a refusal is corrected again, once");
        h.request(item_used(3, 50, guid_n(901)), h.pawn_ch);
        CHECK(h.mirror.payloads == payloads + 15 && h.s->item_requests_refused() == 10, "(and not twice in a row)");
        CHECK(mirror_matches(h) && gear_matches(h), "the client ends up with the inventory");
    }
}
static void test_budgets_chest_unequip() {
    {
        Harness h;
        std::vector<fake::Bunch> bunches;
        for (int i = 0; i < 12; ++i) bunches.push_back(h.client.actor(h.pawn_ch, item_used(4, 2, guid_n(102)), true));
        h.client_sends(std::move(bunches));
        h.settle();
        CHECK(h.s->item_requests_applied() == 1 && h.s->item_requests_refused() == 7 && h.s->requests_dropped() == 4, "the chest taken off twelve times at once: applied once, refused seven times (it is not there), four dropped");
        CHECK(h.s->inventory().at(Bag, 4) && h.s->inventory().at(Bag, 4)->item_record_id == fi::kChest && !h.s->inventory().at(Worn, 2) && h.s->inventory().count(Bag) == 5, "the inventory holds the chest in the bag once");
        CHECK(mirror_matches(h) && gear_matches(h) && h.mirror.unreadable == 0, "and the client agrees");
        h.step(seconds(1));
        h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
        CHECK(h.s->item_requests_applied() == 2 && h.s->inventory().at(Worn, 2) && mirror_matches(h) && gear_matches(h), "a second later the chest goes on again");
    }
}
static void test_budgets() {
    std::printf("budgets: 100 ms between requests, and a flood of 5000 in one packet\n");
    test_budgets_refusals();
    test_budgets_chest_unequip();
}
static void test_activate_alone() {
    std::printf("ServerActivateItem\n");
    Harness h;
    const items::Inventory before = h.s->inventory();
    const size_t events = h.mirror.events.size();
    h.request(item_activate(4, 2, guid_n(102)), h.pawn_ch);
    h.request(item_activate(3, 0, guid_n(200)), h.pawn_ch);
    CHECK(h.mirror.events.size() == events && h.s->inventory() == before && h.s->item_requests_applied() == 0 && h.s->item_requests_refused() == 0 && !h.s->failed(),
          "on its own it changes nothing, sends nothing and is not counted: ServerItemUsed carries the request");
    h.request_both(item_activate(4, 2, guid_n(102)), item_used(4, 2, guid_n(102)));
    CHECK(h.s->item_requests_applied() == 1 && h.mirror.events.size() == events + 2 && h.s->inventory().at(Bag, 4), "the pair, activate first, is one unequip: two events");
}
static void test_wire_forms() {
    std::printf("what is on the wire\n");
    Harness h;
    const BunchWriter used = item_used(4, 2, "b7a32f5ffa5b7f8e82bef4bc4ca1d7a9");
    CHECK(used.bit_pos() > 0, "ServerItemUsed writes");
    const BunchWriter sr = bag_move(h, 1, 5, guid_n(201), 32);
    CHECK(sr.bit_pos() > 0, "ServerRequestStorageEvent writes");
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const auto parsed = repl::read_client_bunch(pc, [](const emit::FIntrepidNetworkGUID &) -> const schema::ClassSchema * { return nullptr; }, sr.data(), sr.bit_pos());
    const bool found = parsed && parsed->rpcs.size() == 1 && parsed->rpcs[0].call.decoded && parsed->rpcs[0].call.rpc && std::string(parsed->rpcs[0].call.rpc->name) == "ServerRequestStorageEvent";
    CHECK(found, "the client's bunch reads as one ServerRequestStorageEvent, decoded to its last bit");
    if (found) {
        const auto ev = repl::parse_storage_event(parsed->rpcs[0].call);
        CHECK(ev && ev->type == 2 && ev->move_source.slots.size() == 1 && ev->move_source.slots[0].slot == 1 && ev->move_source.slots[0].count == 5 && ev->targets.size() == 1 && ev->targets[0].slots[0].slot == 32,
              "a Move from slot 1 (5 items) to slot 32");
    }
}
static void test_hostile() {
    std::printf("hostile requests\n");
    HostileFuzzer f;
    Harness &h = f.h;
    const std::set<std::string> &guids = f.guids;
    const size_t kRequests = 2500;
    bool sane = true;
    for (size_t i = 0; i < kRequests; ++i) {
        h.step(milliseconds(120));
        const auto inv_items = h.s->inventory().items();
        const size_t applied = h.s->item_requests_applied();
        const size_t kind = f.pick(5);
        if (kind < 2)
            f.step_use(inv_items, applied);
        else if (kind < 4)
            f.step_move(inv_items, applied);
        else
            f.step_damaged(inv_items);
        if (i % 50 == 49) {
            h.settle();
            sane = sane && inventory_is_sane(h.s->inventory(), guids, h.s->items_destroyed()) && !h.s->failed();
        }
    }
    const size_t applied_by_use = f.applied_by_use, applied_by_move = f.applied_by_move;
    h.settle();
    sane = sane && inventory_is_sane(h.s->inventory(), guids, h.s->items_destroyed());
    std::printf("    %zu requests: %zu applied (%zu ServerItemUsed, %zu storage events, %zu destroys), %zu refused, %zu dropped for the budget\n", kRequests, h.s->item_requests_applied(), applied_by_use, applied_by_move,
                h.s->items_destroyed(), h.s->item_requests_refused(), h.s->requests_dropped());
    CHECK(!h.s->failed() && sane, "2500 requests, valid, wrong and damaged: the session is up and the inventory is still a valid one, only items it started with, each in one slot, none lost but by a destroy");
    CHECK(h.s->item_requests_applied() > 50 && applied_by_use > 10 && applied_by_move > 10, "(the run did apply requests of both kinds)");
    CHECK(h.mirror.unreadable == 0, "everything the server sent reads");
    CHECK(mirror_matches(h), "the client's storages equal the inventory");
    CHECK(gear_matches(h), "and its HeldItems and AppearanceIDs equal what is worn");
}
static void test_saving_in_session_end() {
    {
        Harness h;
        h.s->finish("test");
        CHECK(h.sink.inventories.empty(), "a session that never changed an item saves no inventory at its end");
    }
    {
        Harness h;
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        h.s->finish("the client quit");
        CHECK(h.sink.inventories.size() == 1 && h.sink.inventories[0].id == kCyb && h.s->inventories_handed_over() == 1, "the end of a session that changed an item hands the inventory over, with the character's id");
        if (!h.sink.inventories.empty()) {
            CHECK(same_items(h.sink.inventories[0].items, h.s->inventory().items()) && h.sink.inventories[0].items.size() == 17, "it is the whole inventory as the session has it: 17 items");
            const auto &items = h.sink.inventories[0].items;
            bool chest_in_bag = false;
            for (const auto &it : items) chest_in_bag = chest_in_bag || (it.item_record_id == fi::kChest && it.container == Bag && it.slot == 4 && it.instance_guid == guid_n(102));
            CHECK(chest_in_bag, "the chest is in bag slot 4 (the same instance: its guid is kept)");
        }
        h.s->finish("again");
        CHECK(h.sink.inventories.size() == 1, "finish is once");
        CHECK(h.sink.positions.empty(), "(no position was saved: the character did not move)");
    }
    {
        Harness h;
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        h.step(milliseconds(150));
        h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
        CHECK(h.s->inventory() == items::Inventory::from_items(cyb_record().items, h.error).value(), "(the chest is worn again: the inventory is the one it started as)");
        h.s->finish("end");
        CHECK(h.sink.inventories.size() == 1, "it is still handed over: the character service may hold the intermediate state of a lost periodic save");
    }
}
static void test_saving_in_session_periodic() {
    {
        Harness h;
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        CHECK(h.sink.inventories.empty(), "(a change does not save at once: the saving is the interval's)");
        h.step(seconds(30));
        CHECK(h.sink.inventories.empty(), "30 s on: nothing is saved yet");
        h.step(seconds(31));
        CHECK(h.sink.inventories.size() == 1 && !h.s->inventory_dirty() && h.s->inventory_changed(), "after 60 s the changed inventory is handed over");
        h.step(seconds(61));
        CHECK(h.sink.inventories.size() == 1, "60 s later, with nothing changed since: it is not saved again");
        h.request(item_used(3, 4, guid_n(102)), h.pawn_ch);
        h.step(seconds(61));
        CHECK(h.sink.inventories.size() == 2 && same_items(h.sink.inventories[1].items, h.s->inventory().items()) && h.s->inventory().at(Worn, 2), "a change after that is handed over at the next interval, as the session has it now");
        h.s->finish("end");
        CHECK(h.sink.inventories.size() == 3, "and the end hands it over once more: it changed this session");
        h.step(seconds(61));
        CHECK(h.sink.inventories.size() == 3, "after finish the session saves no more");
    }
    {
        Harness h(cyb_record(), /*with_sink=*/false);
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        h.step(seconds(61));
        h.s->finish("end");
        CHECK(h.s->inventories_handed_over() == 0 && h.s->item_requests_applied() == 1 && !h.s->failed() && mirror_matches(h), "without a PositionSink nothing is handed over");
    }
    {
        Harness h;
        h.request(item_used(4, 2, guid_n(102)), h.pawn_ch);
        h.s->finish("end");
        CHECK(h.sink.positions.empty() && h.sink.inventories.size() == 1, "an inventory is saved without a position, and (movement tests) a position without an inventory");
    }
}
static void test_saving_in_the_session() {
    std::printf("saving in PlayerSession: 60 s when dirty, at finish when changed, never when untouched\n");
    test_saving_in_session_end();
    test_saving_in_session_periodic();
}
static void test_saver_inventories_queue() {
    {
        FakeCharacters api;
        CharacterSaver saver(api, quick());
        saver.save_inventory(kCyb, bard_items());
        CHECK(saver.flush(seconds(3)), "an inventory is saved; flush says everything is");
        const auto s = api.saved_inventories();
        CHECK(s.size() == 1 && s[0].id == kCyb && same_items(s[0].items, bard_items()) && saver.inventories_saved() == 1 && saver.saved() == 0 && saver.failed() == 0 && saver.queued() == 0,
              "the character service got the id and all 17 items as given; the position count is not touched");
    }
    {
        FakeCharacters api;
        api.hold();
        CharacterSaver saver(api, quick());
        saver.save_inventory("zz", first_items(1));
        CHECK(wait_until([&] { return api.in_flight(); }), "(the worker is inside the first call)");
        saver.save("aa", pos_of(1));
        saver.save_inventory("aa", first_items(1));
        saver.save_inventory("aa", first_items(2));
        saver.save_inventory("aa", first_items(3));
        CHECK(saver.queued() == 1 && saver.replaced() == 2, "while it is busy a character has one entry, with one position and one inventory in it: the later inventories replaced the waiting one (two replaced)");
        api.release();
        CHECK(saver.flush(seconds(3)), "(released)");
        const auto s = api.saved_inventories();
        CHECK(s.size() == 2 && s[0].id == "zz" && s[1].id == "aa" && s[1].items.size() == 3 && api.position_count() == 1,
              "the service saw zz's, then aa's latest inventory (3 items, not 1 or 2) and aa's position: nothing older after something newer");
    }
}
static void test_saver_inventories_retry() {
    {
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save_inventory("aa", first_items(4));
        CHECK(saver.flush(seconds(3)) && api.inventory_call_count() == 3 && saver.inventories_saved() == 1 && saver.failed() == 0, "an Unavailable service is tried again: two refusals and Ok is one saved inventory, not a failure");
    }
    {
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save_inventory("aa", first_items(4));
        saver.flush(seconds(3));
        CHECK(api.inventory_call_count() == 3 && saver.inventories_saved() == 0 && saver.failed() == 1 && saver.queued() == 0, "three Unavailable answers and the inventory is given up: counted as failed");
    }
}
static void test_saver_inventories_final_and_newer() {
    {
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Refused};
        CharacterSaver saver(api, quick());
        saver.save_inventory("aa", first_items(4));
        saver.flush(seconds(3));
        api.inventory_script = {svc::CallStatus::NotFound};
        saver.save_inventory("bb", first_items(4));
        saver.flush(seconds(3));
        CHECK(api.inventory_call_count() == 2 && saver.failed() == 2 && saver.inventories_saved() == 0, "a refusal (items the service's rules do not take) and NotFound are final: one call each, no retry");
    }
    {
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save("aa", pos_of(5));
        saver.save_inventory("aa", first_items(5));
        CHECK(saver.flush(seconds(3)) && api.position_call_count() == 1 && api.inventory_call_count() == 2 && saver.saved() == 1 && saver.inventories_saved() == 1 && saver.failed() == 0,
              "a position and an inventory for one character: the inventory is retried, the position is not sent again");
    }
    {
        FakeCharacters api;
        api.inventory_script = {svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver::Config cfg = quick();
        cfg.retry_delay = milliseconds(300);
        CharacterSaver saver(api, cfg);
        saver.save_inventory("aa", first_items(2));
        wait_until([&] { return api.inventory_call_count() == 1; });
        saver.save_inventory("aa", first_items(6));
        CHECK(saver.flush(seconds(3)), "(after the pause everything is saved)");
        const auto s = api.saved_inventories();
        CHECK(s.size() == 1 && s[0].items.size() == 6 && api.inventory_call_count() == 2, "the retry sends the newer inventory (6 items), not the old one: two calls, one saved");
    }
    {
        struct OldService : svc::CharacterApi {
            size_t positions = 0;
            svc::CharacterResult get_character(const std::string &) override {
                return {};
            }
            svc::CallStatus save_position(const std::string &, const svc::Position &) override {
                ++positions;
                return svc::CallStatus::Ok;
            }
        } api;
        CharacterSaver saver(api, quick());
        saver.save("aa", pos_of(1));
        saver.save_inventory("aa", first_items(2));
        saver.flush(seconds(3));
        CHECK(api.positions == 1 && saver.saved() == 1 && saver.failed() == 1 && saver.inventories_saved() == 0, "a service without SaveInventory: the position is saved, the inventory is a counted failure (no retry)");
    }
}
static void test_saver_inventories() {
    std::printf("CharacterSaver: save_inventory coalesces, retries Unavailable, drops after three, and keeps position and inventory apart\n");
    test_saver_inventories_queue();
    test_saver_inventories_retry();
    test_saver_inventories_final_and_newer();
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    test_join();
    test_unequip_and_equip_the_chest();
    test_two_hander_and_slots();
    test_moves_and_swaps();
    test_activate_alone();
    test_wire_forms();
    test_refusals();
    test_refusals_that_need_a_state();
    test_budgets();
    test_hostile();
    test_saving_in_the_session();
    test_saver_inventories();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
