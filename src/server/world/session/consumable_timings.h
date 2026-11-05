// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <string>

namespace aoc::worldd {

inline double consumable_montage_end(const std::string& soft_path) {
    if (soft_path.find("Consumable_Scroll") != std::string::npos) return 0.98;
    if (soft_path.find("Consumable_Potion") != std::string::npos) return 1.86;
    return -1;
}

}  // namespace aoc::worldd
