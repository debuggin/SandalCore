// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_repl_items_common.h"

using namespace aoc::protocol;
using namespace aoc::protocol::repl;
using emit::BunchWriter;
using emit::FIntrepidNetworkGUID;
using wire::PacketReader;
namespace gen = aoc::protocol::schema::gen;
namespace cap = capture_items;

namespace {

std::optional<ItemStorageEvent> read_event_bits(const Bits& bits, size_t* consumed = nullptr) {
    return with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) {
        PacketReader r(d, bytes, n);
        auto e = read_item_storage_event(r);
        if (consumed) *consumed = r.pos();
        return e;
    });
}

Bits write_event_bits(const ItemStorageEvent& e, bool* ok = nullptr) {
    BunchWriter w;
    const bool wrote = write_item_storage_event(w, e);
    if (ok) *ok = wrote;
    return Bits(w);
}

}  // namespace

static void test_event_fits_and_default(const ItemStorageEvent& e) {
    ItemStorageEvent bad = e;
    bad.targets[0].object.ObjectId = 1;
    ItemStorageEvent many = e;
    many.targets.resize(kMaxEventContainers + 1);
    ItemStorageEvent slots = e;
    slots.move_source.slots.resize(kMaxEventSlots + 1);
    ItemStorageEvent item = e;
    item.item.stats.resize(kMaxItemStats + 1);
    BunchWriter w;
    CHECK(!item_storage_event_fits(bad) && !item_storage_event_fits(many) && !item_storage_event_fits(slots) && !item_storage_event_fits(item) &&
              !write_item_storage_event(w, bad) && !write_item_storage_event(w, many) && !write_item_storage_event(w, slots) &&
              !write_item_storage_event(w, item) && w.bit_pos() == 0,
          "an object with ObjectId 1, too many targets or slots, an item that does not fit: not written");
    BunchWriter dw;
    CHECK(write_item_storage_event(dw, ItemStorageEvent{}) && dw.bit_pos() == (128 + 32 + 8) + 1 + 32 + 8 + 128 + 96 + 32,
          "a default event is 465 bits: two empty containers, the empty item, no optionals");
}

static void test_event() {
    std::printf("FItemStorageEvent by hand\n");
    const ItemStorageEvent e = sample_event();
    bool ok = false;
    const Bits bits = write_event_bits(e, &ok);
    size_t consumed = 0;
    const auto back = read_event_bits(bits, &consumed);
    CHECK(ok && back && *back == e && consumed == bits.size(), "an event with a source, an item and a target reads back equal, to the last bit");

    const size_t container_bits = 128 + 32 + 192 + 8;
    const size_t item_at = container_bits;
    const size_t flag_at = item_at + 1096;
    CHECK(bits.size() == container_bits + 1096 + 32 + 8 + 128 + 3 * 32 + 32 + container_bits, "its size follows the exe's order: 2,113 bits with two containers of one slot");
    CHECK(bits.get(flag_at, 32) == 0 && bits.get(flag_at + 32, 8) == 2 && bits.get(flag_at + 40, 32) == 1, "then the optional-struct UBOOL (0), the EventType byte (Move 2), the instigator");

    ItemStorageEvent x = e;
    x.extra_guid_a = GuidValue{9, 8, 7, 6};
    x.extra_i64 = 0x1122334455667788ull;
    x.extra_guid_b = GuidValue{5, 5, 5, 5};
    const Bits xb = write_event_bits(x, &ok);
    const auto xr = read_event_bits(xb);
    CHECK(ok && xr && *xr == x && xb.size() == bits.size() + 128 + 64 + 128, "the three optionals, set: each is its UBOOL and its value");

    auto refused = [&](const Bits& b) { return !read_event_bits(b).has_value(); };
    Bits b = bits; b.set(flag_at, 1, 32);
    CHECK(refused(b), "the optional-struct UBOOL set (the retail delete and chest events do: a struct that is not decoded): refused");
    b = bits; b.set(flag_at, 2, 32);
    CHECK(refused(b), "a UBOOL of 2: refused");
    b = bits; b.set(0, 1, 32); b.set(32, 0, 32);
    CHECK(refused(b), "a container naming ObjectId 1 (the export-flags form): refused");
    b = bits; b.set(128, kMaxEventSlots + 1, 32);
    CHECK(refused(b), "more slots than the bound: refused");
    b = bits; b.set(128, 0xffffffffull, 32);
    CHECK(refused(b), "a slot count of -1: refused");
    b = bits; b.set(bits.size() - container_bits - 32, kMaxEventContainers + 1, 32);
    CHECK(refused(b), "more target containers than the bound: refused");
    b = bits; b.set(bits.size() - container_bits - 32, 64, 32);
    CHECK(refused(b), "target containers the bits left cannot hold: refused");
    bool all_refused = true;
    for (size_t n = 0; n < bits.size(); ++n) all_refused = all_refused && !read_event_bits(bits.slice(0, n)).has_value();
    CHECK(all_refused, "an event cut at every length: refused each time");
    test_event_fits_and_default(e);
}

