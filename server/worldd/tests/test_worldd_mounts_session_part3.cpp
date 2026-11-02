// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_mounts_session_common.h"

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace cm = capture_mount_session;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::BunchWriter;
using std::chrono::milliseconds;

static void find_callout_in_bunch(const fake::Delivered& d, std::optional<repl::ElementState>& our_callout) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return;
    for (const repl::BlockView& b : *blocks) {
        if (!b.header.is_actor) continue;
        BunchWriter w;
        w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
        auto st = repl::load_object_state(class_named("AoCPlayerControllerBP_C"), w.data(), b.payload_bits, b.header.has_rep_layout);
        if (!st) continue;
        const schema::NetField* f = st->schema->field("CalloutQueueReplication");
        const repl::FastArrayState* a = f ? st->arrays.find(f->index) : nullptr;
        if (a && !a->items().empty()) our_callout = a->items().back().values;
    }
}

static std::optional<repl::ElementState> find_callout_in_delivered(const Harness& h, size_t from) {
    std::optional<repl::ElementState> our_callout;
    for (size_t i = from; i < h.got.size(); ++i) {
        const fake::Delivered& d = h.got[i];
        if (d.channel != h.pc_ch || d.open || d.close) continue;
        find_callout_in_bunch(d, our_callout);
    }
    return our_callout;
}

static void check_golden_refusal_capture(uint16_t capture) {
    const cm::Block* item = retail(capture, "item");
    const cm::Block* end = retail(capture, "end");
    const cm::Block* callout = retail(capture, "callout");
    const cm::Block* off = cm::find(capture, "callout_off");
    if (!item || !end || !callout) return;
    const auto ri = load_block(*item);
    const auto rc = load_block(*callout);
    const auto ri_item = ri ? ability_item_of(*ri) : std::nullopt;
    CHECK(ri_item && rc, (std::string("retail's item and callout of ") + std::to_string(capture) + " read").c_str());
    if (!ri_item || !rc) return;
    Harness h(cyb_record());
    h.play();
    const size_t from = h.got.size();
    summon_with(h, kLockedSlot, 11);
    std::optional<repl::ElementState> shown;
    for (milliseconds done{0}; done < milliseconds(1200); done += milliseconds(20)) {
        h.step();
        if (!shown) shown = our_item(h, true);
    }
    const std::optional<repl::ElementState> our_callout = find_callout_in_delivered(h, from);
    CHECK(shown.has_value(), "worldd shows the item of the refused summon");
    if (shown) {
        const std::set<uint16_t> d = item_diff(*ri_item, *shown);
        const std::set<uint16_t> allowed_item = {2, 15, 17};
        CHECK(std::includes(allowed_item.begin(), allowed_item.end(), d.begin(), d.end()),
              (std::string("the refusal's item is retail's ") + std::to_string(capture) + " but for the instance, the counter and the instigator").c_str());
    }
    const schema::NetField* f = rc->schema->field("CalloutQueueReplication");
    const repl::FastArrayState* ra = f ? rc->arrays.find(f->index) : nullptr;
    CHECK(ra && !ra->items().empty() && our_callout.has_value(), "worldd sends a callout, and so did retail");
    if (ra && !ra->items().empty() && our_callout) {
        const std::set<uint16_t> d = item_diff(ra->items().back().values, *our_callout);
        const std::set<uint16_t> allowed = {21, 25, 26, 27, 28};
        bool subset = true;
        std::string s;
        for (const uint16_t x : d) {
            subset = subset && allowed.count(x);
            s += std::to_string(x) + " ";
        }
        CHECK(subset, (std::string("the callout is retail's but for its key and creation time (differing: ") + s + ")").c_str());
    }
    (void)off;
}

static void test_golden_refusal() {
    std::printf("golden: the refused summon of the retail captures\n");
    for (const uint16_t capture : {876, 870}) check_golden_refusal_capture(capture);
    Harness h(cyb_record());
    h.play();
    const Clock::time_point req = h.t;
    summon_with(h, kLockedSlot, 11);
    const Saw saw = watch(h, req, milliseconds(4500));
    const cm::Block* a = cm::find(876, "item");
    const cm::Block* b = cm::find(870, "item");
    const cm::Block* e1 = cm::find(876, "end");
    const cm::Block* e2 = cm::find(870, "end");
    CHECK(a && b && e1 && e2 && saw.ability_item * 1000 >= std::min(a->ms, b->ms) - 40 && saw.ability_item * 1000 <= std::max(a->ms, b->ms) + 40,
          "the item comes within retail's range (+95 to +116 ms)");
    CHECK(a && b && e1 && e2 && saw.item_end * 1000 >= std::min(e1->ms, e2->ms) - 50 && saw.item_end * 1000 <= std::max(e1->ms, e2->ms) + 50,
          "the end comes within retail's range (+1.58 to +1.62 s)");
}

