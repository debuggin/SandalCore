// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "test_harness.h"

#include "spawns/area.h"
#include "spawns/region_data.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace {

using namespace aoc::worldd;
namespace region = aoc::worldd::spawns::region;

std::filesystem::path resolve_region_fixture(const char* filename) {
    const std::filesystem::path rel = std::filesystem::path("build/gen/fixtures/region") / filename;
    if (std::filesystem::exists(rel)) return rel;
    const std::filesystem::path parent_rel = std::filesystem::path("../build/gen/fixtures/region") / filename;
    if (std::filesystem::exists(parent_rel)) return parent_rel;

    std::filesystem::path dir = std::filesystem::path(__FILE__).parent_path();
    for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
        if (std::filesystem::exists(dir / rel)) return dir / rel;
        if (std::filesystem::exists(dir / parent_rel)) return dir / parent_rel;
        dir = dir.parent_path();
    }
    return rel;
}

int test_json_fixture_matches_generated_region_data() {
    const auto path = resolve_region_fixture("riverlands_rects.json");
    SANDAL_ASSERT(std::filesystem::exists(path));

    std::ifstream in(path);
    SANDAL_ASSERT(in.is_open());
    const nlohmann::json doc = nlohmann::json::parse(in);

    SANDAL_ASSERT(doc.at("cell_cm").get<int32_t>() == region::kCellCm);
    SANDAL_ASSERT(doc.at("count").get<std::size_t>() == region::kRiverlandsRectCount);
    SANDAL_ASSERT(doc.at("area_cells").get<uint32_t>() == region::kRiverlandsAreaCells);

    const auto& rects = doc.at("rects_xy_cm");
    SANDAL_ASSERT(rects.is_array());
    SANDAL_ASSERT(rects.size() == region::kRiverlandsRectCount);

    uint64_t total_cells = 0;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& r = rects[i];
        SANDAL_ASSERT(r.is_array() && r.size() == 4);
        const int32_t x0 = r[0].get<int32_t>();
        const int32_t y0 = r[1].get<int32_t>();
        const int32_t x1 = r[2].get<int32_t>();
        const int32_t y1 = r[3].get<int32_t>();

        SANDAL_ASSERT(x0 == region::kRiverlandsRects[i].x0);
        SANDAL_ASSERT(y0 == region::kRiverlandsRects[i].y0);
        SANDAL_ASSERT(x1 == region::kRiverlandsRects[i].x1);
        SANDAL_ASSERT(y1 == region::kRiverlandsRects[i].y1);
        SANDAL_ASSERT(x0 < x1 && y0 < y1);
        SANDAL_ASSERT(x0 % region::kCellCm == 0 && y0 % region::kCellCm == 0);
        SANDAL_ASSERT(x1 % region::kCellCm == 0 && y1 % region::kCellCm == 0);

        total_cells += static_cast<uint64_t>((x1 - x0) / region::kCellCm) *
                       static_cast<uint64_t>((y1 - y0) / region::kCellCm);
    }
    SANDAL_ASSERT(total_cells == region::kRiverlandsAreaCells);
    return 0;
}

int test_half_open_rect_boundaries_and_index_lookup() {
    const auto& idx = region::Index::get();
    int32_t min_x = region::kRiverlandsRects[0].x0;
    int32_t min_y = region::kRiverlandsRects[0].y0;
    int32_t max_x = region::kRiverlandsRects[0].x1;
    int32_t max_y = region::kRiverlandsRects[0].y1;

    for (std::size_t i = 0; i < region::kRiverlandsRectCount; ++i) {
        const auto& r = region::kRiverlandsRects[i];
        if (r.x0 < min_x) min_x = r.x0;
        if (r.y0 < min_y) min_y = r.y0;
        if (r.x1 > max_x) max_x = r.x1;
        if (r.y1 > max_y) max_y = r.y1;

        SANDAL_ASSERT(idx.contains(static_cast<double>(r.x0), static_cast<double>(r.y0)));
        SANDAL_ASSERT(idx.contains(static_cast<double>(r.x1) - 0.25, static_cast<double>(r.y1) - 0.25));
        SANDAL_ASSERT(idx.zoi_guid(static_cast<double>(r.x0) + 1.0, static_cast<double>(r.y0) + 1.0) != 0);
        SANDAL_ASSERT(idx.zoi_name(static_cast<double>(r.x0) + 1.0, static_cast<double>(r.y0) + 1.0) != nullptr);
    }

    const double mid_x = 0.5 * (static_cast<double>(min_x) + static_cast<double>(max_x));
    const double mid_y = 0.5 * (static_cast<double>(min_y) + static_cast<double>(max_y));
    SANDAL_ASSERT(!idx.contains(static_cast<double>(min_x) - 0.25, mid_y));
    SANDAL_ASSERT(!idx.contains(static_cast<double>(max_x), mid_y));
    SANDAL_ASSERT(!idx.contains(mid_x, static_cast<double>(min_y) - 0.25));
    SANDAL_ASSERT(!idx.contains(mid_x, static_cast<double>(max_y)));
    SANDAL_ASSERT(idx.zoi_guid(static_cast<double>(max_x), mid_y) == 0);
    SANDAL_ASSERT(idx.zoi_name(static_cast<double>(max_x), mid_y) == nullptr);
    return 0;
}

int test_area_config_riverlands_and_non_finite_guards() {
    const auto area = spawns::AreaConfig::riverlands();
    const double sx = bootstrap::kDefaultSpawn.x;
    const double sy = bootstrap::kDefaultSpawn.y;

    SANDAL_ASSERT(area.contains(sx, sy));
    SANDAL_ASSERT(area.zoi_at(sx, sy) == 6064629127135166465ULL);
    SANDAL_ASSERT(area.zoi_name_at(sx, sy) != nullptr);
    SANDAL_ASSERT(std::string_view(area.zoi_name_at(sx, sy)) == "Verra_RVR_StartingArea_RuinsOfAela");

    spawns::AreaConfig no_zones = area;
    no_zones.zones = false;
    SANDAL_ASSERT(no_zones.contains(sx, sy));
    SANDAL_ASSERT(no_zones.zoi_at(sx, sy) == 0);
    SANDAL_ASSERT(no_zones.zoi_name_at(sx, sy) == nullptr);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    SANDAL_ASSERT(!area.contains(nan, sy));
    SANDAL_ASSERT(!area.contains(sx, nan));
    SANDAL_ASSERT(!area.contains(inf, sy));
    SANDAL_ASSERT(!area.contains(-inf, sy));
    SANDAL_ASSERT(!area.contains(1e18, -1e18));
    SANDAL_ASSERT(area.zoi_at(nan, sy) == 0);
    SANDAL_ASSERT(area.zoi_at(inf, -inf) == 0);
    SANDAL_ASSERT(area.zoi_name_at(nan, sy) == nullptr);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "json_fixture_matches_generated_region_data")) {
        if (test_json_fixture_matches_generated_region_data() != 0) return 1;
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "half_open_rect_boundaries_and_index_lookup")) {
        if (test_half_open_rect_boundaries_and_index_lookup() != 0) return 1;
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "area_config_riverlands_and_non_finite_guards")) {
        if (test_area_config_riverlands_and_non_finite_guards() != 0) return 1;
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
