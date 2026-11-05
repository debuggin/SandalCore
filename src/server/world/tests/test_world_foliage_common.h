// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "capture_foliage.h"
#include "protocol/repl/fname.h"
#include "session/player_world.h"
#include "test_support.h"

#include <spdlog/spdlog.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

inline int g_pass = 0;
inline int g_fail = 0;

#undef CHECK
#define CHECK(cond, msg)                       \
    do {                                       \
        if (cond) {                            \
            std::printf("  [ok ] %s\n", msg);  \
            g_pass++;                          \
        } else {                               \
            std::printf("  [FAIL] %s\n", msg); \
            g_fail++;                          \
        }                                      \
    } while (0)

inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;

inline bool file_exists(const char *p) {
    if (!p || !*p) {
        return false;
    }
    FILE *f = std::fopen(p, "rb");
    if (f) {
        std::fclose(f);
    }
    return f != nullptr;
}

inline const char *db_path() {
    const char *env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) {
        return env;
    }
#ifdef FOLIAGE_CONTENT_DB_PATH
    if (file_exists(FOLIAGE_CONTENT_DB_PATH)) {
        return FOLIAGE_CONTENT_DB_PATH;
    }
#endif
    return nullptr;
}

inline const char *tsv_path() {
    const char *env = std::getenv("AOC_FOLIAGE_MAP");
    if (env && *env) {
        return env;
    }
#ifdef FOLIAGE_TSV_PATH
    if (file_exists(FOLIAGE_TSV_PATH)) {
        return FOLIAGE_TSV_PATH;
    }
#endif
    return nullptr;
}


struct World {
    std::optional<aoc::worldd::content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;
    aoc::worldd::gather::Gathering *g = nullptr;
    aoc::worldd::foliage::Foliage *f = nullptr;

    bool open(const char *path, const char *tsv, bool all_cells = true) {
        std::string error;
        db = aoc::worldd::content::ContentDb::open(path, error);
        if (!db) {
            return false;
        }
        world = std::make_unique<aoc::worldd::PlayerWorld>(&*db, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId, guids);
        if (!world->ready()) {
            return false;
        }
        aoc::worldd::gather::Config cfg;
        if (!world->start_gathering(cfg, error)) {
            return false;
        }
        g = world->gathering();
        aoc::worldd::foliage::Config fc;
        fc.map_path = tsv;
        fc.all_cells = all_cells;
        if (!world->start_foliage(fc, error)) {
            return false;
        }
        f = world->foliage();
        return true;
    }

    uint32_t row_of(const std::string &owner, const std::string &comp, uint32_t index) const {
        const auto r = f->map().find(owner, comp, index);
        if (!r) {
            std::exit(2);
        }
        return *r;
    }

    std::unique_ptr<aoc::worldd::stats::CharacterStats> make_stats(int level = 25) {
        const aoc::worldd::bootstrap::StatContext ctx = world->stat_context();
        auto cs = std::make_unique<aoc::worldd::stats::CharacterStats>(*ctx.bundle, *ctx.gear, 2, level, ctx.rounding);
        std::string err;
        if (!cs->start({}, err)) {
            std::exit(2);
        }
        return cs;
    }
};

inline constexpr const char *kCell41_19 = "InstancedFoliageActor_25600_-41_19_0";

void run_foliage_cells_tests(World &w, const char *db, const char *tsv);
