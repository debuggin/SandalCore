// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "capture_items.h"
#include "protocol/repl/item_codec.h"
#include "protocol/repl/rpc.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

inline int g_pass = 0;
inline int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline const aoc::protocol::schema::ClassSchema& schema_named(const char* name) {
    const aoc::protocol::schema::ClassSchema* s = aoc::protocol::schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

inline std::vector<uint8_t> bytes_of_hex(const char* hex) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    return out;
}

struct Bits {
    std::vector<bool> b;
    Bits() = default;
    Bits(const std::vector<uint8_t>& bytes, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back((bytes[i >> 3] >> (i & 7)) & 1); }
    explicit Bits(const aoc::protocol::emit::BunchWriter& w) : Bits(w.bytes(), w.bit_pos()) {}
    size_t size() const { return b.size(); }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out((b.size() + 7) / 8, 0);
        for (size_t i = 0; i < b.size(); ++i) out[i >> 3] |= static_cast<uint8_t>(b[i] ? 1 : 0) << (i & 7);
        return out;
    }
    Bits slice(size_t from, size_t n) const { Bits r; r.b.assign(b.begin() + from, b.begin() + from + n); return r; }
    void set(size_t at, uint64_t v, int n) { for (int i = 0; i < n; ++i) b[at + i] = (v >> i) & 1; }
    uint64_t get(size_t at, int n) const { uint64_t v = 0; for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(b[at + i] ? 1 : 0) << i; return v; }
    void insert(size_t at, uint64_t v, int n) { for (int i = 0; i < n; ++i) b.insert(b.begin() + at + i, (v >> i) & 1); }
    void erase(size_t at, size_t n) { b.erase(b.begin() + at, b.begin() + at + n); }
    bool operator==(const Bits& o) const { return b == o.b; }
};

inline Bits bits_of_hex(const char* hex, size_t n) { return Bits(bytes_of_hex(hex), n); }

template <class F>
inline auto with_exact(const Bits& bits, F f) {
    const std::vector<uint8_t> v = bits.bytes();
    std::unique_ptr<uint8_t[]> exact(new uint8_t[v.empty() ? 1 : v.size()]);
    if (!v.empty()) std::memcpy(exact.get(), v.data(), v.size());
    return f(static_cast<const uint8_t*>(exact.get()), v.size(), bits.size());
}

inline std::optional<aoc::protocol::repl::ItemInstance> read_item_bits(const Bits& bits, size_t* consumed = nullptr) {
    return with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) {
        aoc::protocol::wire::PacketReader r(d, bytes, n);
        auto it = aoc::protocol::repl::read_item_instance(r);
        if (consumed) *consumed = r.pos();
        return it;
    });
}

inline std::optional<aoc::protocol::repl::ItemStorageEvent> read_event_bits(const Bits& bits, size_t* consumed = nullptr) {
    return with_exact(bits, [&](const uint8_t* d, size_t bytes, size_t n) {
        aoc::protocol::wire::PacketReader r(d, bytes, n);
        auto e = aoc::protocol::repl::read_item_storage_event(r);
        if (consumed) *consumed = r.pos();
        return e;
    });
}

inline Bits write_item_bits(const aoc::protocol::repl::ItemInstance& it, bool* ok = nullptr) {
    aoc::protocol::emit::BunchWriter w;
    const bool wrote = aoc::protocol::repl::write_item_instance(w, it);
    if (ok) *ok = wrote;
    return Bits(w);
}

inline Bits write_event_bits(const aoc::protocol::repl::ItemStorageEvent& e, bool* ok = nullptr) {
    aoc::protocol::emit::BunchWriter w;
    const bool wrote = aoc::protocol::repl::write_item_storage_event(w, e);
    if (ok) *ok = wrote;
    return Bits(w);
}

inline std::string num(uint64_t v) { return std::to_string(v); }
inline std::string snum(int64_t v) { return std::to_string(v); }

inline std::string hexn(uint64_t v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*llx", digits, static_cast<unsigned long long>(v));
    return buf;
}

inline std::string guid_text(const aoc::protocol::repl::GuidValue& g) {
    return hexn(g.a, 8) + hexn(g.b, 8) + hexn(g.c, 8) + hexn(g.d, 8);
}

inline std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

inline std::string utf8_hex(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += hexn(c, 2);
    return out;
}

