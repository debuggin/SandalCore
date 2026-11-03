#include "../world/test_harness.h"

#include "nav/area_nav.h"
#include "tests/nav_fixture.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

nav::VmapArchive make_test_vmap_archive() {
    nav::VmapArchive arch{};
    arch.world_bmin[0] = -1000.0f; arch.world_bmin[1] = -1000.0f; arch.world_bmin[2] = -500.0f;
    arch.world_bmax[0] = 1000.0f;  arch.world_bmax[1] = 1000.0f;  arch.world_bmax[2] = 500.0f;
    for (uint8_t i = 0; i < 32; ++i) arch.content_sha256[i] = static_cast<uint8_t>(i + 1u);

    nav::BvhNode32 water_node{};
    water_node.bmin[0] = -200; water_node.bmin[1] = -200; water_node.bmin[2] = -100;
    water_node.bmax[0] = 200;  water_node.bmax[1] = 200;  water_node.bmax[2] = 0;
    water_node.tri_count = 1;
    water_node.material_flags = nav::kVmapMatWater;
    arch.nodes.push_back(water_node);

    nav::BvhNode32 lava_node{};
    lava_node.bmin[0] = 300; lava_node.bmin[1] = 300; lava_node.bmin[2] = -50;
    lava_node.bmax[0] = 500; lava_node.bmax[1] = 500; lava_node.bmax[2] = 50;
    lava_node.left_or_first_tri = 1;
    lava_node.tri_count = 1;
    lava_node.material_flags = static_cast<uint16_t>(nav::kVmapMatLava | nav::kVmapMatBridge);
    arch.nodes.push_back(lava_node);

    nav::VmapTriangle wall_tri{};
    wall_tri.v0[0] = 0.0f; wall_tri.v0[1] = -100.0f; wall_tri.v0[2] = -100.0f;
    wall_tri.v1[0] = 0.0f; wall_tri.v1[1] = 100.0f;  wall_tri.v1[2] = -100.0f;
    wall_tri.v2[0] = 0.0f; wall_tri.v2[1] = 0.0f;    wall_tri.v2[2] = 200.0f;
    wall_tri.material_flags = nav::kVmapMatOpaqueLos;
    arch.triangles.push_back(wall_tri);

    nav::VmapTriangle foliage_tri{};
    foliage_tri.v0[0] = 50.0f; foliage_tri.v0[1] = -100.0f; foliage_tri.v0[2] = 300.0f;
    foliage_tri.v1[0] = 50.0f; foliage_tri.v1[1] = 100.0f;  foliage_tri.v1[2] = 300.0f;
    foliage_tri.v2[0] = 50.0f; foliage_tri.v2[1] = 0.0f;    foliage_tri.v2[2] = 500.0f;
    foliage_tri.material_flags = nav::kVmapMatFoliageTransparent;
    arch.triangles.push_back(foliage_tri);

    nav::VmapTileTocEntry toc{};
    toc.tile_x = 3;
    toc.tile_y = -2;
    toc.node_count = static_cast<uint32_t>(arch.nodes.size());
    toc.tri_count = static_cast<uint32_t>(arch.triangles.size());
    arch.toc.push_back(toc);
    return arch;
}

int test_vmap_archive_roundtrip_and_crc32_bounds() {
    const nav::VmapArchive src = make_test_vmap_archive();
    const std::string path = "test_aocvmap1_roundtrip.bin";
    std::string err;
    SANDAL_ASSERT(nav::write_vmap_file(path, src, &err) && err.empty());

    std::vector<uint8_t> raw;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        SANDAL_ASSERT(in.good());
        raw.resize(static_cast<size_t>(in.tellg()));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    }
    SANDAL_ASSERT(raw.size() >= nav::kVmapHeaderSize);
    SANDAL_ASSERT(std::memcmp(raw.data(), nav::kVmapMagic, 8) == 0);

    nav::VmapArchive loaded{};
    SANDAL_ASSERT(nav::read_vmap_file(path, &loaded, &err));
    SANDAL_ASSERT(loaded.toc.size() == 1U && loaded.nodes.size() == 2U && loaded.triangles.size() == 2U);
    SANDAL_ASSERT(loaded.toc[0].tile_x == 3 && loaded.toc[0].tile_y == -2 && loaded.toc[0].crc32 != 0U);
    SANDAL_ASSERT(std::memcmp(loaded.content_sha256, src.content_sha256, 32) == 0);

    raw[0] = 'X';
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    }
    SANDAL_ASSERT(!nav::read_vmap_file(path, &loaded, &err) && !err.empty());

    raw[0] = 'A';
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size() - 4U));
    }
    SANDAL_ASSERT(!nav::read_vmap_file(path, &loaded, &err));
    std::remove(path.c_str());
    return 0;
}

int test_vmap_raycast_los_opaque_vs_foliage_and_water_lava_volumes() {
    const nav::NavSettings defaults{};
    SANDAL_ASSERT(defaults.agent_max_climb == 100.0f);
    SANDAL_ASSERT(defaults.max_tiles == 16384);

    const nav::VmapArchive arch = make_test_vmap_archive();
    const float blocked_a[3] = {-100.0f, 0.0f, 0.0f};
    const float blocked_b[3] = {100.0f, 0.0f, 0.0f};
    float hit_t = 1.0f;
    SANDAL_ASSERT(!nav::vmap_raycast_los(arch, blocked_a, blocked_b, &hit_t));
    SANDAL_ASSERT(hit_t > 0.0f && hit_t < 1.0f);

    const float foliage_a[3] = {0.0f, 0.0f, 350.0f};
    const float foliage_b[3] = {100.0f, 0.0f, 350.0f};
    hit_t = 0.0f;
    SANDAL_ASSERT(nav::vmap_raycast_los(arch, foliage_a, foliage_b, &hit_t));
    SANDAL_ASSERT(hit_t == 1.0f);

    const float in_water[3] = {0.0f, 0.0f, -50.0f};
    SANDAL_ASSERT((nav::vmap_query_point_materials(arch, in_water) & nav::kVmapMatWater) != 0U);

    const float in_lava_bridge[3] = {400.0f, 400.0f, 0.0f};
    const uint16_t mats = nav::vmap_query_point_materials(arch, in_lava_bridge);
    SANDAL_ASSERT((mats & nav::kVmapMatLava) != 0U);
    SANDAL_ASSERT((mats & nav::kVmapMatBridge) != 0U);

    const float in_air[3] = {-900.0f, -900.0f, 400.0f};
    SANDAL_ASSERT(nav::vmap_query_point_materials(arch, in_air) == 0U);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "bvh_chunk_bounds_query")) {
        if (test_bvh_chunk_bounds_query() != 0 || test_vmap_archive_roundtrip_and_crc32_bounds() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "detour_polyref64_los_raycast")) {
        if (test_detour_polyref64_los_raycast() != 0 ||
            test_vmap_raycast_los_opaque_vs_foliage_and_water_lava_volumes() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "vmap_archive_roundtrip_and_crc32_bounds")) {
        if (test_vmap_archive_roundtrip_and_crc32_bounds() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "vmap_raycast_los_opaque_vs_foliage_and_water_lava_volumes")) {
        if (test_vmap_raycast_los_opaque_vs_foliage_and_water_lava_volumes() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
