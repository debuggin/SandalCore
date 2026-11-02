#include "../world/test_harness.h"

#include <algorithm>
#include <cstdint>

namespace {

struct Point3 {
    float x;
    float y;
    float z;
};

struct Aabb3 {
    Point3 min;
    Point3 max;
};

[[nodiscard]] bool aabb_contains_point(const Aabb3& box, Point3 p) noexcept {
    return p.x >= box.min.x && p.x <= box.max.x &&
           p.y >= box.min.y && p.y <= box.max.y &&
           p.z >= box.min.z && p.z <= box.max.z;
}

[[nodiscard]] bool aabb_intersects_sphere(const Aabb3& box, Point3 center, float radius) noexcept {
    if (radius < 0.0F) {
        return false;
    }
    const float cx = std::clamp(center.x, box.min.x, box.max.x) - center.x;
    const float cy = std::clamp(center.y, box.min.y, box.max.y) - center.y;
    const float cz = std::clamp(center.z, box.min.z, box.max.z) - center.z;
    return (cx * cx + cy * cy + cz * cz) <= (radius * radius);
}

struct DecodedPolyRef {
    bool valid{false};
    uint32_t salt{0};
    uint32_t tile_id{0};
    uint32_t poly_index{0};
};

[[nodiscard]] constexpr uint64_t encode_poly_ref64(
    uint32_t salt,
    uint32_t tile_id,
    uint32_t poly_index) noexcept {
    return (static_cast<uint64_t>(salt & 0xFFFFU) << 48U) |
           (static_cast<uint64_t>(tile_id & 0xFFFFFFU) << 24U) |
           static_cast<uint64_t>(poly_index & 0xFFFFFFU);
}

[[nodiscard]] constexpr DecodedPolyRef decode_poly_ref64(uint64_t ref) noexcept {
    if (ref == 0ULL) {
        return {};
    }
    return {
        true,
        static_cast<uint32_t>((ref >> 48U) & 0xFFFFU),
        static_cast<uint32_t>((ref >> 24U) & 0xFFFFFFU),
        static_cast<uint32_t>(ref & 0xFFFFFFU),
    };
}

struct Vec2 {
    float x;
    float y;
};

[[nodiscard]] constexpr float cross_2d(Vec2 a, Vec2 b, Vec2 c) noexcept {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

[[nodiscard]] constexpr bool segments_intersect_2d(Vec2 a, Vec2 b, Vec2 c, Vec2 d) noexcept {
    const float d1 = cross_2d(c, d, a);
    const float d2 = cross_2d(c, d, b);
    const float d3 = cross_2d(a, b, c);
    const float d4 = cross_2d(a, b, d);
    return ((d1 > 0.0F && d2 < 0.0F) || (d1 < 0.0F && d2 > 0.0F)) &&
           ((d3 > 0.0F && d4 < 0.0F) || (d3 < 0.0F && d4 > 0.0F));
}

int test_bvh_chunk_bounds_query() {
    constexpr Aabb3 kChunk{{-64.0F, -64.0F, 0.0F}, {64.0F, 64.0F, 128.0F}};
    SANDAL_ASSERT(aabb_contains_point(kChunk, {0.0F, 0.0F, 64.0F}));
    SANDAL_ASSERT(aabb_contains_point(kChunk, {64.0F, -64.0F, 128.0F}));
    SANDAL_ASSERT(!aabb_contains_point(kChunk, {64.1F, 0.0F, 64.0F}));
    SANDAL_ASSERT(!aabb_contains_point(kChunk, {0.0F, 0.0F, -0.1F}));

    const auto* spell = sandal::test::find_standin_ability(1002U);
    SANDAL_ASSERT(spell != nullptr);
    SANDAL_ASSERT(aabb_intersects_sphere(kChunk, {80.0F, 0.0F, 32.0F}, spell->range_m));
    SANDAL_ASSERT(!aabb_intersects_sphere(kChunk, {90.0F, 0.0F, 32.0F}, spell->range_m));
    SANDAL_ASSERT(!aabb_intersects_sphere(kChunk, {0.0F, 0.0F, 32.0F}, -1.0F));
    return 0;
}

int test_detour_polyref64_los_raycast() {
    SANDAL_ASSERT(!decode_poly_ref64(0ULL).valid);

    const uint64_t ref = encode_poly_ref64(42U, 1024U, 511U);
    const auto decoded = decode_poly_ref64(ref);
    SANDAL_ASSERT(decoded.valid);
    SANDAL_ASSERT(decoded.salt == 42U);
    SANDAL_ASSERT(decoded.tile_id == 1024U);
    SANDAL_ASSERT(decoded.poly_index == 511U);

    constexpr Vec2 kWallA{10.0F, -5.0F};
    constexpr Vec2 kWallB{10.0F, 5.0F};
    SANDAL_ASSERT(segments_intersect_2d({0.0F, 0.0F}, {20.0F, 0.0F}, kWallA, kWallB));
    SANDAL_ASSERT(!segments_intersect_2d({0.0F, 8.0F}, {20.0F, 8.0F}, kWallA, kWallB));
    SANDAL_ASSERT(!segments_intersect_2d({0.0F, 0.0F}, {9.5F, 0.0F}, kWallA, kWallB));
    return 0;
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