struct ClientLists {
    std::vector<int64_t> known, records;
    size_t updates = 0;
};

static bool apply_ability_array_field(const repl::RepState& st, const schema::ClassSchema& cls, const char* name, std::vector<int64_t>& list) {
    const repl::Value* val = st.get(cls, name);
    const repl::ArrayValue* a = val ? val->get_if<repl::ArrayValue>() : nullptr;
    if (!a) return false;
    list.resize(a->elements.size());
    for (size_t i = 0; i < a->elements.size(); ++i) {
        const auto it = a->elements[i].values.find(1);
        if (it != a->elements[i].values.end() && it->second.get_if<int64_t>()) list[i] = *it->second.get_if<int64_t>();
    }
    return true;
}

static void apply_delivered_client_lists(const Harness& h, const schema::ClassSchema& cls, const fake::Delivered& d, ClientLists& out) {
    if (d.channel != h.pawn_ch || d.open || d.close || d.content.bit_pos() <= 8) return;
    const auto views = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!views) return;
    for (const repl::BlockView& v : *views) {
        if (!(v.header.object == h.s->bootstrap().setup().guids.ability) || !v.header.has_rep_layout) continue;
        size_t p = v.payload_pos;
        const auto st = repl::read_rep_block(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!st) continue;
        const bool c1 = apply_ability_array_field(*st, cls, "KnownAbilities", out.known);
        const bool c2 = apply_ability_array_field(*st, cls, "ReplicatedRecordsToLoad", out.records);
        if (c1 || c2) ++out.updates;
    }
}

static ClientLists client_lists(const Harness& h) {
    ClientLists out;
    const repl::Subobject* ab = h.s->bootstrap().pawn().find(h.s->bootstrap().setup().guids.ability);
    if (!ab) return out;
    for (const fake::Delivered& d : h.got) apply_delivered_client_lists(h, *ab->obj.schema, d, out);
    return out;
}

static constexpr int64_t kSummonMountRecord = 108436548376185ll;

static size_t times_in(const std::vector<int64_t>& v, int64_t x) { return static_cast<size_t>(std::count(v.begin(), v.end(), x)); }

static bool use_mount_item_like_retail(Harness& h, int slot, int32_t client_instance = 7) {
    const svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    const std::string guid = item ? item->instance_guid : std::string();
    if (times_in(client_lists(h).records, kSummonMountRecord) == 0) {
        h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true)});
        return false;
    }
    summon_with(h, slot, client_instance);
    return true;
}

static std::vector<svc::InventoryItem> kit_without_mounts() {
    std::vector<svc::InventoryItem> out;
    for (svc::InventoryItem& it : kit()) {
        if (it.item_record_id == g_mount_record || it.item_record_id == g_other_mount_record || it.item_record_id == g_aquatic_record || it.item_record_id == g_flying_record) continue;
        out.push_back(std::move(it));
    }
    return out;
}

static void test_item_abilities() {
    std::printf("the pawn's ability records for the mount items it carries: the client needs Summon_Mount loaded to send the trigger\n");
    std::vector<int64_t> with_mounts;
    {
        Harness h(cyb_record());
        h.play();
        const ClientLists l = client_lists(h);
        with_mounts = l.records;
        const std::vector<int64_t> tail(bs::static_known_tail().begin(), bs::static_known_tail().end());
        CHECK(times_in(l.records, kSummonMountRecord) == 1, "a character with mounts in the bag has Summon_Mount in ReplicatedRecordsToLoad, once");
        CHECK(times_in(l.known, kSummonMountRecord) == 0, "and not in KnownAbilities (retail's login: 92 records with it, 75 known without it)");
        CHECK(l.records.size() >= tail.size() && std::equal(tail.begin(), tail.end(), l.records.end() - static_cast<std::ptrdiff_t>(tail.size())),
              "the static tail still ends the records (the item's ability comes before it, as in retail's login)");
        CHECK(l.records == h.s->record_abilities() && l.known == h.s->known_abilities(), "the client's lists are the session's");
        const Clock::time_point req = h.t;
        CHECK(use_mount_item_like_retail(h, kMountSlot), "the client has the ability loaded: it sends ServerTriggerAbility with ServerItemUsed");
        const Saw saw = watch(h, req, milliseconds(2600));
        CHECK(h.s->mount_summon_requests() == 1 && saw.open >= 0 && mount_count(h) == 1 && saw.ability_item >= 0,
              "the summon starts on the client's trigger: the ability item, then the mount's open");
    }
    {
        Harness h(cyb_record(kit_without_mounts()));
        h.play();
        const ClientLists l = client_lists(h);
        std::vector<int64_t> expected = with_mounts;
        expected.erase(std::remove(expected.begin(), expected.end(), kSummonMountRecord), expected.end());
        CHECK(!l.records.empty() && times_in(l.records, kSummonMountRecord) == 0, "a character with no mount item does not load Summon_Mount");
        CHECK(l.records == h.s->record_abilities() && l.records == expected, "and its records are the other character's but for that one");
    }
}

