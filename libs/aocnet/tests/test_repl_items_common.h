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

inline Survey g_survey;

void run_repl_items_storage();