template <class T> inline uint64_t bits_of(T v) {
    return std::bit_cast<std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>(v);
}

inline std::string dump_item(const aoc::protocol::repl::ItemInstance& it) {
    if (!it.has_item) return "empty";
    std::vector<std::string> gems, stats, cds, custom;
    for (const aoc::protocol::repl::SocketedGem& g : it.gems)
        gems.push_back(num(g.socket_index) + ":" + num(g.item_record_id) + ":" + num(g.socket_record_id) + ":" + num(g.quality) + ":" + (g.flag ? "1" : "0"));
    for (const aoc::protocol::repl::PersistedStat& s : it.stats) stats.push_back(num(s.stat_id) + ":" + hexn(bits_of(s.value), 8));
    for (const aoc::protocol::repl::PersistedCooldown& c : it.cooldowns)
        cds.push_back(num(c.record_guid) + ":" + hexn(bits_of(c.date_time_start), 16) + ":" + num(c.saved_charges));
    for (const aoc::protocol::repl::PersistedCustomData& d : it.custom_data) custom.push_back(num(d.type) + ":" + utf8_hex(d.text));
    return "rec=" + num(it.record_id) + " qty=" + num(it.quantity) + " qual=" + num(it.quality) + " ench=" + num(it.enchantment_level) +
           " temp=" + num(it.tempering_value) + " gems=[" + join(gems, ",") + "] dur=" + hexn(bits_of(it.durability), 8) +
           " stolen=" + (it.stolen ? "1" : "0") + " guid=" + guid_text(it.guid) + " ct=" + snum(it.creation_time) +
           " crate=" + guid_text(it.crate_guid) + " node=" + num(it.node_guid_purchased_from) + " lock=" + snum(it.lock_expiration) +
           " stats=[" + join(stats, ",") + "] cd=[" + join(cds, ",") + "] custom=[" + join(custom, ",") + "] ts=" +
           hexn(bits_of(it.synchronized_timestamp), 16);
}

inline std::string short_item(const aoc::protocol::repl::ItemInstance& it) {
    return it.has_item ? num(it.record_id) + "@" + guid_text(it.guid) : "empty";
}

inline std::string obj_text(const aoc::protocol::emit::FIntrepidNetworkGUID& o) {
    return num(o.ObjectId) + "." + num(o.ServerId) + "." + num(o.Randomizer);
}

inline std::string dump_container(const aoc::protocol::repl::StorageEventContainer& c) {
    std::vector<std::string> slots;
    for (const aoc::protocol::repl::StorageEventSlot& s : c.slots) slots.push_back(snum(s.slot) + ":" + snum(s.count) + ":" + guid_text(s.item_guid));
    return obj_text(c.object) + "/" + join(slots, ",") + "/st=" + num(c.status);
}

inline std::string dump_event(const aoc::protocol::repl::ItemStorageEvent& e) {
    std::vector<std::string> targets;
    for (const aoc::protocol::repl::StorageEventContainer& c : e.targets) targets.push_back(dump_container(c));
    return "type=" + num(e.type) + " src=" + dump_container(e.move_source) + " item=" + short_item(e.item) +
           " instig=" + guid_text(e.instigator) + " xa=" + (e.extra_guid_a ? guid_text(*e.extra_guid_a) : "-") +
           " xi=" + (e.extra_i64 ? num(*e.extra_i64) : "-") + " xb=" + (e.extra_guid_b ? guid_text(*e.extra_guid_b) : "-") +
           " targets=[" + join(targets, ";") + "]";
}

