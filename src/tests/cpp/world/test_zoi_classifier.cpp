// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "test_harness.h"

#include "spawns/region_data.h"
#include "spawns/region_index.h"
#include "world/zoi_surface.h"
#include "world/zoi_surface_table.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace {

using namespace aoc::worldd;
namespace region = aoc::worldd::spawns::region;

bool open_zoi_fixture(std::ifstream& in, const char* filename) {
    const std::string rel = std::string("build/gen/fixtures/region/") + filename;
    in.open(rel);
    if (in.is_open()) return true;
    in.clear();
    const std::string parent_rel = std::string("../build/gen/fixtures/region/") + filename;
    in.open(parent_rel);
    if (in.is_open()) return true;
    in.clear();

    std::string dir = __FILE__;
    for (int depth = 0; depth < 6; ++depth) {
        const auto pos = dir.find_last_of("/\\");
        if (pos == std::string::npos) break;
        dir.resize(pos);
        in.open(dir + "/" + rel);
        if (in.is_open()) return true;
        in.clear();
        in.open(dir + "/" + parent_rel);
        if (in.is_open()) return true;
        in.clear();
    }
    return false;
}

int verify_single_zoi_entry(const nlohmann::json& z, std::size_t idx, std::size_t& rect_cursor, uint64_t& total_cells) {
    const auto& hdr = region::kZois[idx];
    SANDAL_ASSERT(z.at("guid").get<uint64_t>() == hdr.guid);
    SANDAL_ASSERT(z.at("name").get<std::string>() == std::string(hdr.name));
    SANDAL_ASSERT(z.at("cells").get<uint32_t>() == hdr.cells);
    SANDAL_ASSERT(hdr.first == rect_cursor);

    const auto& rects = z.at("rects_xy_cm");
    SANDAL_ASSERT(rects.is_array() && rects.size() == hdr.count);

    uint64_t zoi_cells = 0;
    for (std::size_t k = 0; k < rects.size(); ++k) {
        const auto& r = rects[k];
        const auto& hr = region::kZoiRects[hdr.first + k];
        SANDAL_ASSERT(r[0].get<int32_t>() == hr.x0 && r[1].get<int32_t>() == hr.y0);
        SANDAL_ASSERT(r[2].get<int32_t>() == hr.x1 && r[3].get<int32_t>() == hr.y1);
        zoi_cells += static_cast<uint64_t>((hr.x1 - hr.x0) / region::kCellCm) *
                     static_cast<uint64_t>((hr.y1 - hr.y0) / region::kCellCm);
    }
    SANDAL_ASSERT(zoi_cells == hdr.cells);
    rect_cursor += hdr.count;
    total_cells += zoi_cells;
    return 0;
}

int test_json_zoi_fixture_matches_generated_region_data() {
    std::ifstream in;
    SANDAL_ASSERT(open_zoi_fixture(in, "riverlands_zoi_rects.json"));
    const nlohmann::json doc = nlohmann::json::parse(in);

    SANDAL_ASSERT(doc.at("cell_cm").get<int32_t>() == region::kCellCm);
    const auto& zois = doc.at("zois");
    SANDAL_ASSERT(zois.is_array() && zois.size() == region::kZoiCount);

    std::size_t rect_cursor = 0;
    uint64_t total_cells = 0;
    for (std::size_t i = 0; i < zois.size(); ++i) {
        if (verify_single_zoi_entry(zois[i], i, rect_cursor, total_cells) != 0) return 1;
    }
    SANDAL_ASSERT(rect_cursor == region::kZoiRectCount);
    SANDAL_ASSERT(total_cells == region::kRiverlandsAreaCells);
    return 0;
}

int test_zoi_surface_table_agrees_with_zoi_rects_fixture() {
    const auto info = world::zoi_surface_info();
    SANDAL_ASSERT(info.chunks == 4635);
    SANDAL_ASSERT(info.uniform == 3240);
    SANDAL_ASSERT(world::zoi_surface::kChunkCm == 32768);
    SANDAL_ASSERT(world::zoi_surface::kCellCm == 512);
    SANDAL_ASSERT(world::zoi_surface::kColumns == 76);
    SANDAL_ASSERT(world::zoi_surface::kRowsOfChunks == 79);

    const auto palette = world::zoi_surface_palette();
    SANDAL_ASSERT(palette.size() == 58);
    SANDAL_ASSERT(palette[0] == 0ULL);
    for (std::size_t i = 1; i < palette.size(); ++i) {
        SANDAL_ASSERT(palette[i - 1] < palette[i]);
        SANDAL_ASSERT(palette[i] == world::zoi_surface::kPalette[i]);
    }

    const auto& idx = region::Index::get();
    const double half = 0.5 * static_cast<double>(region::kCellCm);
    for (std::size_t z = 0; z < region::kZoiCount; ++z) {
        const auto& zoi = region::kZois[z];
        for (uint32_t k = 0; k < zoi.count; ++k) {
            const auto& r = region::kZoiRects[zoi.first + k];
            const double x_lo = static_cast<double>(r.x0) + half;
            const double y_lo = static_cast<double>(r.y0) + half;
            const double x_hi = static_cast<double>(r.x1) - half;
            const double y_hi = static_cast<double>(r.y1) - half;

            SANDAL_ASSERT(world::zoi_at(x_lo, y_lo) == zoi.guid);
            SANDAL_ASSERT(world::zoi_at(x_hi, y_hi) == zoi.guid);
            SANDAL_ASSERT(idx.zoi_guid(x_lo, y_lo) == zoi.guid);
            SANDAL_ASSERT(idx.zoi_guid(x_hi, y_hi) == zoi.guid);
        }
    }
    return 0;
}

int test_zoi_surface_boundary_and_adversarial_inputs() {
    const double out_west_x = static_cast<double>(world::zoi_surface::kMinCx - 1) * world::zoi_surface::kChunkCm;
    const double out_east_x = static_cast<double>(world::zoi_surface::kMinCx + world::zoi_surface::kColumns + 1) *
                              world::zoi_surface::kChunkCm;
    const double out_south_y = static_cast<double>(world::zoi_surface::kMinCy - 1) * world::zoi_surface::kChunkCm;
    const double out_north_y = static_cast<double>(world::zoi_surface::kMinCy + world::zoi_surface::kRowsOfChunks + 1) *
                               world::zoi_surface::kChunkCm;

    SANDAL_ASSERT(world::zoi_at(out_west_x, 0.0) == 0ULL);
    SANDAL_ASSERT(world::zoi_at(out_east_x, 0.0) == 0ULL);
    SANDAL_ASSERT(world::zoi_at(0.0, out_south_y) == 0ULL);
    SANDAL_ASSERT(world::zoi_at(0.0, out_north_y) == 0ULL);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    SANDAL_ASSERT(world::zoi_at(nan, 0.0) == 0ULL);
    SANDAL_ASSERT(world::zoi_at(0.0, nan) == 0ULL);
    SANDAL_ASSERT(world::zoi_at(inf, -inf) == 0ULL);
    SANDAL_ASSERT(world::zoi_at(1e20, -1e20) == 0ULL);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "json_zoi_fixture_matches_generated_region_data")) {
        if (test_json_zoi_fixture_matches_generated_region_data() != 0) return 1;
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "zoi_surface_table_agrees_with_zoi_rects_fixture")) {
        if (test_zoi_surface_table_agrees_with_zoi_rects_fixture() != 0) return 1;
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "zoi_surface_boundary_and_adversarial_inputs")) {
        if (test_zoi_surface_boundary_and_adversarial_inputs() != 0) return 1;
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