static void test_storage_survey_invariants() {
    size_t shape_ok = 0, empty_ok = 0, filled_index_ok = 0, zeros_ok = 0;
    std::set<std::pair<int32_t, size_t>> sizes;
    for (const StoragePayload& p : g_survey.payloads) {
        if (p.storage.size() == static_cast<size_t>(p.maximum_items)) ++shape_ok;
        bool empties = true, filled = true;
        for (size_t i = 0; i < p.storage.size(); ++i) {
            const StorageSlot& s = p.storage[i];
            if (s.item.has_item) filled = filled && s.slot == static_cast<int32_t>(i);
            else empties = empties && s == StorageSlot{};
        }
        empty_ok += empties;
        filled_index_ok += filled;
        zeros_ok += p.container_name.empty() && p.maximum_weight == 0 && !p.loot_fountain_on_death && !p.randomize_angular &&
                    !p.client_retrieve_only && p.minimum_impulse == Vec3d{} && p.maximum_impulse == Vec3d{} &&
                    p.item_drop_on_destruction_percent == 0 && p.maximum_items_to_drop_percent == 0;
        sizes.insert({p.maximum_items, filled_slots(p)});
    }
    const size_t n = g_survey.payloads.size();
    char msg[200];
    std::snprintf(msg, sizeof msg, "all %zu distinct captured payloads: one Storage element per MaximumItems", n);
    CHECK(n == 6 + 4 && shape_ok == n, msg);
    CHECK(empty_ok == n && filled_index_ok == n, "an empty slot is the default StorageSlot (all zeros); a filled slot's Slot is its index");
    CHECK(zeros_ok == n, "no name, weight, percents, impulses or loot flags in any of them");
    CHECK(sizes.count({70, 64}) && sizes.count({83, 29}) && sizes.count({100, 16}) && sizes.count({24, 9}) && sizes.count({40, 4}) &&
              sizes.count({30, 3}),
          "the login's storages: Backpack 70 slots (64 filled), Equipment 83 (29), Quest 100 (16), resource bags 24 (9), 40 (4), 30 (3)");
    CHECK(sizes.count({70, 0}) && sizes.count({83, 13}) && sizes.count({100, 0}) && sizes.count({24, 0}),
          "the Bard's: an empty Backpack, Equipment with its 13 starter items, an empty Quest storage and resource bag");

    bool dims = false, matrices = false, backpack_plain = true;
    for (const StoragePayload& p : g_survey.payloads) {
        if (p.maximum_items == 24) dims = p.dimension == Vec2d{6, 4} && p.override_matrix.empty();
        if (p.maximum_items == 40) matrices = p.dimension == Vec2d{8, 5} && p.override_matrix.size() == 8 && p.override_matrix[0].size() == 5;
        if (p.maximum_items == 70) backpack_plain = backpack_plain && p.dimension == Vec2d{} && p.override_matrix.empty();
    }
    CHECK(dims && matrices && backpack_plain, "a resource bag has its Dimension (6x4, 8x5, 6x5) and the larger ones an OverrideMatrix (8 columns of 5); the others have neither");
}

static void test_storage_empty_backpack() {
    const StoragePayload* empty_backpack = nullptr;
    for (const StoragePayload& p : g_survey.payloads) {
        if (p.maximum_items == 70 && filled_slots(p) == 0) empty_backpack = &p;
    }
    if (empty_backpack) {
        StoragePayload built;
        built.target = empty_backpack->target;
        built.maximum_items = 70;
        built.storage.resize(70);
        CHECK(built == *empty_backpack, "an empty Backpack is a target, MaximumItems 70 and 70 default slots");
        const schema::RpcSchema* rpc = class_of(kPawn).rpc("ClientReceiveStoragePayload");
        const auto params = storage_payload_params(*rpc, built);
        BunchWriter w;
        bool failed = false;
        const size_t bits_written = params ? write_rpc_params(*rpc, *params, w, {}, &failed) : 0;
        bool same = false;
        for (const cap::Rpc& c : cap::kBard) {
            if (std::string(c.rpc) == "ClientReceiveStoragePayload" && c.bits == bits_written) same = Bits(w) == bits_of_hex(c.hex, c.bits) || same;
        }
        CHECK(!failed && bits_written == 13297 && same, "built from scratch it writes the captured empty Backpack's 13,297 bits, bit for bit");
        CHECK(storage_payload_params(*class_of(kPc).rpc("ClientReceiveStoragePayload"), built).has_value(),
              "and the PlayerController's entry takes it too");
    } else {
        CHECK(false, "the Bard's empty Backpack is in the corpus");
    }
}

