// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "stats/stat_engine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aoc::worldd {

struct ItemCast {
    int64_t                      record = 0;
    int64_t                      ability = 0;
    int                          quality = 1;
    std::vector<stats::StatMod>  stats;
    int                          container = 0;
    int32_t                      slot = -1;
    std::string                  instance_guid;
    bool                         consumes = true;
};

}  // namespace aoc::worldd
