// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "ability_rig.h"
#include "combat/aftermath.h"
#include "combat/combat_data.h"
#include "combat/npc_stats.h"
#include "combat_rig.h"
#include "npc/motion_table.h"
#include "session/summoner_pets.h"

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif

using namespace ability_rig;

inline int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

struct PetTestData {
    npc::MotionTable motion;
    std::string      wolf_path;
    bool             ok = false;
    PetTestData() {
        std::string error;
        ok = motion.load(NPC_MOTION_PATH, error);
        CHECKF(ok, "npc_motion.tsv: %s", error.c_str());
        for (const auto& [p, row] : motion.rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = p;
        }
    }
};

inline int64_t first_effect_of(const abilities::Catalog& catalog, int64_t hit) {
    const abilities::HitDef* found = catalog.hit(hit);
    return found && !found->effects.empty() ? found->effects.front().effect_guid : 0;
}

inline bool port_of_actor(const combat::CombatWorld& cw, uint64_t actor) {
    for (const combat::PlayerPort* p : cw.players()) {
        if (p->actor_id() == actor) return true;
    }
    return false;
}

inline std::string blueprint_path_holding(const npc::MotionTable& motion, const char* needle) {
    for (const auto& [p, row] : motion.rows()) {
        if (p.find(needle) != std::string::npos) return p;
    }
    return {};
}

void run_ability_npc_pets_tests();
void run_ability_npc_pets_cmds_tests();
