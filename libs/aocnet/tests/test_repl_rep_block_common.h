// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "capture_blocks.h"
#include "protocol/repl/rep_block.h"
#include "protocol/schema/generated/index.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

inline int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline const aoc::protocol::schema::ClassSchema& schema_named(const char* name) {
    const aoc::protocol::schema::ClassSchema* s = aoc::protocol::schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

inline const aoc::protocol::schema::ClassSchema& schema_of(const capture_blocks::Block& b) {
    return schema_named(b.class_name);
}

struct Decoded {
    std::vector<uint8_t> bytes;
    std::optional<aoc::protocol::repl::RepState> state;
    size_t pos = 0;
};

inline Decoded decode(const capture_blocks::Block& b, const aoc::protocol::repl::CodecOptions& opts = {}) {
    Decoded d;
    d.bytes = capture_blocks::bytes_of(b.hex);
    d.state = aoc::protocol::repl::read_rep_block(schema_of(b), d.bytes.data(), b.bits, d.pos, opts);
    return d;
}

inline bool writes_exactly(const aoc::protocol::schema::ClassSchema& s, const aoc::protocol::repl::RepState& state,
                           const uint8_t* expect, size_t bits, const aoc::protocol::repl::CodecOptions& opts = {}) {
    aoc::protocol::emit::BunchWriter w;
    aoc::protocol::repl::RepShadow shadow;
    const size_t n = aoc::protocol::repl::write_rep_block(s, state, shadow, w, opts);
    size_t diff = 0;
    if (n != bits || w.bit_pos() != bits) {
        std::printf("    wrote %zu bits, expected %zu\n", n, bits);
        return false;
    }
    if (!capture_blocks::same_bit_range(w.data(), expect, bits, &diff)) {
        std::printf("    first differing bit %zu\n", diff);
        return false;
    }
    return true;
}

inline std::vector<uint8_t> slice_bits(const std::vector<uint8_t>& bytes, size_t start, size_t n) {
    std::vector<uint8_t> out((n + 7) / 8, 0);
    for (size_t i = 0; i < n; ++i) out[i >> 3] |= ((bytes[(start + i) >> 3] >> ((start + i) & 7)) & 1) << (i & 7);
    return out;
}

inline aoc::protocol::emit::FIntrepidNetworkGUID guid(uint64_t id) {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4;
    return g;
}

inline void round_trip_block(const capture_blocks::Block& b, const aoc::protocol::repl::CodecOptions& opts = {}) {
    const std::string name = std::string(b.name) + " (" + b.class_name + ", " + std::to_string(b.rep_bits) + " bits)";
    const Decoded d = decode(b, opts);
    CHECK(d.state && d.pos == b.rep_bits, (name + ": reads to the last bit of the RepLayout block").c_str());
    if (!d.state) return;
    CHECK(writes_exactly(schema_of(b), *d.state, d.bytes.data(), b.rep_bits, opts),
          (name + ": the decoded state writes back the exact bits").c_str());
    aoc::protocol::emit::BunchWriter w;
    aoc::protocol::repl::RepShadow shadow;
    aoc::protocol::repl::write_rep_block(schema_of(b), *d.state, shadow, w, opts);
    const size_t before = w.bit_pos();
    CHECK(aoc::protocol::repl::write_rep_block(schema_of(b), *d.state, shadow, w, opts) == 0 && w.bit_pos() == before,
          (name + ": a second write of the same state writes nothing").c_str());
}

void run_repl_rep_block_part2();
