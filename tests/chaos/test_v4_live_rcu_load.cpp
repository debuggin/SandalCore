// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "content/content_snapshot.h"
#include "net/tick_loop.h"
#include "world/actor_registry.h"
#include "world/relevancy.h"

namespace sandal::test {

class DeterministicPrng {
public:
    explicit constexpr DeterministicPrng(uint64_t seed) noexcept
        : state_(seed != 0ULL ? seed : 0x9E3779B97F4A7C15ULL) {}
    [[nodiscard]] constexpr uint64_t next_u64() noexcept {
        uint64_t x = (state_ += 0x9E3779B97F4A7C15ULL);
        x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31U);
    }
    [[nodiscard]] constexpr float next_signed_unit() noexcept {
        const uint32_t bits = static_cast<uint32_t>(next_u64() & 0xFFFFFFULL);
        return (static_cast<float>(bits) / static_cast<float>(0x800000U)) - 1.0F;
    }
private:
    uint64_t state_;
};

template <typename Fn>
[[nodiscard]] int run_case_dispatch(int argc, char** argv, std::string_view case_name, Fn&& fn) {
    const DataFreeConfig cfg = parse_test_cli(argc, argv);
    return should_run_case(cfg, case_name) ? fn(cfg) : 0;
}

}  // namespace sandal::test

