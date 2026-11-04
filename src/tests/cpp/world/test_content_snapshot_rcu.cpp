// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "content/content_snapshot.h"

namespace sandal::test {

template <typename Fn>
[[nodiscard]] int run_case_dispatch(int argc, char** argv, std::string_view case_name, Fn&& fn) {
    const DataFreeConfig cfg = parse_test_cli(argc, argv);
    if (!should_run_case(cfg, case_name)) {
        return 0;
    }
    return fn(cfg);
}

}  // namespace sandal::test

namespace {

constexpr std::string_view kValidDigestV1 =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kValidDigestUpper =
    "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789";

std::shared_ptr<aoc::worldd::content::ContentSnapshot> make_test_snapshot(
    uint64_t version,
    std::string_view digest,
    int64_t item_id,
    int32_t quality,
    std::string_view item_label) {
    auto snap = std::make_shared<aoc::worldd::content::ContentSnapshot>();
    snap->version = version;
    snap->sha256_digest = std::string{digest};
    snap->loaded_at_unix_ms = static_cast<int64_t>(1700000000000ULL + version);
    snap->wal_mode_enabled = true;
    snap->quality_cache.emplace(item_id, quality);
    snap->rarity_cache.emplace(item_id, quality + 1);
    snap->name_cache.emplace(item_id, std::string{item_label});
    return snap;
}

int test_sha256_hex_validation() {
    using aoc::worldd::content::is_valid_sha256_hex;
    SANDAL_ASSERT(is_valid_sha256_hex(kValidDigestV1));
    SANDAL_ASSERT(is_valid_sha256_hex(kValidDigestUpper));
    SANDAL_ASSERT(!is_valid_sha256_hex(""));
    SANDAL_ASSERT(!is_valid_sha256_hex(kValidDigestV1.substr(0, 63)));
    SANDAL_ASSERT(!is_valid_sha256_hex(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0"));
    SANDAL_ASSERT(!is_valid_sha256_hex(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"));
    SANDAL_ASSERT(!is_valid_sha256_hex(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde/"));
    return 0;
}

int test_publish_rejection_guards() {
    using namespace aoc::worldd::content;
    ContentSnapshotRegistry reg;
    SANDAL_ASSERT(reg.current() == nullptr);

    std::string err;
    double swap_ms = -1.0;
    SANDAL_ASSERT(!reg.publish(nullptr, err, &swap_ms));
    SANDAL_ASSERT(!err.empty() && reg.current() == nullptr);

    auto bad_sha = make_test_snapshot(1U, "bad_digest", 100, 2, "BrokenSword");
    err.clear();
    SANDAL_ASSERT(!reg.publish(bad_sha, err, &swap_ms));
    SANDAL_ASSERT(!err.empty() && reg.current() == nullptr);

    auto v2 = make_test_snapshot(2U, kValidDigestV1, 100, 3, "IronBlade_v2");
    err.clear();
    SANDAL_ASSERT(reg.publish(v2, err, &swap_ms));
    SANDAL_ASSERT(err.empty() && swap_ms >= 0.0 && swap_ms < kMaxHotReloadSwapMs);
    SANDAL_ASSERT(reg.current() != nullptr && reg.current()->version == 2U);

    auto stale_v1 = make_test_snapshot(1U, kValidDigestUpper, 100, 4, "StaleBlade_v1");
    auto dup_v2 = make_test_snapshot(2U, kValidDigestUpper, 100, 4, "DupBlade_v2");
    SANDAL_ASSERT(!reg.publish(stale_v1, err) && !err.empty());
    SANDAL_ASSERT(!reg.publish(dup_v2, err) && !err.empty());
    SANDAL_ASSERT(reg.current()->version == 2U);
    return 0;
}

int test_rcu_reader_isolation_and_monotonic_swaps() {
    using namespace aoc::worldd::content;
    ContentSnapshotRegistry reg;
    std::string err;
    double swap_ms = 0.0;

    auto v1 = make_test_snapshot(1U, kValidDigestV1, 5001, 2, "Relic_v1");
    SANDAL_ASSERT(reg.publish(v1, err, &swap_ms) && err.empty());
    SANDAL_ASSERT(swap_ms < kMaxHotReloadSwapMs);

    const std::shared_ptr<const ContentSnapshot> snap_v1 = reg.current();
    SANDAL_ASSERT(snap_v1 != nullptr && snap_v1->version == 1U);
    SANDAL_ASSERT(snap_v1->item_quality(5001) == 2 && snap_v1->item_name(5001) == "Relic_v1");

    for (uint64_t ver = 2U; ver <= 50U; ++ver) {
        const auto digest = (ver % 2U == 0U) ? kValidDigestUpper : kValidDigestV1;
        auto next_snap = make_test_snapshot(
            ver,
            digest,
            5001,
            static_cast<int32_t>(ver),
            "Relic_v" + std::to_string(ver));
        swap_ms = -1.0;
        SANDAL_ASSERT(reg.publish(next_snap, err, &swap_ms) && err.empty());
        SANDAL_ASSERT(swap_ms >= 0.0 && swap_ms < kMaxHotReloadSwapMs);
        SANDAL_ASSERT(snap_v1->version == 1U);
        SANDAL_ASSERT(snap_v1->item_quality(5001) == 2 && snap_v1->item_name(5001) == "Relic_v1");
    }

    const auto snap_v50 = reg.current();
    SANDAL_ASSERT(snap_v50 != nullptr && snap_v50->version == 50U);
    SANDAL_ASSERT(snap_v50->item_quality(5001) == 50 && snap_v50->item_name(5001) == "Relic_v50");
    SANDAL_ASSERT(snap_v50->item_quality(9999) == 1 && snap_v50->item_name(9999).empty());
    SANDAL_ASSERT(snap_v1->version == 1U && snap_v1->item_name(5001) == "Relic_v1");
    return 0;
}

int run_content_snapshot_rcu(const sandal::test::DataFreeConfig&) {
    if (test_sha256_hex_validation() != 0) {
        return 1;
    }
    if (test_publish_rejection_guards() != 0) {
        return 1;
    }
    return test_rcu_reader_isolation_and_monotonic_swaps();
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "content_snapshot_rcu", run_content_snapshot_rcu);
}