static void test_storage_full_payload_and_refusals() {
    StoragePayload full;
    full.target.ObjectId = 1234;
    full.container_name = "Bag";
    full.maximum_weight = 12.5f;
    full.maximum_items = 3;
    full.loot_fountain_on_death = true;
    full.item_drop_on_destruction_percent = 0.25f;
    full.maximum_items_to_drop_percent = 0.5f;
    full.minimum_impulse = Vec3d{1, -2, 3};
    full.maximum_impulse = Vec3d{4, 5, -6};
    full.randomize_angular = true;
    full.storage.resize(3);
    full.storage[1].item = plain_item();
    full.storage[1].slot = 1;
    full.storage[1].transaction_lock = GuidValue{0x80000000, 2, 3, 0xffffffff};
    full.storage[1].check_loot_permission = true;
    full.storage[1].loot_permission = {GuidValue{1, 2, 3, 4}, GuidValue{0xffffffff, 0, 0, 0x80000000}};
    full.client_retrieve_only = true;
    full.dimension = Vec2d{3, 1};
    full.override_matrix = {{-1, -2, 7}, {}, {0x7fffffff, INT32_MIN}};
    const schema::RpcSchema* rpc = class_of(kPawn).rpc("ClientReceiveStoragePayload");
    const auto params = storage_payload_params(*rpc, full);
    BunchWriter w;
    bool failed = false;
    const size_t written = params ? write_rpc_params(*rpc, *params, w, {}, &failed) : 0;
    size_t pos = 0;
    std::optional<RpcParams> read_back;
    with_exact(Bits(w), [&](const uint8_t* d, size_t, size_t bits) { read_back = read_rpc_params(*rpc, d, bits, pos); return 0; });
    const auto parsed = read_back ? parse_storage_payload(*rpc, *read_back) : std::nullopt;
    CHECK(params && !failed && written > 0 && parsed && *parsed == full, "a payload with every member set (a name, weights, impulses, a loot lock and permissions, a matrix) survives the round trip");
    RpcCall call;
    call.rpc = rpc;
    if (read_back) call.params = *read_back;
    call.decoded = true;
    CHECK(parse_storage_payload(call) && *parse_storage_payload(call) == full, "parse_storage_payload takes a decoded call");
    call.decoded = false;
    CHECK(!parse_storage_payload(call).has_value(), "and refuses an undecoded one");
    const schema::RpcSchema* other = class_of(kPc).rpc("ClientReceiveStorageEvent");
    CHECK(!storage_payload_params(*other, full).has_value() && !parse_storage_payload(*other, RpcParams{}).has_value(),
          "another RPC is not a storage payload");
    CHECK(!parse_storage_payload(*rpc, RpcParams{}).has_value(), "parameters without NewStorageData are not a payload");
    StoragePayload bad = full;
    bad.storage[1].item.stats.resize(kMaxItemStats + 1);
    StoragePayload bad_name = full;
    bad_name.container_name = std::string("a\0b", 3);
    CHECK(!storage_payload_params(*rpc, bad).has_value() && !storage_payload_params(*rpc, bad_name).has_value(),
          "an item that does not fit, a name with a NUL: no parameters");
    StoragePayload huge = full;
    huge.storage.resize(0x10000);
    CHECK(!storage_payload_params(*rpc, huge).has_value(), "more than 65,535 slots: no parameters");
}