inline std::string dump_payload(const aoc::protocol::repl::StoragePayload& p) {
    std::vector<std::string> filled, matrix;
    for (size_t i = 0; i < p.storage.size(); ++i) {
        if (p.storage[i].item.has_item) filled.push_back(num(i) + ":" + snum(p.storage[i].slot) + ":" + short_item(p.storage[i].item));
    }
    for (const auto& col : p.override_matrix) {
        std::vector<std::string> cells;
        for (int32_t c : col) cells.push_back(snum(c));
        matrix.push_back(join(cells, ","));
    }
    return "target=" + (p.target.ObjectId ? obj_text(p.target) : std::string("-")) + " name=" + p.container_name +
           " weight=" + hexn(bits_of(p.maximum_weight), 8) + " max=" + snum(p.maximum_items) +
           " fountain=" + (p.loot_fountain_on_death ? "1" : "0") + " drop=" + hexn(bits_of(p.item_drop_on_destruction_percent), 8) +
           " dropmax=" + hexn(bits_of(p.maximum_items_to_drop_percent), 8) + " imin=" + hexn(bits_of(p.minimum_impulse.x), 16) + "," +
           hexn(bits_of(p.minimum_impulse.y), 16) + "," + hexn(bits_of(p.minimum_impulse.z), 16) + " imax=" + hexn(bits_of(p.maximum_impulse.x), 16) +
           "," + hexn(bits_of(p.maximum_impulse.y), 16) + "," + hexn(bits_of(p.maximum_impulse.z), 16) +
           " angular=" + (p.randomize_angular ? "1" : "0") + " slots=" + num(p.storage.size()) + " filled=" + num(filled_slots(p)) +
           " retrieve=" + (p.client_retrieve_only ? "1" : "0") + " dim=" + hexn(bits_of(p.dimension.x), 16) + "," +
           hexn(bits_of(p.dimension.y), 16) + " matrix=" + join(matrix, ";") + " [" + join(filled, ",") + "]";
}

enum Where { kPc = 0, kPawn = 1, kAbility = 2 };

inline const aoc::protocol::schema::ClassSchema& class_of(Where w) {
    static const aoc::protocol::schema::ClassSchema& pc = schema_named("AoCPlayerControllerBP_C");
    static const aoc::protocol::schema::ClassSchema& pawn = schema_named("PlayerPawn_C");
    static const aoc::protocol::schema::ClassSchema& ability = schema_named("UAoCAbilityComponent");
    return w == kPc ? pc : w == kPawn ? pawn : ability;
}

inline aoc::protocol::repl::CodecOptions client_options() {
    aoc::protocol::repl::CodecOptions o;
    o.inline_objects = true;
    return o;
}

inline const capture_items::Rpc* corpus_rpcs(int corpus, size_t* n) {
    switch (corpus) {
        case 0:  *n = sizeof(capture_items::kLogin) / sizeof(capture_items::kLogin[0]); return capture_items::kLogin;
        case 1:  *n = sizeof(capture_items::kBard) / sizeof(capture_items::kBard[0]); return capture_items::kBard;
        default: *n = sizeof(capture_items::kClient) / sizeof(capture_items::kClient[0]); return capture_items::kClient;
    }
}

inline aoc::protocol::repl::ItemInstance plain_item() {
    aoc::protocol::repl::ItemInstance it;
    it.has_item = true;
    it.record_id = 6064632584274051086ull;
    it.quantity = 1;
    it.quality = 1;
    it.durability = 11.0f;
    it.guid = aoc::protocol::repl::GuidValue{0x9cb881dd, 0x7fbb492e, 0xb9e6b0e1, 0x9677c0f7};
    it.creation_time = 639040482080000000ll;
    return it;
}

inline aoc::protocol::repl::StorageEventContainer container(
    uint64_t object,
    std::vector<aoc::protocol::repl::StorageEventSlot> slots,
    uint8_t status = aoc::protocol::repl::kStorageNoError) {
    aoc::protocol::repl::StorageEventContainer c;
    c.object.ObjectId = object;
    c.object.ServerId = 31;
    c.object.Randomizer = 2044408973;
    c.slots = std::move(slots);
    c.status = status;
    return c;
}

inline aoc::protocol::repl::ItemStorageEvent sample_event() {
    aoc::protocol::repl::ItemStorageEvent e;
    e.type = static_cast<uint8_t>(aoc::protocol::repl::StorageEventType::Move);
    e.move_source = container(701180, {{40, 1, plain_item().guid}});
    e.item = plain_item();
    e.instigator = aoc::protocol::repl::GuidValue{1, 2, 3, 4};
    e.targets.push_back(container(701180, {{32, 1, aoc::protocol::repl::GuidValue{}}}));
    return e;
}

struct Survey {
    std::vector<aoc::protocol::repl::StoragePayload> payloads;
    size_t events = 0, event_adds = 0, event_other = 0;
    size_t add_shape_ok = 0;
};

inline std::map<std::string, size_t> g_counts;
inline size_t g_items[3] = {};
inline size_t g_empty[3] = {};
inline Survey g_survey;

void run_repl_items_part2();
