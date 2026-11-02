#include "../world/test_harness.h"

#include <algorithm>
#include <cmath>

namespace {

struct HeightQuad {
    float z00;
    float z10;
    float z01;
    float z11;
    float min_bound_z;
    float max_bound_z;
};

[[nodiscard]] float snap_ground_z(const HeightQuad& q, float u, float v) noexcept {
    const float cu = std::clamp(u, 0.0F, 1.0F);
    const float cv = std::clamp(v, 0.0F, 1.0F);
    const float bottom = q.z00 + (q.z10 - q.z00) * cu;
    const float top = q.z01 + (q.z11 - q.z01) * cu;
    const float raw_z = bottom + (top - bottom) * cv;
    return std::clamp(raw_z, q.min_bound_z, q.max_bound_z);
}

struct NavCorridorLimits {
    float water_floor_z;
    float ceiling_z;
    float max_step_up_z;
};

[[nodiscard]] float clamp_corridor_z(
    float current_z,
    float candidate_ground_z,
    const NavCorridorLimits& limits) noexcept {
    const float stepped_z = std::min(candidate_ground_z, current_z + limits.max_step_up_z);
    return std::clamp(stepped_z, limits.water_floor_z, limits.ceiling_z);
}

int test_heightfield_ground_snap_no_z() {
    constexpr HeightQuad kQuad{10.0F, 20.0F, 30.0F, 40.0F, 10.0F, 38.0F};
    SANDAL_ASSERT(std::fabs(snap_ground_z(kQuad, 0.0F, 0.0F) - 10.0F) < 1e-4F);
    SANDAL_ASSERT(std::fabs(snap_ground_z(kQuad, 1.0F, 0.0F) - 20.0F) < 1e-4F);
    SANDAL_ASSERT(std::fabs(snap_ground_z(kQuad, 0.0F, 1.0F) - 30.0F) < 1e-4F);
    SANDAL_ASSERT(std::fabs(snap_ground_z(kQuad, 0.5F, 0.5F) - 25.0F) < 1e-4F);

    SANDAL_ASSERT(std::fabs(snap_ground_z(kQuad, -2.0F, -1.0F) - 10.0F) < 1e-4F);
    SANDAL_ASSERT(std::fabs(snap_ground_z(kQuad, 1.0F, 1.0F) - 38.0F) < 1e-4F);
    SANDAL_ASSERT(std::fabs(snap_ground_z(kQuad, 3.0F, 4.0F) - 38.0F) < 1e-4F);

    const auto* ranged = sandal::test::find_standin_ability(1003U);
    SANDAL_ASSERT(ranged != nullptr);
    const float dz = snap_ground_z(kQuad, 0.5F, 0.5F) - snap_ground_z(kQuad, 0.0F, 0.0F);
    SANDAL_ASSERT(dz == 15.0F && dz <= ranged->range_m);
    return 0;
}

int test_water_floor_clamp_corridor() {
    constexpr NavCorridorLimits kLimits{-2.5F, 50.0F, 1.8F};
    sandal::test::ManualClock clock{0};

    float actor_z = 0.0F;
    clock.advance_ticks(1, 30);
    actor_z = clamp_corridor_z(actor_z, -12.0F, kLimits);
    SANDAL_ASSERT(std::fabs(actor_z - (-2.5F)) < 1e-4F);

    clock.advance_ticks(1, 30);
    actor_z = clamp_corridor_z(actor_z, 10.0F, kLimits);
    SANDAL_ASSERT(std::fabs(actor_z - (-0.7F)) < 1e-4F);

    actor_z = 49.0F;
    clock.advance_ticks(1, 30);
    actor_z = clamp_corridor_z(actor_z, 55.0F, kLimits);
    SANDAL_ASSERT(std::fabs(actor_z - 50.0F) < 1e-4F);
    SANDAL_ASSERT(clock.now_ms() == 99);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "heightfield_ground_snap_no_z")) {
        if (test_heightfield_ground_snap_no_z() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "water_floor_clamp_corridor")) {
        if (test_water_floor_clamp_corridor() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
