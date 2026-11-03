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

inline aoc::protocol::emit::FIntrepidNetworkGUID guid(uint64_t id) {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4;
    return g;
}

void run_repl_rep_block_writer();
