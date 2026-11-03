// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/constants.h"
#include "protocol/schema/generated/index.h"
#include "session/mounts.h"
#include "session/player_world.h"
#include "spawns/mount_spawn.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

inline int g_pass = 0;
inline int g_fail = 0;
inline aoc::worldd::spawns::MountBook* g_book = nullptr;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline uint64_t g_next = 0x2000000;

inline aoc::protocol::emit::FIntrepidNetworkGUID mint() {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId = g_next;
    g_next += 2;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}

inline const aoc::worldd::content::MountRow* first_of_kind(
    const aoc::worldd::spawns::MountBook& book,
    const char* kind) {
    for (const aoc::worldd::content::MountRow& r : book.data().mounts) {
        if (r.kind == kind && !r.mule) return &r;
    }
    return nullptr;
}

void run_mounts_summon();
