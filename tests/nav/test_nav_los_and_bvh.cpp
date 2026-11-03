#include "../world/test_harness.h"

#include "nav/area_nav.h"
#include "tests/nav_fixture.h"

#include <cmath>
#include <limits>

namespace {

namespace nav = aoc::worldd::nav;
namespace navfix = aoc::worldd::navfix;

std::unique_ptr<nav::AreaNav> build_bounded_region_nav(std::string& error) {
    nav::NavFile file = navfix::build_file(2);
    file.region.push_back(nav::RegionRect{-10000.0, -10000.0, 10000.0, 10000.0});
    return nav::AreaNav::from_file(std::move(file), &error);
}

int test_bvh_chunk_bounds_query() {
    SANDAL_ASSERT(nav::floor_div(0, 8) == 0 && nav::floor_div(7, 8) == 0);
    SANDAL_ASSERT(nav::floor_div(8, 8) == 1 && nav::floor_div(-1, 8) == -1);
    SANDAL_ASSERT(nav::floor_div(-8, 8) == -1 && nav::floor_div(-9, 8) == -2);

    const auto chunk = nav::chunk_of_tile(15, -3);
    SANDAL_ASSERT(chunk.x == 1 && chunk.y == -1);

    std::string err;
    auto area = build_bounded_region_nav(err);
    SANDAL_ASSERT(area != nullptr && err.empty());
    SANDAL_ASSERT(area->has_region() && !area->windowed());
    SANDAL_ASSERT(area->tile_count() >= 4U && area->polygon_count() >= 10U);
    SANDAL_ASSERT(area->in_region(0.0, 0.0) && area->in_region(-10000.0, 9999.0));
    SANDAL_ASSERT(!area->in_region(10000.0, 0.0) && !area->in_region(0.0, -10000.5));
    SANDAL_ASSERT(area->ready_at(-1000.0, 0.0));
    SANDAL_ASSERT(area->chunk_resident({0, 0}));

    const auto c_pos = area->chunk_at(1000.0, 1000.0);
    const auto c_far = area->chunk_at(-35000.0, -35000.0);
    SANDAL_ASSERT(c_pos != c_far);

    nav::Vec3 sample{};
    SANDAL_ASSERT(area->random_point({0.0, 0.0, 1000.0}, 800.0, 0.25, 0.5, &sample));
    SANDAL_ASSERT(area->in_region(sample.x, sample.y));
    SANDAL_ASSERT(area->on_navmesh(sample));
    return 0;
}

int verify_detour_corridor_pathfinding(const nav::AreaNav& area) {
    const nav::Vec3 west{-3500.0, 0.0, 1000.0};
    const nav::Vec3 east{-2500.0, 0.0, 1000.0};
    const auto around = area.find_path(west, east);
    SANDAL_ASSERT(around.found && around.complete && around.points.size() >= 3U);
    SANDAL_ASSERT(around.fail == nav::PathResult::Fail::None);

    double max_abs_y = 0.0;
    for (const auto& pt : around.points) {
        if (std::fabs(pt.y) > max_abs_y) {
            max_abs_y = std::fabs(pt.y);
        }
    }
    SANDAL_ASSERT(max_abs_y > 1900.0);

    const nav::Vec3 inside_pen{-8400.0, 6600.0, 1000.0};
    const auto into_pen = area.find_path(west, inside_pen);
    SANDAL_ASSERT(into_pen.found && !into_pen.complete);
    SANDAL_ASSERT(into_pen.fail == nav::PathResult::Fail::Unreachable);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    SANDAL_ASSERT(!area.find_path({nan, 0.0, 1000.0}, east).found);
    SANDAL_ASSERT(!area.find_path({50000.0, 50000.0, 1000.0}, east).found);
    return 0;
}

int test_detour_polyref64_los_raycast() {
    const nav::AreaNav& area = navfix::shared_nav();

    const nav::Vec3 clear_a{-1000.0, -500.0, 1000.0};
    const nav::Vec3 clear_b{-1000.0, 500.0, 1000.0};
    nav::Vec3 hit{};
    SANDAL_ASSERT(area.line_of_sight(clear_a, clear_b));
    SANDAL_ASSERT(area.raycast(clear_a, clear_b, &hit));

    const nav::Vec3 wall_a{-3500.0, 0.0, 1000.0};
    const nav::Vec3 wall_b{-2500.0, 0.0, 1000.0};
    SANDAL_ASSERT(!area.line_of_sight(wall_a, wall_b));
    SANDAL_ASSERT(!area.raycast(wall_a, wall_b, &hit));
    SANDAL_ASSERT(hit.x > -3500.0 && hit.x < -3050.0);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    SANDAL_ASSERT(!area.raycast({nan, 0.0, 1000.0}, wall_b, &hit));
    return verify_detour_corridor_pathfinding(area);
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "bvh_chunk_bounds_query")) {
        if (test_bvh_chunk_bounds_query() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "detour_polyref64_los_raycast")) {
        if (test_detour_polyref64_los_raycast() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