static void test_storage_events_and_field_round_trip() {
    const schema::RpcSchema* rpc = class_of(kPawn).rpc("ClientReceiveStoragePayload");
    const schema::RpcSchema* ev = class_of(kPc).rpc("ClientReceiveStorageEvent");
    const schema::RpcSchema* request = class_of(kPc).rpc("ServerRequestStorageEvent");
    const ItemStorageEvent e = sample_event();
    const auto ep = storage_event_params(*ev, e);
    const auto er = storage_event_params(*request, e);
    CHECK(ep && er && parse_storage_event(*ev, *ep) == e && parse_storage_event(*request, *er) == e,
          "ClientReceiveStorageEvent(ItemEvent) and ServerRequestStorageEvent(StorageEvent) take the event and give it back");
    CHECK(!storage_event_params(*rpc, e).has_value() && !parse_storage_event(*rpc, RpcParams{}).has_value() && !parse_storage_event(*ev, RpcParams{}).has_value(),
          "no other RPC is an event RPC, and an event left at the default is no event");
    const schema::RpcSchema* storage_events = class_of(kPc).rpc("ServerRequestStorageEvents");
    CHECK(!storage_events || !storage_event_params(*storage_events, e).has_value(), "the array form (ServerRequestStorageEvents) is not claimed");

    ItemStorageEvent add;
    add.type = uint8_t(StorageEventType::Add);
    add.item = plain_item();
    FIntrepidNetworkGUID backpack_guid;
    backpack_guid.ObjectId = 430988;
    add.targets.push_back({.object = backpack_guid, .slots = {{3, 1, add.item.guid}}});
    const auto p = storage_event_params(*ev, add);
    BunchWriter aw;
    bool af = false;
    const size_t ab = p ? write_rpc_params(*ev, *p, aw, {}, &af) : 0;
    CHECK(p && !af && ab == 1921,
          "an Add built the README's way (one target container with one slot of the item's GUID) is 1,921 bits: the size of every captured Add of a plain item");

    const cap::Rpc* backpack = nullptr;
    for (const cap::Rpc& c : cap::kLogin) {
        if (std::string(c.rpc) == "ClientReceiveStoragePayload" && c.bits == 84145) backpack = &c;
    }
    if (backpack) {
        const schema::ClassSchema& pawn = class_of(kPawn);
        const schema::RpcSchema* prpc = pawn.rpc("ClientReceiveStoragePayload");
        size_t at = 0;
        std::optional<RpcParams> params2;
        const Bits bb = bits_of_hex(backpack->hex, backpack->bits);
        with_exact(bb, [&](const uint8_t* d, size_t, size_t bits) { params2 = read_rpc_params(*prpc, d, bits, at); return 0; });
        BunchWriter field;
        bool f = false;
        const size_t field_bits = params2 ? write_rpc_field(pawn, *prpc, *params2, field, {}, &f) : 0;
        size_t fpos = 0;
        const auto calls = field_bits ? with_exact(Bits(field), [&](const uint8_t* d, size_t, size_t bits) { return read_rpc_fields(pawn, d, bits, fpos); })
                                      : std::optional<std::vector<RpcCall>>{};
        CHECK(!f && calls && calls->size() == 1 && (*calls)[0].decoded && (*calls)[0].param_bits == backpack->bits &&
                  parse_storage_payload((*calls)[0]).has_value() && filled_slots(*parse_storage_payload((*calls)[0])) == 64,
              "the login's Backpack (84,145 bits) as a pawn field: written with its header, read_rpc_fields gives it back whole");
    } else {
        CHECK(false, "the login's Backpack is in the corpus");
    }
}

static void test_storage_layer() {
    std::printf("the typed storage RPCs\n");
    test_storage_survey_invariants();
    test_storage_empty_backpack();
    test_storage_full_payload_and_refusals();
    test_storage_events_and_field_round_trip();
}

static void test_events_of_the_captures() {
    std::printf("what the captured events show\n");
    CHECK(g_survey.events == 125 + 13 && g_survey.event_other == 0, "all 138 events the servers sent (the login's 125 and the Bard's 13) are Adds");
    CHECK(g_survey.add_shape_ok == g_survey.events,
          "each Add has no source container, one target container holding one slot (of the item's quantity and instance GUID), status 14 and no optionals");
}

struct Rng {
    uint64_t s;
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    size_t below(size_t n) { return static_cast<size_t>(next() % n); }
};

static bool read_write_read(const schema::RpcSchema& rpc, const Bits& bits, bool* read_ok) {
    const CodecOptions opts = client_options();
    size_t pos = 0;
    std::optional<RpcParams> first;
    with_exact(bits, [&](const uint8_t* d, size_t, size_t n) { first = read_rpc_params(rpc, d, n, pos, opts); return 0; });
    *read_ok = first.has_value();
    if (!first) return true;
    BunchWriter w;
    bool failed = false;
    write_rpc_params(rpc, *first, w, opts, &failed);
    if (failed) return false;
    size_t pos2 = 0;
    std::optional<RpcParams> second;
    with_exact(Bits(w), [&](const uint8_t* d, size_t, size_t n) { second = read_rpc_params(rpc, d, n, pos2, opts); return 0; });
    return second && second->values == first->values;
}

