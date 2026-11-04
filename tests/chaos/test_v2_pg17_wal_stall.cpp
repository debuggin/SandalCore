// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "content/content_snapshot.h"
#include "net/spsc_ring.h"
#include "net/tick_loop.h"

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

namespace worldd = aoc::worldd;
namespace content = aoc::worldd::content;

constexpr std::size_t kAsyncCompletionRingCapacity = 1024U;
using WalCompletionRing1024 = worldd::SpscRing<worldd::AsyncTaskCompletion, kAsyncCompletionRingCapacity>;

constexpr std::string_view kDigestGen1 =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestGen2 =
    "2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kDigestGen3 =
    "3333333333333333333333333333333333333333333333333333333333333333";

std::shared_ptr<content::ContentSnapshot> build_snapshot(
    uint64_t version,
    std::string_view digest,
    int64_t item_guid,
    int quality,
    std::string_view label) {
    auto snap = std::make_shared<content::ContentSnapshot>();
    snap->version = version;
    snap->sha256_digest = std::string{digest};
    snap->loaded_at_unix_ms = static_cast<int64_t>(1700000000000ULL + version);
    snap->wal_mode_enabled = true;
    snap->quality_cache.emplace(item_guid, quality);
    snap->name_cache.emplace(item_guid, std::string{label});
    return snap;
}

int test_pg17_wal_stall_saturation_and_fifo_drain() {
    sandal::test::DeterministicPrng prng(0x563257414C53544CULL);
    auto wal_ring = std::make_unique<WalCompletionRing1024>();
    worldd::AsyncTaskCompletionRing default_ring;
    worldd::TickLoopBudget budget;
    uint64_t next_seq = 1U;

    for (uint32_t stall_tick = 0U; stall_tick < 32U; ++stall_tick) {
        for (uint32_t batch = 0U; batch < 32U; ++batch) {
            SANDAL_ASSERT(wal_ring->try_push(worldd::AsyncTaskCompletion{next_seq, worldd::LoginResult{true, "", {}}}));
            if (next_seq <= default_ring.capacity()) {
                SANDAL_ASSERT(default_ring.try_push(worldd::AsyncTaskCompletion{next_seq, worldd::LoginResult{true, "", {}}}));
            }
            ++next_seq;
        }
        const worldd::TickPhaseBudgets stall_phases{1500U, 5200U + (prng.next_u64() % 400ULL), 4100U, 1400U};
        SANDAL_ASSERT(stall_phases.drain_inbound <= 2000U && stall_phases.total_us() < 15000U);
        budget.record_tick_phases(stall_phases);
        SANDAL_ASSERT(!budget.last_exceeded_slo() && !budget.last_overran_tick());
    }
    SANDAL_ASSERT(wal_ring->size_approx() == kAsyncCompletionRingCapacity && wal_ring->dropped_count() == 0U);
    SANDAL_ASSERT(!wal_ring->try_push(worldd::AsyncTaskCompletion{1025U, worldd::LoginResult{true, "", {}}}));
    SANDAL_ASSERT(wal_ring->dropped_count() == 1U && wal_ring->size_approx() == kAsyncCompletionRingCapacity);
    SANDAL_ASSERT(!default_ring.try_push(worldd::AsyncTaskCompletion{257U, worldd::LoginResult{true, "", {}}}));
    SANDAL_ASSERT(default_ring.dropped_count() == 1U);

    uint64_t expected_seq = 1U;
    for (uint32_t drain_tick = 0U; drain_tick < 8U; ++drain_tick) {
        for (uint32_t i = 0U; i < 128U; ++i) {
            worldd::AsyncTaskCompletion ev{};
            SANDAL_ASSERT(wal_ring->try_pop(ev) && ev.session_id == expected_seq && ev.result.accepted);
            ++expected_seq;
        }
        const worldd::TickPhaseBudgets drain_phases{1950U, 5400U, 4200U, 1500U};
        SANDAL_ASSERT(drain_phases.drain_inbound <= 2000U && drain_phases.total_us() < 15000U);
        budget.record_tick_phases(drain_phases);
    }
    SANDAL_ASSERT(expected_seq == 1025U && wal_ring->size_approx() == 0U && wal_ring->empty());
    SANDAL_ASSERT(budget.slo_violation_count() == 0U && budget.overrun_count() == 0U);
    return 0;
}

int test_wait_free_snapshot_isolation_during_stall() {
    auto stalled_ring = std::make_unique<WalCompletionRing1024>();
    for (uint64_t seq = 1U; seq <= kAsyncCompletionRingCapacity; ++seq) {
        SANDAL_ASSERT(stalled_ring->try_push(worldd::AsyncTaskCompletion{seq, worldd::LoginResult{true, "", {}}}));
    }
    content::ContentSnapshotRegistry registry;
    std::string err;
    double swap_ms = -1.0;

    auto snap_v1 = build_snapshot(1U, kDigestGen1, 7001, 3, "Bastion_v1");
    SANDAL_ASSERT(registry.publish(snap_v1, err, &swap_ms) && err.empty());
    SANDAL_ASSERT(swap_ms >= 0.0 && swap_ms < content::kMaxHotReloadSwapMs);
    const std::shared_ptr<const content::ContentSnapshot> reader_pinned = registry.current();
    SANDAL_ASSERT(reader_pinned != nullptr && reader_pinned->version == 1U);

    auto snap_v2 = build_snapshot(2U, kDigestGen2, 7001, 5, "Bastion_v2");
    swap_ms = -1.0;
    SANDAL_ASSERT(registry.publish(snap_v2, err, &swap_ms) && err.empty());
    SANDAL_ASSERT(swap_ms >= 0.0 && swap_ms < content::kMaxHotReloadSwapMs);
    SANDAL_ASSERT(reader_pinned->version == 1U && reader_pinned->item_quality(7001) == 3);
    SANDAL_ASSERT(reader_pinned->item_name(7001) == "Bastion_v1");

    SANDAL_ASSERT(!registry.publish(nullptr, err) && !err.empty());
    SANDAL_ASSERT(!registry.publish(build_snapshot(1U, kDigestGen3, 7001, 9, "Stale_v1"), err) && !err.empty());
    SANDAL_ASSERT(!registry.publish(build_snapshot(2U, kDigestGen3, 7001, 9, "Dup_v2"), err) && !err.empty());
    SANDAL_ASSERT(!registry.publish(build_snapshot(3U, "bad_hex", 7001, 9, "BadSha_v3"), err) && !err.empty());

    const auto active = registry.current();
    SANDAL_ASSERT(active != nullptr && active->version == 2U && active->item_quality(7001) == 5);
    SANDAL_ASSERT(active->item_name(7001) == "Bastion_v2" && stalled_ring->size_approx() == kAsyncCompletionRingCapacity);
    return 0;
}

int run_v2_pg17_wal_stall(const sandal::test::DataFreeConfig&) {
    return (test_pg17_wal_stall_saturation_and_fifo_drain() == 0 &&
            test_wait_free_snapshot_isolation_during_stall() == 0)
               ? 0
               : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "v2_pg17_wal_stall", run_v2_pg17_wal_stall);
}
