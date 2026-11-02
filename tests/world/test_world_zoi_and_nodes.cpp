#include "test_harness.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace {

struct ZoiCellCoord {
    int32_t x{0};
    int32_t y{0};
};

[[nodiscard]] ZoiCellCoord world_to_zoi_cell(float world_x, float world_y, float cell_size_m) noexcept {
    if (cell_size_m <= 0.0F) {
        return {0, 0};
    }
    return {
        static_cast<int32_t>(std::floor(world_x / cell_size_m)),
        static_cast<int32_t>(std::floor(world_y / cell_size_m)),
    };
}

[[nodiscard]] bool is_zoi_neighbor(ZoiCellCoord a, ZoiCellCoord b) noexcept {
    const int32_t dx = std::abs(a.x - b.x);
    const int32_t dy = std::abs(a.y - b.y);
    return dx <= 1 && dy <= 1;
}

struct CityNodeState {
    uint8_t stage{0};
    int64_t xp{0};
    int64_t last_promotion_ms{0};
};

[[nodiscard]] bool contribute_node_xp(CityNodeState& node, int64_t delta_xp, int64_t now_ms) noexcept {
    constexpr std::array<int64_t, 4> kStageThresholds{0, 500, 1500, 4000};
    constexpr int64_t kPromotionLockoutMs = 60'000;
    if (delta_xp <= 0) {
        return false;
    }
    node.xp += delta_xp;
    if (node.stage + 1U < kStageThresholds.size() &&
        node.xp >= kStageThresholds[node.stage + 1U] &&
        (now_ms - node.last_promotion_ms) >= kPromotionLockoutMs) {
        ++node.stage;
        node.last_promotion_ms = now_ms;
        return true;
    }
    return false;
}

int test_zoi_spatial_cell_partition() {
    constexpr float kCellSize = 64.0F;
    const auto origin = world_to_zoi_cell(0.0F, 0.0F, kCellSize);
    SANDAL_ASSERT(origin.x == 0 && origin.y == 0);

    const auto edge = world_to_zoi_cell(63.9F, 64.0F, kCellSize);
    SANDAL_ASSERT(edge.x == 0 && edge.y == 1);

    const auto neg = world_to_zoi_cell(-0.1F, -64.1F, kCellSize);
    SANDAL_ASSERT(neg.x == -1 && neg.y == -2);

    const auto invalid = world_to_zoi_cell(128.0F, 128.0F, 0.0F);
    SANDAL_ASSERT(invalid.x == 0 && invalid.y == 0);

    SANDAL_ASSERT(is_zoi_neighbor(origin, edge));
    SANDAL_ASSERT(is_zoi_neighbor(origin, {-1, -1}));
    SANDAL_ASSERT(!is_zoi_neighbor(origin, neg));
    SANDAL_ASSERT(!is_zoi_neighbor(origin, {2, 0}));
    return 0;
}

int test_city_node_stage_progression() {
    sandal::test::ManualClock clock{0};
    CityNodeState node{};

    SANDAL_ASSERT(!contribute_node_xp(node, 0, clock.now_ms()));
    SANDAL_ASSERT(!contribute_node_xp(node, -50, clock.now_ms()));
    SANDAL_ASSERT(node.xp == 0 && node.stage == 0U);

    clock.advance_ms(60'000);
    SANDAL_ASSERT(!contribute_node_xp(node, 499, clock.now_ms()));
    SANDAL_ASSERT(node.stage == 0U);
    SANDAL_ASSERT(contribute_node_xp(node, 1, clock.now_ms()));
    SANDAL_ASSERT(node.stage == 1U && node.xp == 500);

    clock.advance_ms(30'000);
    SANDAL_ASSERT(!contribute_node_xp(node, 1000, clock.now_ms()));
    SANDAL_ASSERT(node.stage == 1U && node.xp == 1500);

    clock.advance_ms(30'000);
    SANDAL_ASSERT(contribute_node_xp(node, 1, clock.now_ms()));
    SANDAL_ASSERT(node.stage == 2U && node.xp == 1501);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "zoi_spatial_cell_partition")) {
        if (test_zoi_spatial_cell_partition() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "city_node_stage_progression")) {
        if (test_city_node_stage_progression() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