static Bits mutate_fuzz_bits(const Bits& original, Rng& rng) {
    static const uint64_t specials[] = {0, 1, 2, 31, 32, 63, 64, 65, 255, 4096, 4097, 65535, 0x7fffffff, 0x80000000ull, 0xffffffffull};
    Bits bits = original;
    switch (rng.below(5)) {
        case 0:
            for (size_t k = 1 + rng.below(8); k; --k) {
                const size_t at = rng.below(bits.size());
                bits.b[at] = !bits.b[at];
            }
            break;
        case 1:
            bits.b.resize(rng.below(bits.size()));
            break;
        case 2:
            for (size_t k = rng.below(200); k; --k) bits.b.push_back(rng.below(2));
            break;
        case 3: {
            const size_t at = rng.below(bits.size() > 32 ? bits.size() - 32 : 1);
            if (bits.size() >= 32) bits.set(at, specials[rng.below(sizeof specials / sizeof specials[0])], 32);
            break;
        }
        default: {
            const int width = rng.below(3) == 0 ? 6 : rng.below(2) ? 8 : 16;
            if (bits.size() > 64) bits.set(rng.below(bits.size() - 16), specials[rng.below(sizeof specials / sizeof specials[0])], width);
            break;
        }
    }
    return bits;
}

static void test_fuzz() {
    std::printf("hostile input: a seeded fuzz of the captured payloads\n");
    Rng rng{0x9e3779b97f4a7c15ull};
    size_t runs = 0, read = 0, refused = 0, inconsistent = 0;
    double slowest = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int corpus = 0; corpus < 3; ++corpus) {
        size_t n = 0;
        const cap::Rpc* rpcs = corpus_rpcs(corpus, &n);
        for (size_t i = 0; i < n; ++i) {
            const cap::Rpc& c = rpcs[i];
            const Where where = c.pc ? kPc : c.pawn ? kPawn : kAbility;
            const schema::RpcSchema& rpc = *class_of(where).rpc(c.rpc);
            const Bits original = bits_of_hex(c.hex, c.bits);
            const int mutations = c.bits > 20000 ? 12 : (std::string(c.rpc) == "ClientReceiveStoragePayload" ? 40 : 25);
            for (int m = 0; m < mutations; ++m) {
                const Bits bits = mutate_fuzz_bits(original, rng);
                const auto start = std::chrono::steady_clock::now();
                bool ok = false;
                if (!read_write_read(rpc, bits, &ok)) ++inconsistent;
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                slowest = std::max(slowest, ms);
                ++runs;
                (ok ? read : refused)++;
            }
        }
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char msg[240];
    std::snprintf(msg, sizeof msg, "%zu mutated captures (%zu still read, %zu refused): whatever reads writes back to a payload that reads equal", runs, read, refused);
    CHECK(runs >= 3000 && inconsistent == 0, msg);
    std::snprintf(msg, sizeof msg, "and none took long (slowest %.1f ms, all %.1f s)", slowest, total);
    CHECK(slowest < 1000 && total < 120, msg);
    CHECK(read > 0 && refused > 0, "the mutations include both outcomes");
}

static void test_hostile_counts() {
    std::printf("hostile counts cost nothing\n");
    const schema::RpcSchema* rpc = class_of(kPawn).rpc("ClientReceiveStoragePayload");
    StoragePayload p;
    p.target.ObjectId = 5;
    p.maximum_items = 1;
    p.storage.resize(1);
    const auto params = storage_payload_params(*rpc, p);
    BunchWriter w;
    write_rpc_params(*rpc, *params, w);
    Bits bits(w);
    const size_t count_at = bits.size() - 16 - 128 - 1 - 178 - 16;
    CHECK(bits.get(count_at, 16) == 1, "(the count of the one slot sits where the layout says)");
    bits.set(count_at, 0xffff, 16);
    bool ok = true;
    const auto t0 = std::chrono::steady_clock::now();
    const bool consistent = read_write_read(*rpc, bits, &ok);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(consistent && !ok && ms < 200, "65,535 slots announced over a few hundred bits: refused, quickly");
    StoragePayload m = p;
    m.override_matrix.assign(1000, {});
    const auto mp = storage_payload_params(*rpc, m);
    BunchWriter mw;
    write_rpc_params(*rpc, *mp, mw);
    Bits mbits(mw);
    bool mok = false;
    CHECK(read_write_read(*rpc, mbits, &mok) && mok, "a thousand empty matrix columns (the bits hold them) read and write back");
    mbits.b.resize(mbits.size() - 16 * 400);
    CHECK(read_write_read(*rpc, mbits, &mok) && !mok, "the same cut short (the columns it announced are missing): refused");
}

void run_repl_items_storage() {
    test_event();
    test_storage_layer();
    test_events_of_the_captures();
    test_hostile_counts();
    test_fuzz();
}
