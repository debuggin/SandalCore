// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aoc::worldd::spawns {

enum class RegionBit : uint8_t {
    None = 0,
    Riverlands = 1u << 0,
    Sandsquall = 1u << 1,
    Jundark = 1u << 2,
    TableMountains = Jundark,
    Anvils = 1u << 3,
    TurquoiseSea = 1u << 4,
    All = 0x1Fu,
};

[[nodiscard]] RegionBit resolve_point_region_bit(double x_cm, double y_cm) noexcept;
[[nodiscard]] constexpr std::string_view region_bit_slug(RegionBit bit) noexcept;
[[nodiscard]] float signed_region_border_distance_cm(double x_cm, double y_cm,
                                                     RegionBit current_region) noexcept;

}  // namespace aoc::worldd::spawns

namespace aoc::worldd::world {

inline constexpr float kBorderHysteresisBandCm = 2000.0f;
inline constexpr float kRegionHandoverHysteresisCm = 5000.0f;
inline constexpr uint32_t kRegionHandoverDwellSamples = 3;
inline constexpr int64_t kRegionHandoverCooldownMs = 2000;

enum class AuthorityState : uint8_t {
    Local = 0,
    StayLocal = 0,
    HandoverPending = 1,
    RemoteGhost = 2,
    HandoverCommitted = 2,
};

using BorderDecision = AuthorityState;

struct RegionalHandoverState {
    spawns::RegionBit authoritative_region = spawns::RegionBit::Riverlands;
    spawns::RegionBit candidate_region = spawns::RegionBit::Riverlands;
    uint32_t          dwell_samples = 0;
    uint32_t          dwell_ticks_beyond_hysteresis = 0;
    int64_t           last_handover_ms = 0;
    int64_t           last_handover_commit_ms = 0;
    uint64_t          handover_count = 0;
    uint64_t          lease_epoch = 1;
    float             last_signed_border_distance_cm = 0.0f;
    AuthorityState    authority_state = AuthorityState::Local;
    uint32_t          suppressed_oscillations = 0;
    bool              last_suppressed_by_hysteresis = false;
    bool              outside_worker_area = false;
};

[[nodiscard]] inline float runtime_handover_hysteresis_cm() noexcept {
    for (const char* k : {"AOC_WORLD_HANDOVER_HYSTERESIS_CM", "WORLD_HANDOVER_HYSTERESIS_CM"}) {
        if (const char* v = std::getenv(k); v && *v) {
            char* end = nullptr;
            const float f = std::strtof(v, &end);
            if (end != v && std::isfinite(f) && f >= 0.0f) return std::clamp(f, 0.0f, 100000.0f);
        }
    }
    return kRegionHandoverHysteresisCm;
}

[[nodiscard]] inline uint32_t runtime_handover_dwell_samples() noexcept {
    for (const char* k : {"AOC_WORLD_HANDOVER_DWELL_SAMPLES", "WORLD_HANDOVER_DWELL_SAMPLES"}) {
        if (const char* v = std::getenv(k); v && *v) {
            const long n = std::strtol(v, nullptr, 10);
            if (n >= 1 && n <= 60) return static_cast<uint32_t>(n);
        }
    }
    return kRegionHandoverDwellSamples;
}

[[nodiscard]] inline int64_t runtime_handover_cooldown_ms() noexcept {
    for (const char* k : {"AOC_WORLD_HANDOVER_COOLDOWN_MS", "WORLD_HANDOVER_COOLDOWN_MS"}) {
        if (const char* v = std::getenv(k); v && *v) {
            const long long n = std::strtoll(v, nullptr, 10);
            if (n >= 0 && n <= 60000) return static_cast<int64_t>(n);
        }
    }
    return kRegionHandoverCooldownMs;
}

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

[[nodiscard]] constexpr AuthorityState evaluate_border_handover(
    float signed_distance_to_border_cm,
    bool currently_local_authority,
    float hysteresis_cm = kBorderHysteresisBandCm) noexcept {
    return evaluate_border_handover(
        signed_distance_to_border_cm,
        currently_local_authority ? AuthorityState::Local : AuthorityState::RemoteGhost,
        hysteresis_cm);
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

[[nodiscard]] constexpr AuthorityState complete_border_handover(
    RegionalHandoverState& state,
    int64_t now_ms) noexcept {
    state.dwell_samples = 0;
    state.dwell_ticks_beyond_hysteresis = 0;
    if (now_ms > 0) {
        state.last_handover_ms = now_ms;
        state.last_handover_commit_ms = now_ms;
    }
    ++state.handover_count;
    ++state.lease_epoch;
    state.authority_state = AuthorityState::HandoverCommitted;
    state.last_suppressed_by_hysteresis = false;
    return AuthorityState::HandoverCommitted;
}

[[nodiscard]] inline BorderDecision evaluate_regional_handover_sample(
    RegionalHandoverState& state,
    spawns::RegionBit target_region,
    float signed_dist_cm,
    int64_t now_ms,
    bool is_explicit_teleport = false,
    float hysteresis_override_cm = -1.0f) noexcept {
    const float hyst_cm = (hysteresis_override_cm >= 0.0f) ? hysteresis_override_cm : runtime_handover_hysteresis_cm();
    const uint32_t req_dwell = runtime_handover_dwell_samples();
    const int64_t cooldown_ms = runtime_handover_cooldown_ms();
    state.last_signed_border_distance_cm = signed_dist_cm;
    if (target_region == spawns::RegionBit::None ||
        target_region == state.authoritative_region ||
        signed_dist_cm < hyst_cm) {
        state.candidate_region = state.authoritative_region;
        state.dwell_samples = 0;
        state.dwell_ticks_beyond_hysteresis = 0;
        if (target_region != spawns::RegionBit::None &&
            target_region != state.authoritative_region) {
            state.last_suppressed_by_hysteresis = true;
            ++state.suppressed_oscillations;
        } else if (signed_dist_cm < -hyst_cm) {
            state.last_suppressed_by_hysteresis = false;
            state.authority_state = AuthorityState::Local;
        }
        return BorderDecision::StayLocal;
    }
    if (state.last_handover_ms > 0 &&
        (now_ms - state.last_handover_ms) < cooldown_ms) {
        state.last_suppressed_by_hysteresis = true;
        ++state.suppressed_oscillations;
        return BorderDecision::StayLocal;
    }
    if (state.candidate_region != target_region) {
        state.candidate_region = target_region;
        state.dwell_samples = 0;
        state.dwell_ticks_beyond_hysteresis = 0;
    }
    state.dwell_samples = is_explicit_teleport ? req_dwell
                                               : (state.dwell_samples + 1);
    state.dwell_ticks_beyond_hysteresis = state.dwell_samples;
    if (state.dwell_samples >= req_dwell) {
        state.authoritative_region = target_region;
        state.candidate_region = target_region;
        state.dwell_samples = 0;
        state.dwell_ticks_beyond_hysteresis = 0;
        state.last_handover_ms = now_ms;
        state.last_handover_commit_ms = now_ms;
        ++state.handover_count;
        ++state.lease_epoch;
        state.authority_state = AuthorityState::HandoverPending;
        state.last_suppressed_by_hysteresis = false;
        return BorderDecision::HandoverPending;
    }
    state.last_suppressed_by_hysteresis = true;
    return BorderDecision::StayLocal;
}

[[nodiscard]] inline BorderDecision evaluate_regional_handover(
    RegionalHandoverState& state,
    double signed_dist_cm,
    int64_t now_ms,
    float hysteresis_cm = -1.0f,
    bool is_explicit_teleport = false) noexcept {
    const float hyst = (hysteresis_cm >= 0.0f) ? hysteresis_cm : runtime_handover_hysteresis_cm();
    const uint32_t req_dwell = runtime_handover_dwell_samples();
    const int64_t cooldown_ms = runtime_handover_cooldown_ms();
    const float penetration = state.outside_worker_area ? static_cast<float>(-signed_dist_cm)
                                                        : static_cast<float>(signed_dist_cm);
    if (!std::isfinite(penetration) || penetration < hyst) {
        state.dwell_samples = 0;
        if (penetration > 0.0f) state.last_suppressed_by_hysteresis = true;
        else if (penetration < -hyst) state.last_suppressed_by_hysteresis = false;
        return BorderDecision::StayLocal;
    }
    if (state.last_handover_ms > 0 && (now_ms - state.last_handover_ms) < cooldown_ms) {
        state.last_suppressed_by_hysteresis = true;
        return BorderDecision::StayLocal;
    }
    state.dwell_samples = is_explicit_teleport ? req_dwell : (state.dwell_samples + 1);
    if (state.dwell_samples >= req_dwell) {
        state.outside_worker_area = !state.outside_worker_area;
        state.dwell_samples = 0;
        state.last_handover_ms = now_ms;
        ++state.handover_count;
        ++state.lease_epoch;
        state.last_suppressed_by_hysteresis = false;
        return BorderDecision::HandoverPending;
    }
    state.last_suppressed_by_hysteresis = true;
    return BorderDecision::StayLocal;
}

[[nodiscard]] inline BorderDecision evaluate_regional_handover(
    RegionalHandoverState& state,
    double x_cm,
    double y_cm,
    int64_t now_ms,
    bool is_explicit_teleport = false) noexcept {
    const spawns::RegionBit target = spawns::resolve_point_region_bit(x_cm, y_cm);
    const float dist = spawns::signed_region_border_distance_cm(x_cm, y_cm, state.authoritative_region);
    return evaluate_regional_handover_sample(state, target, dist, now_ms, is_explicit_teleport);
}

[[nodiscard]] inline BorderDecision evaluate_regional_handover(
    RegionalHandoverState& state,
    double x_cm,
    double y_cm,
    int64_t now_ms,
    float hysteresis_cm,
    bool is_explicit_teleport = false) noexcept {
    const spawns::RegionBit target = spawns::resolve_point_region_bit(x_cm, y_cm);
    const float dist = spawns::signed_region_border_distance_cm(x_cm, y_cm, state.authoritative_region);
    return evaluate_regional_handover_sample(state, target, dist, now_ms, is_explicit_teleport, hysteresis_cm);
}

}  // namespace aoc::worldd::world
