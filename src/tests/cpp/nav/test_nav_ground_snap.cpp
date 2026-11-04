#include "../world/test_harness.h"

#include "nav/area_nav.h"
#include "tests/nav_fixture.h"

#include <cmath>
#include <limits>
#include <vector>

namespace {

namespace nav = aoc::worldd::nav;
namespace navfix = aoc::worldd::navfix;

int verify_terrain_grid_corridor_clamp() {
    nav::TerrainGrid grid{};
    grid.x0 = 0.0;
    grid.y0 = 0.0;
    grid.step_x = 100.0;
    grid.step_y = 100.0;
    grid.z_base = 0.0;
    grid.z_scale = 1.0;
    grid.nx = 2;
    grid.ny = 2;
    grid.raw = {
        static_cast<uint16_t>(32768 - 250),
        static_cast<uint16_t>(32768 + 100),
        static_cast<uint16_t>(32768 + 300),
        static_cast<uint16_t>(32768 + 500),
    };

    double z00 = 0.0;
    double z10 = 0.0;
    double z01 = 0.0;
    double z11 = 0.0;
    double z_mid = 0.0;
    SANDAL_ASSERT(grid.height(0.0, 0.0, &z00) && std::fabs(z00 - (-250.0)) < 1e-3);
    SANDAL_ASSERT(grid.height(100.0, 0.0, &z10) && std::fabs(z10 - 100.0) < 1e-3);
    SANDAL_ASSERT(grid.height(0.0, 100.0, &z01) && std::fabs(z01 - 300.0) < 1e-3);
    SANDAL_ASSERT(grid.height(100.0, 100.0, &z11) && std::fabs(z11 - 500.0) < 1e-3);
    SANDAL_ASSERT(grid.height(50.0, 50.0, &z_mid) && std::fabs(z_mid - 125.0) < 1e-3);

    double z_out = 0.0;
    SANDAL_ASSERT(!grid.height(-1.0, 50.0, &z_out));
    SANDAL_ASSERT(!grid.height(101.0, 50.0, &z_out));
    SANDAL_ASSERT(!grid.height(std::numeric_limits<double>::quiet_NaN(), 0.0, &z_out));
    return 0;
}

int test_heightfield_ground_snap_no_z() {
    const nav::AreaNav& area = navfix::shared_nav();
    SANDAL_ASSERT(area.tile_count() > 0U && area.terrain_grids() == 16U);
    SANDAL_ASSERT(area.polygon_count() > 0U);

    double tz_origin = 0.0;
    double tz_peak = 0.0;
    double tz_slope = 0.0;
    SANDAL_ASSERT(area.terrain_height(0.0, 0.0, &tz_origin));
    SANDAL_ASSERT(std::fabs(tz_origin - 1000.0) < 1.0);
    SANDAL_ASSERT(area.terrain_height(6000.0, 0.0, &tz_peak));
    SANDAL_ASSERT(std::fabs(tz_peak - 1400.0) < 2.0);
    SANDAL_ASSERT(area.terrain_height(4500.0, 0.0, &tz_slope));
    SANDAL_ASSERT(std::fabs(tz_slope - 1200.0) < 5.0);

    double tz_oob = 0.0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    SANDAL_ASSERT(!area.terrain_height(50000.0, 0.0, &tz_oob));
    SANDAL_ASSERT(!area.terrain_height(nan, 0.0, &tz_oob));

    double gz_flat = 0.0;
    double gz_far_hint = 0.0;
    SANDAL_ASSERT(area.ground_height(0.0, 0.0, 1000.0, &gz_flat));
    SANDAL_ASSERT(!area.ground_height(0.0, 0.0, 5000.0, &gz_far_hint));
    SANDAL_ASSERT(area.ground_height(0.0, 0.0, 5000.0, &gz_far_hint, 4500.0, 150.0));
    SANDAL_ASSERT(std::fabs(gz_flat - 1000.0) < 20.0);
    SANDAL_ASSERT(std::fabs(gz_far_hint - 1000.0) < 20.0);
    SANDAL_ASSERT(!area.ground_height(50000.0, 50000.0, 1000.0, &gz_flat));
    SANDAL_ASSERT(!area.ground_height(nan, 0.0, 1000.0, &gz_flat));
    return 0;
}

int test_water_floor_clamp_corridor() {
    const nav::AreaNav& area = navfix::shared_nav();

    const std::vector<double> floors = area.floors_at(9000.0, -5000.0, 500.0, 2000.0);
    SANDAL_ASSERT(floors.size() >= 2U);
    SANDAL_ASSERT(floors.front() > floors.back());
    SANDAL_ASSERT(std::fabs(floors.front() - 1540.0) < 30.0);
    SANDAL_ASSERT(std::fabs(floors.back() - 1000.0) < 30.0);

    double gz_low = 0.0;
    double gz_high = 0.0;
    SANDAL_ASSERT(area.ground_height(9000.0, -5000.0, 1010.0, &gz_low));
    SANDAL_ASSERT(area.ground_height(9000.0, -5000.0, 1535.0, &gz_high));
    SANDAL_ASSERT(std::fabs(gz_low - 1000.0) < 30.0);
    SANDAL_ASSERT(std::fabs(gz_high - 1540.0) < 30.0);
    SANDAL_ASSERT((gz_high - gz_low) > 450.0);

    SANDAL_ASSERT(area.on_navmesh({0.0, 0.0, 1000.0}));
    SANDAL_ASSERT(!area.on_navmesh({0.0, 0.0, 1200.0}));

    nav::Vec3 nearest{};
    SANDAL_ASSERT(area.nearest_point({0.0, 0.0, 1200.0}, 200.0, 300.0, &nearest));
    SANDAL_ASSERT(std::fabs(nearest.z - 1000.0) < 20.0);
    SANDAL_ASSERT(area.on_navmesh(nearest));
    return verify_terrain_grid_corridor_clamp();
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
