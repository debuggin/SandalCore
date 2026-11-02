// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "combat/hit_port.h"
#include "combat/hit_resolver.h"
#include "session/abilities.h"
#include "session/ability_catalog.h"
#include "stats/character_stats.h"

#include <cstdint>
#include <vector>

namespace aoc::worldd {

// Cast-bar abilities wait for ServerRequestBranchVerification before jumping to the branch section and taking cost.
struct BranchedCast {
    const char* ability;
    int64_t     branch;
    float       position;
    double      item;
    double      request;
    double      answer;
    double      cost;
    double      end;
    int         animation = -1;
    bool        cost_first = false;
    bool        cancels = true;
    double      full = 0;
    int64_t     held_skill = 0;
    double      hold_limit = 0;
    double      release = 0;
    int64_t     charged_branch = 0;
    double      charged_answer = 0;
    double      charged_cost = 0;
};

struct LoopedCast {
    const char* ability;
    float       position;
    double      item;
    double      first_shot;
    double      period;
    double      jump;
    double      end;
};

struct GatheredCast {
    const char* ability;
    int         projectile;
    double      angle;
    double      length;
    double      behind;
    double      speed;
    int         charged_projectile = 0;
    double      charged_length = 0;
    double      charged_radius = 0;
    double      charged_speed = 0;
    const char* charged_montage = "";
    uint64_t    charged_montage_guid = 0;
};

struct TimedCast {
    enum class Kind : uint8_t {
        Volley,
        Strike,
        Barrage,
    };
    const char* ability;
    Kind        kind;
    int         projectile;
    double      first;
    double      gap;
    int         count;
    double      speed;
    double      angle = 0;
    double      length = 0;
    double      behind = 0;
    double      radius = 0;
    double      pitch[3] = {0, 0, 0};
    float       jump_position = 0;
    double      jump_at = 0;
    double      end_at = 0;
};

struct ShotCast {
    const char* ability;
    int         animation;
    double      fire;
    bool        magical;
    const char* ask;
    double      ask_gap;
};

}  // namespace aoc::worldd
