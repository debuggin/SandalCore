// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "spawns/area.h"
#include "world/mesh_border.h"

#include <cmath>
#include <cstdio>

namespace {

int g_pass = 0;
int g_fail = 0;

void check_cond(bool ok, int line, const char* expr, const char* msg) {
    if (ok) {
        ++g_pass;
        std::printf("  [ok ] %s\n", msg);
    } else {
        ++g_fail;
        std::printf("  [FAIL] line %d: %s (%s)\n", line, msg, expr);
    }
    std::fflush(stdout);
}

#define CHECK(cond, msg) check_cond(static_cast<bool>(cond), __LINE__, #cond, msg)

using namespace aoc::worldd;

struct BorderAnchor {
    double x_cm = -459889.6;
    double y_cm = 0.0;
};

BorderAnchor find_riverlands_sandsquall_border() noexcept {
    const double x = -459889.6;
    double lo = 599317.1;  // Miraleth (Riverlands)
    double hi = 873514.2;  // Sunhaven latitude (Sandsquall)
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (spawns::resolve_point_region_bit(x, mid) == spawns::RegionBit::Riverlands) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return {x, 0.5 * (lo + hi)};
}

void test_region_resolution_and_signed_distance(const BorderAnchor& border) {
    std::printf("--- spawns/area.h regional border distance ---\n");
    CHECK(spawns::region_bit_slug(spawns::RegionBit::Riverlands) == "riverlands",
          "region_bit_slug maps Riverlands");
    CHECK(spawns::region_bit_slug(spawns::RegionBit::Sandsquall) == "sandsquall",
          "region_bit_slug maps Sandsquall");
    CHECK(spawns::resolve_point_region_bit(border.x_cm, border.y_cm - 2500.0) == spawns::RegionBit::Riverlands,
          "point -2500 cm south of border resolves to Riverlands");
    CHECK(spawns::resolve_point_region_bit(border.x_cm, border.y_cm + 2500.0) == spawns::RegionBit::Sandsquall,
          "point +2500 cm north of border resolves to Sandsquall");

    const float dist_inside = spawns::signed_region_border_distance_cm(
        border.x_cm, border.y_cm - 2500.0, spawns::RegionBit::Riverlands);
    const float dist_shallow = spawns::signed_region_border_distance_cm(
        border.x_cm, border.y_cm + 2500.0, spawns::RegionBit::Riverlands);
    const float dist_deep = spawns::signed_region_border_distance_cm(
        border.x_cm, border.y_cm + 8000.0, spawns::RegionBit::Riverlands);

    CHECK(dist_inside <= 0.0f, "signed_region_border_distance_cm is negative inside current_region");
    CHECK(dist_shallow > 0.0f && dist_shallow < world::kRegionHandoverHysteresisCm,
          "signed_region_border_distance_cm is in (0, 5000) cm for +2500 cm penetration");
    CHECK(dist_deep >= world::kRegionHandoverHysteresisCm,
          "signed_region_border_distance_cm is >= 5000 cm for +8000 cm penetration");
}

void test_boundary_jitter_anti_ping_pong(const BorderAnchor& border) {
    std::printf("--- world/mesh_border.h anti-ping-pong jitter deadband ---\n");
    world::RegionalHandoverState state{};
    state.authoritative_region = spawns::RegionBit::Riverlands;
    state.candidate_region = spawns::RegionBit::Riverlands;

    int64_t now_ms = 1000;
    for (int i = 0; i < 10; ++i) {
        const double offset_cm = (i % 2 == 0) ? 2500.0 : -2500.0;
        const auto dec = world::evaluate_regional_handover(
            state, border.x_cm, border.y_cm + offset_cm, now_ms);
        CHECK(dec == world::BorderDecision::StayLocal,
              "oscillation within +/-2500 cm stays local");
        now_ms += 100;
    }
    CHECK(state.handover_count == 0, "10 border oscillations produce 0 handovers");
    CHECK(state.authoritative_region == spawns::RegionBit::Riverlands,
          "authoritative_region remains Riverlands during border jitter");
    CHECK(state.last_suppressed_by_hysteresis,
          "last_suppressed_by_hysteresis records deadband suppression");
}

void test_sustained_crossing_and_cooldown(const BorderAnchor& border) {
    std::printf("--- world/mesh_border.h sustained crossing & 2000ms cooldown ---\n");
    world::RegionalHandoverState state{};
    state.authoritative_region = spawns::RegionBit::Riverlands;
    state.candidate_region = spawns::RegionBit::Riverlands;

    const auto d1 = world::evaluate_regional_handover(state, border.x_cm, border.y_cm + 8000.0, 1000);
    const auto d2 = world::evaluate_regional_handover(state, border.x_cm, border.y_cm + 8000.0, 1100);
    const auto d3 = world::evaluate_regional_handover(state, border.x_cm, border.y_cm + 8000.0, 1200);

    CHECK(d1 == world::BorderDecision::StayLocal && d2 == world::BorderDecision::StayLocal,
          "samples 1 and 2 beyond 5000 cm accumulate dwell without premature handover");
    CHECK(d3 == world::BorderDecision::HandoverPending,
          "sample 3 beyond 5000 cm commits handover");
    CHECK(state.handover_count == 1 && state.authoritative_region == spawns::RegionBit::Sandsquall &&
              state.lease_epoch == 2,
          "sustained crossing updates authoritative_region to Sandsquall and increments lease_epoch");

    for (int i = 0; i < 3; ++i) {
        const auto cd_dec = world::evaluate_regional_handover(
            state, border.x_cm, border.y_cm - 8000.0, 1700 + i * 50);
        CHECK(cd_dec == world::BorderDecision::StayLocal,
              "reverse crossing at +500ms (< 2000ms cooldown) is suppressed");
    }
    CHECK(state.handover_count == 1 && state.authoritative_region == spawns::RegionBit::Sandsquall &&
              state.last_suppressed_by_hysteresis,
          "cooldown gate prevents rapid bounce-back to Riverlands");

    world::BorderDecision back_dec = world::BorderDecision::StayLocal;
    for (int i = 0; i < 3; ++i) {
        back_dec = world::evaluate_regional_handover(
            state, border.x_cm, border.y_cm - 8000.0, 3700 + i * 100);
    }
    CHECK(back_dec == world::BorderDecision::HandoverPending && state.handover_count == 2 &&
              state.authoritative_region == spawns::RegionBit::Riverlands && state.lease_epoch == 3,
          "reverse crossing after +2500ms (>= 2000ms cooldown) cleanly hands over to Riverlands");
}

}  // namespace

int main() {
    const BorderAnchor border = find_riverlands_sandsquall_border();
    test_region_resolution_and_signed_distance(border);
    test_boundary_jitter_anti_ping_pong(border);
    test_sustained_crossing_and_cooldown(border);

    std::printf("\nResult: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
