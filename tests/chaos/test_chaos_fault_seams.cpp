#include "../world/test_harness.h"

#include <cstdint>
#include <memory>

namespace {

struct SlidingSeqWindow14 {
    uint16_t max_seq{0};
    uint64_t ack_mask{0};
    bool initialized{false};
};

[[nodiscard]] bool accept_seq14(SlidingSeqWindow14& win, uint16_t seq) noexcept {
    constexpr uint16_t kMod14 = 16384U;
    constexpr uint16_t kHalf14 = 8192U;
    seq &= (kMod14 - 1U);
    if (!win.initialized) {
        win.initialized = true;
        win.max_seq = seq;
        win.ack_mask = 1ULL;
        return true;
    }
    const uint16_t forward_diff = static_cast<uint16_t>((seq - win.max_seq) & (kMod14 - 1U));
    if (forward_diff == 0U) {
        return false;
    }
    if (forward_diff < kHalf14) {
        win.ack_mask = (forward_diff >= 64U) ? 1ULL : ((win.ack_mask << forward_diff) | 1ULL);
        win.max_seq = seq;
        return true;
    }
    const uint16_t back_diff = static_cast<uint16_t>((win.max_seq - seq) & (kMod14 - 1U));
    if (back_diff >= 64U) {
        return false;
    }
    const uint64_t bit = (1ULL << back_diff);
    if ((win.ack_mask & bit) != 0ULL) {
        return false;
    }
    win.ack_mask |= bit;
    return true;
}

struct ContentSnapshotGeneration {
    uint64_t generation{0};
    uint32_t mob_loot_table_crc{0};
};

[[nodiscard]] constexpr uint64_t make_fencing_token(uint32_t lease_epoch, uint32_t save_seq) noexcept {
    return (static_cast<uint64_t>(lease_epoch) << 32U) | static_cast<uint64_t>(save_seq);
}

[[nodiscard]] constexpr bool apply_fenced_save(uint64_t& committed_token, uint64_t candidate_token) noexcept {
    if (candidate_token < committed_token) {
        return false;
    }
    committed_token = candidate_token;
    return true;
}

int test_udp_jitter_loss_reorder_seam() {
    sandal::test::ManualClock clock{0};
    SlidingSeqWindow14 win{};

    SANDAL_ASSERT(accept_seq14(win, 100U));
    SANDAL_ASSERT(!accept_seq14(win, 100U));

    clock.advance_ms(200);
    SANDAL_ASSERT(accept_seq14(win, 103U));
    SANDAL_ASSERT(accept_seq14(win, 101U));
    SANDAL_ASSERT(accept_seq14(win, 102U));
    SANDAL_ASSERT(!accept_seq14(win, 101U));

    SlidingSeqWindow14 wrap_win{};
    SANDAL_ASSERT(accept_seq14(wrap_win, 16382U));
    SANDAL_ASSERT(accept_seq14(wrap_win, 16383U));
    SANDAL_ASSERT(accept_seq14(wrap_win, 0U));
    SANDAL_ASSERT(!accept_seq14(wrap_win, 16383U));
    SANDAL_ASSERT(!accept_seq14(wrap_win, 16300U));
    SANDAL_ASSERT(clock.now_ms() == 200);
    return 0;
}

int test_rcu_snapshot_generation_fence() {
    sandal::test::ManualClock clock{0};
    auto active_snapshot = std::make_shared<const ContentSnapshotGeneration>(
        ContentSnapshotGeneration{1ULL, 0xA1B2C3D4U});

    const auto tick_pinned = active_snapshot;
    active_snapshot = std::make_shared<const ContentSnapshotGeneration>(
        ContentSnapshotGeneration{2ULL, 0x55667788U});

    clock.advance_ticks(1, 30);
    SANDAL_ASSERT(tick_pinned->generation == 1ULL && tick_pinned->mob_loot_table_crc == 0xA1B2C3D4U);
    SANDAL_ASSERT(active_snapshot->generation == 2ULL && active_snapshot->mob_loot_table_crc == 0x55667788U);

    uint64_t committed_fence = make_fencing_token(3U, 10U);
    SANDAL_ASSERT(!apply_fenced_save(committed_fence, make_fencing_token(2U, 99U)));
    SANDAL_ASSERT(!apply_fenced_save(committed_fence, make_fencing_token(3U, 9U)));
    SANDAL_ASSERT(apply_fenced_save(committed_fence, make_fencing_token(3U, 11U)));
    SANDAL_ASSERT(apply_fenced_save(committed_fence, make_fencing_token(4U, 1U)));
    SANDAL_ASSERT(clock.now_ms() == 33);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "udp_jitter_loss_reorder_seam")) {
        if (test_udp_jitter_loss_reorder_seam() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "rcu_snapshot_generation_fence")) {
        if (test_rcu_snapshot_generation_fence() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