namespace {

namespace content = aoc::worldd::content;
namespace world = aoc::worldd::world;
namespace worldd = aoc::worldd;

constexpr std::size_t kActorCount = 128U;
constexpr uint64_t kTotalTicks = 300U;
constexpr int64_t kHotfixItemGuid = 9001;
constexpr std::string_view kDigestEven =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kDigestOdd =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

std::shared_ptr<content::ContentSnapshot> make_hotfix_snapshot(uint64_t version, std::string_view digest) {
    auto snap = std::make_shared<content::ContentSnapshot>();
    snap->version = version;
    snap->sha256_digest = std::string{digest};
    snap->loaded_at_unix_ms = static_cast<int64_t>(1700000000000ULL + version);
    snap->wal_mode_enabled = true;
    snap->quality_cache.emplace(kHotfixItemGuid, static_cast<int>(version * 10U));
    snap->name_cache.emplace(kHotfixItemGuid, "HotfixRelic_v" + std::to_string(version));
    return snap;
}

int populate_actors(world::ActorRegistry& actors, sandal::test::DeterministicPrng& prng) {
    for (std::size_t i = 0U; i < kActorCount; ++i) {
        const uint64_t id = static_cast<uint64_t>((i + 1U) * 2U);
        const float cx = static_cast<float>(static_cast<int32_t>(i % 8U) - 4) * world::kSpatialCellSizeCm;
        const float cy = static_cast<float>(static_cast<int32_t>((i / 8U) % 8U) - 4) * world::kSpatialCellSizeCm;
        world::WorldActor a{};
        a.id = id;
        a.actor.guid.ObjectId = id;
        a.x = cx + (prng.next_signed_unit() * 2500.0F);
        a.y = cy + (prng.next_signed_unit() * 2500.0F);
        a.z = 100.0;
        SANDAL_ASSERT(actors.add(a));
    }
    SANDAL_ASSERT(actors.size() == kActorCount && actors.occupied_cell_count() >= 16U);
    return 0;
}

int execute_simulated_tick(
    uint64_t tick,
    world::ActorRegistry& actors,
    content::ContentSnapshotRegistry& snap_reg,
    sandal::test::DeterministicPrng& prng,
    uint32_t& swap_count) {
    const std::shared_ptr<const content::ContentSnapshot> pinned = snap_reg.current();
    SANDAL_ASSERT(pinned != nullptr);
    const uint64_t pinned_ver = pinned->version;
    const int pinned_qual = pinned->item_quality(kHotfixItemGuid);
    const std::string pinned_name = pinned->item_name(kHotfixItemGuid);

    const uint64_t moved_id = static_cast<uint64_t>(((tick % kActorCount) + 1U) * 2U);
    const world::WorldActor* cur = actors.find(moved_id);
    SANDAL_ASSERT(cur != nullptr);
    const double nx = cur->x + static_cast<double>(prng.next_signed_unit() * 120.0F);
    const double ny = cur->y + static_cast<double>(prng.next_signed_unit() * 120.0F);
    SANDAL_ASSERT(actors.move(moved_id, nx, ny, cur->z, 0.0));

    if (tick > 0U && (tick % 10U) == 0U) {
        const uint64_t next_ver = (tick / 10U) + 1U;
        const std::string_view digest = (next_ver % 2U == 0U) ? kDigestEven : kDigestOdd;
        std::string err;
        double swap_ms = -1.0;
        SANDAL_ASSERT(snap_reg.publish(make_hotfix_snapshot(next_ver, digest), err, &swap_ms) && err.empty());
        SANDAL_ASSERT(swap_ms >= 0.0 && swap_ms < content::kMaxHotReloadSwapMs);
        SANDAL_ASSERT(snap_reg.current() != nullptr && snap_reg.current()->version == next_ver);
        ++swap_count;
    }

    std::array<uint64_t, kActorCount> aoi_ids{};
    const std::size_t gathered = actors.gather_aoi_candidates(
        static_cast<float>(nx), static_cast<float>(ny), 2, aoi_ids.data(), aoi_ids.size());
    SANDAL_ASSERT(gathered > 0U && gathered == actors.count_actors_in_cell_ring(
        static_cast<float>(nx), static_cast<float>(ny), 2));
    SANDAL_ASSERT(pinned->version == pinned_ver && pinned->item_quality(kHotfixItemGuid) == pinned_qual);
    SANDAL_ASSERT(pinned->item_name(kHotfixItemGuid) == pinned_name);
    return 0;
}

int test_live_rcu_300_ticks_and_30_swaps(content::ContentSnapshotRegistry& snap_reg, world::ActorRegistry& actors) {
    sandal::test::DeterministicPrng prng(0x56345243554C4F44ULL);
    SANDAL_ASSERT(populate_actors(actors, prng) == 0);
    worldd::TickLoopBudget budget;
    uint64_t p99_max_us = 0U;
    uint32_t swap_count = 0U;
    for (uint64_t tick = 0U; tick < kTotalTicks; ++tick) {
        if (tick == 0U) {
            const uint64_t first_ver = 1U;
            std::string err;
            double swap_ms = -1.0;
            SANDAL_ASSERT(snap_reg.publish(make_hotfix_snapshot(first_ver, kDigestOdd), err, &swap_ms) && err.empty());
            SANDAL_ASSERT(swap_ms >= 0.0 && swap_ms < content::kMaxHotReloadSwapMs);
            ++swap_count;
        }
        const std::shared_ptr<const content::ContentSnapshot> pre_tick = snap_reg.current();
        SANDAL_ASSERT(pre_tick != nullptr && pre_tick->version == ((tick == 0U) ? 1U : ((tick - 1U) / 10U) + 1U));
        const uint32_t swaps_before = swap_count;
        SANDAL_ASSERT(execute_simulated_tick(tick, actors, snap_reg, prng, swap_count) == 0);
        const uint32_t expected_delta = (tick > 0U && (tick % 10U) == 0U) ? 1U : 0U;
        SANDAL_ASSERT((swap_count - swaps_before) == expected_delta);
        SANDAL_ASSERT(snap_reg.current()->version == (tick / 10U) + 1U);
        const worldd::TickPhaseBudgets phases{1100U + (tick % 300U), 4200U + (tick % 500U), 3100U, 950U};
        budget.record_tick_phases(phases);
        if (phases.total_us() > p99_max_us) {
            p99_max_us = phases.total_us();
        }
        SANDAL_ASSERT(!budget.last_exceeded_slo() && !budget.last_overran_tick());
    }
    SANDAL_ASSERT(swap_count == 30U && snap_reg.current()->version == 30U);
    SANDAL_ASSERT(budget.tick_index() == kTotalTicks && p99_max_us < worldd::TickLoopBudget::kTickSloP99Us);
    SANDAL_ASSERT(budget.slo_violation_count() == 0U && budget.overrun_count() == 0U);
    return 0;
}

int test_corrupt_and_stale_hotfix_rejection(content::ContentSnapshotRegistry& snap_reg, world::ActorRegistry& actors) {
    const auto pinned = snap_reg.current();
    SANDAL_ASSERT(pinned != nullptr && pinned->version == 30U);
    std::string err;
    double swap_ms = -1.0;
    SANDAL_ASSERT(!snap_reg.publish(nullptr, err, &swap_ms) && !err.empty());
    SANDAL_ASSERT(!snap_reg.publish(make_hotfix_snapshot(0U, kDigestEven), err, &swap_ms) && !err.empty());
    SANDAL_ASSERT(!snap_reg.publish(make_hotfix_snapshot(31U, "bad_sha256"), err, &swap_ms) && !err.empty());
    SANDAL_ASSERT(!snap_reg.publish(make_hotfix_snapshot(29U, kDigestEven), err, &swap_ms) && !err.empty());
    SANDAL_ASSERT(!snap_reg.publish(make_hotfix_snapshot(30U, kDigestEven), err, &swap_ms) && !err.empty());
    SANDAL_ASSERT(snap_reg.current() == pinned && snap_reg.current()->item_quality(kHotfixItemGuid) == 300);
    SANDAL_ASSERT(actors.size() == kActorCount && actors.find(2U) != nullptr);
    return 0;
}

int run_v4_live_rcu_load(const sandal::test::DataFreeConfig&) {
    content::ContentSnapshotRegistry snap_reg;
    world::ActorRegistry actors;
    return (test_live_rcu_300_ticks_and_30_swaps(snap_reg, actors) == 0 &&
            test_corrupt_and_stale_hotfix_rejection(snap_reg, actors) == 0)
               ? 0
               : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "v4_live_rcu_load", run_v4_live_rcu_load);
}