static void test_item_abilities_in_a_real_world() {
    std::printf("the same on the real content.db's world (its talents catalog: the production path of the join's lists)\n");
    Harness h(cyb_record(), true);
    if (!h.fx.world->ready() || !h.fx.world->talents()) {
        std::printf("  [skip] the real world is not ready or has no talents: %s\n", h.fx.world->content_problem().c_str());
        return;
    }
    h.play();
    const ClientLists l = client_lists(h);
    const std::vector<int64_t> tail(bs::static_known_tail().begin(), bs::static_known_tail().end());
    CHECK(times_in(l.records, kSummonMountRecord) == 1 && times_in(l.known, kSummonMountRecord) == 0, "Summon_Mount is in the records once and in no KnownAbilities");
    CHECK(l.records.size() >= tail.size() && std::equal(tail.begin(), tail.end(), l.records.end() - static_cast<std::ptrdiff_t>(tail.size())), "before the static tail");
    const Clock::time_point req = h.t;
    CHECK(use_mount_item_like_retail(h, kMountSlot), "the client sends the trigger with the item use");
    const Saw saw = watch(h, req, milliseconds(2600));
    CHECK(saw.open >= 0 && mount_count(h) == 1, "and the mount is summoned");
}

struct NoItemCatalog : items::ItemCatalog {
    std::optional<items::ItemInfo> info(int64_t) const override { return std::nullopt; }
};

static void test_item_ability_arrives_with_the_item() {
    std::printf("a mount item that reaches the bag during the session brings its ability record right after it (retail 821: the record is appended)\n");
    Harness h(cyb_record(kit_without_mounts()));
    h.play();
    const ClientLists before = client_lists(h);
    CHECK(times_in(before.records, kSummonMountRecord) == 0, "the bag holds no mount: no Summon_Mount");
    items::Inventory& inv = const_cast<items::Inventory&>(h.s->inventory());
    svc::InventoryItem fresh = make_item(Bag, 0, g_mount_record, 900);
    fresh.slot = inv.first_free(Bag);
    const int slot = fresh.slot;
    CHECK(inv.grant({fresh}, NoItemCatalog()).ok, "the mount item is granted into the bag");
    h.run(milliseconds(300));
    const ClientLists after = client_lists(h);
    CHECK(times_in(after.records, kSummonMountRecord) == 1 && after.records.size() == before.records.size() + 1 && after.records.back() == kSummonMountRecord,
          "Summon_Mount is appended to the client's records, once");
    CHECK(after.updates == before.updates + 1 && after.known == before.known, "in one update, and KnownAbilities does not change");
    CHECK(after.records == h.s->record_abilities(), "the client's records are the session's");
    svc::InventoryItem second = make_item(Bag, 0, g_other_mount_record, 901);
    second.slot = inv.first_free(Bag);
    CHECK(inv.grant({second}, NoItemCatalog()).ok, "a second mount item is granted");
    h.run(milliseconds(300));
    const ClientLists again = client_lists(h);
    CHECK(again.updates == after.updates && again.records == after.records, "a second mount item adds no record and sends no update");
    const Clock::time_point req = h.t;
    CHECK(use_mount_item_like_retail(h, slot), "the client sends ServerTriggerAbility with the item use");
    const Saw saw = watch(h, req, milliseconds(2600));
    CHECK(saw.open >= 0 && mount_count(h) == 1 && h.s->mount_summon_requests() == 1, "and the mount is summoned");
}

void run_mounts_session_part3() {
    test_golden_refusal();
    test_item_abilities();
    test_item_ability_arrives_with_the_item();
    test_item_abilities_in_a_real_world();
}
