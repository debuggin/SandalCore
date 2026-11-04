// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <cmath>
#include <cstdint>

namespace aoc::worldd::world {

inline constexpr float kBorderHysteresisBandCm = 2000.0f;

enum class AuthorityState : uint8_t {
    Local = 0,
    HandoverPending = 1,
    RemoteGhost = 2,
};

[[nodiscard]] constexpr bool is_newer_lease_epoch(uint64_t candidate_epoch,
                                                  uint64_t current_epoch) noexcept {
    return candidate_epoch > current_epoch;
}

[[nodiscard]] constexpr AuthorityState evaluate_border_handover(
    float signed_border_dist_cm,
    AuthorityState current_state,
    float hysteresis_cm = kBorderHysteresisBandCm) noexcept {
    if (!(signed_border_dist_cm == signed_border_dist_cm) || !(hysteresis_cm >= 0.0f)) {
        return current_state;
    }
    if (signed_border_dist_cm < -hysteresis_cm) {
        return AuthorityState::Local;
    }
    if (signed_border_dist_cm > hysteresis_cm) {
        return current_state == AuthorityState::Local ? AuthorityState::HandoverPending
                                                      : AuthorityState::RemoteGhost;
    }
    return current_state;
}

[[nodiscard]] constexpr AuthorityState complete_border_handover(
    AuthorityState current_state,
    uint64_t candidate_epoch,
    uint64_t current_epoch) noexcept {
    if (current_state == AuthorityState::HandoverPending &&
        is_newer_lease_epoch(candidate_epoch, current_epoch)) {
        return AuthorityState::RemoteGhost;
    }
    return current_state;
}

}  // namespace aoc::worldd::world
