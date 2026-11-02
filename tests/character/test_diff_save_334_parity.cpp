#include "../world/test_harness.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <utility>

#ifndef SANDAL_ASSERT_EQ
#define SANDAL_ASSERT_EQ(lhs, rhs) SANDAL_ASSERT((lhs) == (rhs))
#endif

namespace sandal::test {
namespace harness_ext {

class DeterministicPrng {
public:
    explicit constexpr DeterministicPrng(uint64_t seed = 0x9E3779B97F4A7C15ULL) noexcept
        : state_(seed != 0ULL ? seed : 0x9E3779B97F4A7C15ULL) {}

    [[nodiscard]] constexpr uint64_t next_u64() noexcept {
        state_ ^= state_ >> 12U;
        state_ ^= state_ << 25U;
        state_ ^= state_ >> 27U;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    [[nodiscard]] constexpr uint32_t next_u32(uint32_t bound) noexcept {
        return bound == 0U ? 0U : static_cast<uint32_t>(next_u64() % bound);
    }

private:
    uint64_t state_;
};

struct TestContext {
    DataFreeConfig config{};
    ManualClock clock{0};
    DeterministicPrng prng{0x3340C0DE9E3779B9ULL};
};

struct SuiteCase {
    std::string_view name;
    int (*fn)(TestContext&);
};

template <std::size_t N>
[[nodiscard]] int run_suite(int argc, char** argv, const std::array<SuiteCase, N>& cases) {
    TestContext ctx{parse_test_cli(argc, argv), ManualClock{0}, DeterministicPrng{0x3340C0DE9E3779B9ULL}};
    int ran = 0;
    for (const auto& tc : cases) {
        if (should_run_case(ctx.config, tc.name)) {
            ctx.clock = ManualClock{0};
            ctx.prng = DeterministicPrng{0x3340C0DE9E3779B9ULL};
            if (tc.fn(ctx) != 0) {
                return 1;
            }
            ++ran;
        }
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}

}  // namespace harness_ext
using namespace harness_ext;
}  // namespace sandal::test

namespace {

enum DomainIndex : std::size_t {
    kDomainHeader = 0,
    kDomainBags = 1,
    kDomainEquipment = 2,
    kDomainWallet = 3,
    kDomainQuests = 4,
    kDomainCooldowns = 5,
    kDomainTalents = 6,
};

struct CharacterHeader {
    uint32_t level{1};
    uint64_t xp{0};
    int32_t x_cm{0};
    int32_t y_cm{0};
    int32_t z_cm{0};
    int32_t hp{1000};
    int32_t mp{500};
    bool operator==(const CharacterHeader&) const noexcept = default;
};

struct BagSlotRecord {
    uint32_t item_id{0};
    uint32_t stack_count{0};
    uint32_t durability{0};
    bool operator==(const BagSlotRecord&) const noexcept = default;
};

struct EquipSlotRecord {
    uint32_t item_id{0};
    uint32_t enchant_tier{0};
    uint32_t durability{0};
    bool operator==(const EquipSlotRecord&) const noexcept = default;
};

struct WalletState {
    int64_t copper{0};
    std::array<uint32_t, 6> glint_tiers{};
    bool operator==(const WalletState&) const noexcept = default;
};

struct CharacterSnapshot {
    uint64_t fencing_token{0};
    CharacterHeader header{};
    std::array<BagSlotRecord, 32> bags{};
    std::array<EquipSlotRecord, 16> equipment{};
    WalletState wallet{};
    std::array<uint32_t, 16> quest_counters{};
    std::array<int64_t, 16> cooldown_expiry_ms{};
    std::array<uint8_t, 16> talent_ranks{};
    bool operator==(const CharacterSnapshot&) const noexcept = default;
};

struct TrackedCharacterState {
    CharacterSnapshot live{};
    std::bitset<64> domain_dirty{};
    std::bitset<64> bag_upsert_mask{};
    std::bitset<64> bag_tombstone_mask{};
    std::bitset<64> equip_dirty_mask{};
    std::bitset<64> glint_dirty_mask{};
    std::bitset<64> quest_dirty_mask{};
    std::bitset<64> cooldown_dirty_mask{};
    std::bitset<64> talent_dirty_mask{};
    bool wallet_copper_dirty{false};
};

struct DiffFlushResult {
    bool ok{false};
    uint32_t sql_statements{0};
    uint64_t fencing_token{0};
};

[[nodiscard]] constexpr uint64_t make_fencing_token(uint32_t lease_epoch, uint32_t save_seq) noexcept {
    return (static_cast<uint64_t>(lease_epoch) << 32U) | static_cast<uint64_t>(save_seq);
}

void mark_bag_slot_state(TrackedCharacterState& st, std::size_t slot) noexcept {
    st.domain_dirty.set(kDomainBags);
    if (st.live.bags[slot].item_id == 0U || st.live.bags[slot].stack_count == 0U) {
        st.live.bags[slot] = BagSlotRecord{};
        st.bag_tombstone_mask.set(slot);
        st.bag_upsert_mask.reset(slot);
        return;
    }
    st.bag_upsert_mask.set(slot);
    st.bag_tombstone_mask.reset(slot);
}

void clear_dirty_tracking(TrackedCharacterState& st) noexcept {
    st.domain_dirty.reset();
    st.bag_upsert_mask.reset();
    st.bag_tombstone_mask.reset();
    st.equip_dirty_mask.reset();
    st.glint_dirty_mask.reset();
    st.quest_dirty_mask.reset();
    st.cooldown_dirty_mask.reset();
    st.talent_dirty_mask.reset();
    st.wallet_copper_dirty = false;
}

void apply_bag_diff(const TrackedCharacterState& st, CharacterSnapshot& replica) noexcept {
    for (std::size_t i = 0; i < 32U; ++i) {
        if (st.bag_tombstone_mask.test(i)) {
            replica.bags[i] = BagSlotRecord{};
        } else if (st.bag_upsert_mask.test(i)) {
            replica.bags[i] = st.live.bags[i];
        }
    }
}

void apply_secondary_diffs(const TrackedCharacterState& st, CharacterSnapshot& replica) noexcept {
    for (std::size_t i = 0; i < 16U; ++i) {
        if (st.equip_dirty_mask.test(i)) {
            replica.equipment[i] = st.live.equipment[i];
        }
        if (st.quest_dirty_mask.test(i)) {
            replica.quest_counters[i] = st.live.quest_counters[i];
        }
        if (st.cooldown_dirty_mask.test(i)) {
            replica.cooldown_expiry_ms[i] = st.live.cooldown_expiry_ms[i];
        }
        if (st.talent_dirty_mask.test(i)) {
            replica.talent_ranks[i] = st.live.talent_ranks[i];
        }
    }
}

void apply_wallet_diff(const TrackedCharacterState& st, CharacterSnapshot& replica) noexcept {
    if (st.wallet_copper_dirty) {
        replica.wallet.copper = st.live.wallet.copper;
    }
    for (std::size_t i = 0; i < 6U; ++i) {
        if (st.glint_dirty_mask.test(i)) {
            replica.wallet.glint_tiers[i] = st.live.wallet.glint_tiers[i];
        }
    }
}

[[nodiscard]] DiffFlushResult flush_incremental_diff(
    TrackedCharacterState& st,
    CharacterSnapshot& replica,
    uint32_t lease_epoch,
    uint32_t save_seq,
    bool simulate_failure) noexcept {
    const uint64_t token = make_fencing_token(lease_epoch, save_seq);
    if (token <= replica.fencing_token || simulate_failure) {
        return {false, 0U, replica.fencing_token};
    }
    const uint32_t statements = static_cast<uint32_t>(st.domain_dirty.count());
    if (st.domain_dirty.test(kDomainHeader)) {
        replica.header = st.live.header;
    }
    if (st.domain_dirty.test(kDomainBags)) {
        apply_bag_diff(st, replica);
    }
    if (st.domain_dirty.test(kDomainWallet)) {
        apply_wallet_diff(st, replica);
    }
    apply_secondary_diffs(st, replica);
    replica.fencing_token = token;
    st.live.fencing_token = token;
    clear_dirty_tracking(st);
    return {true, statements, token};
}

[[nodiscard]] CharacterSnapshot serialize_full_snapshot(
    const TrackedCharacterState& st,
    uint32_t lease_epoch,
    uint32_t save_seq) noexcept {
    CharacterSnapshot snap = st.live;
    snap.fencing_token = make_fencing_token(lease_epoch, save_seq);
    return snap;
}

void mutate_bags_step(TrackedCharacterState& st, sandal::test::DeterministicPrng& prng, uint32_t op) noexcept {
    const std::size_t slot_a = prng.next_u32(32U);
    if (op == 0U) {
        st.live.bags[slot_a] = BagSlotRecord{2001U + prng.next_u32(60U), 1U + prng.next_u32(20U), 50U + prng.next_u32(51U)};
        mark_bag_slot_state(st, slot_a);
    } else if (op == 1U) {
        st.live.bags[slot_a] = BagSlotRecord{};
        mark_bag_slot_state(st, slot_a);
    } else {
        const std::size_t slot_b = prng.next_u32(32U);
        std::swap(st.live.bags[slot_a], st.live.bags[slot_b]);
        mark_bag_slot_state(st, slot_a);
        mark_bag_slot_state(st, slot_b);
    }
}

void mutate_wallet_and_gear_step(TrackedCharacterState& st, sandal::test::DeterministicPrng& prng) noexcept {
    const uint32_t glint_item_id = 1003001U + prng.next_u32(6U);
    const std::size_t tier_idx = static_cast<std::size_t>(glint_item_id - 1003001U);
    const uint32_t delta = 1U + prng.next_u32(15U);
    if ((prng.next_u32(2U) == 0U) || st.live.wallet.glint_tiers[tier_idx] < delta) {
        st.live.wallet.glint_tiers[tier_idx] += delta;
    } else {
        st.live.wallet.glint_tiers[tier_idx] -= delta;
    }
    st.live.wallet.copper += static_cast<int64_t>(prng.next_u32(500U)) - 150;
    st.glint_dirty_mask.set(tier_idx);
    st.wallet_copper_dirty = true;
    st.domain_dirty.set(kDomainWallet);

    const std::size_t eq_slot = prng.next_u32(16U);
    st.live.equipment[eq_slot] = EquipSlotRecord{5000U + prng.next_u32(40U), prng.next_u32(6U), 20U + prng.next_u32(81U)};
    st.equip_dirty_mask.set(eq_slot);
    st.domain_dirty.set(kDomainEquipment);
}

void mutate_progression_step(TrackedCharacterState& st, sandal::test::DeterministicPrng& prng, int64_t now_ms) noexcept {
    st.live.header.level = 1U + prng.next_u32(25U);
    st.live.header.xp += 25ULL + prng.next_u32(250U);
    st.live.header.x_cm = static_cast<int32_t>(prng.next_u32(100000U)) - 50000;
    st.live.header.y_cm = static_cast<int32_t>(prng.next_u32(100000U)) - 50000;
    st.live.header.z_cm = static_cast<int32_t>(prng.next_u32(4000U));
    st.live.header.hp = 100 + static_cast<int32_t>(prng.next_u32(4900U));
    st.live.header.mp = 50 + static_cast<int32_t>(prng.next_u32(2450U));
    st.domain_dirty.set(kDomainHeader);

    const std::size_t idx = prng.next_u32(16U);
    st.live.quest_counters[idx] = prng.next_u32(50U);
    st.quest_dirty_mask.set(idx);
    st.domain_dirty.set(kDomainQuests);

    st.live.cooldown_expiry_ms[idx] = now_ms + static_cast<int64_t>(1000U + prng.next_u32(30000U));
    st.cooldown_dirty_mask.set(idx);
    st.domain_dirty.set(kDomainCooldowns);

    st.live.talent_ranks[idx] = static_cast<uint8_t>(prng.next_u32(6U));
    st.talent_dirty_mask.set(idx);
    st.domain_dirty.set(kDomainTalents);
}

void apply_random_mutation(TrackedCharacterState& st, sandal::test::TestContext& ctx) noexcept {
    ctx.clock.advance_ms(10);
    const uint32_t pick = ctx.prng.next_u32(5U);
    if (pick < 3U) {
        mutate_bags_step(st, ctx.prng, pick);
    } else if (pick == 3U) {
        mutate_wallet_and_gear_step(st, ctx.prng);
    } else {
        mutate_progression_step(st, ctx.prng, ctx.clock.now_ms());
    }
}

int test_dirty_slot_diff_save_10k_parity(sandal::test::TestContext& ctx) {
    TrackedCharacterState state{};
    CharacterSnapshot incremental_replica{};
    uint32_t lease_epoch = 1U;
    uint32_t save_seq = 0U;
    uint32_t until_flush = 1U + ctx.prng.next_u32(25U);

    for (uint32_t step = 1U; step <= 10000U; ++step) {
        apply_random_mutation(state, ctx);
        --until_flush;
        if (until_flush == 0U || step == 10000U) {
            ++save_seq;
            if ((save_seq % 64U) == 0U) {
                ++lease_epoch;
                save_seq = 1U;
            }
            const auto res = flush_incremental_diff(state, incremental_replica, lease_epoch, save_seq, false);
            SANDAL_ASSERT(res.ok);
            SANDAL_ASSERT(res.sql_statements >= 1U && res.sql_statements <= 7U);
            SANDAL_ASSERT(state.domain_dirty.none());

            const auto full_replica = serialize_full_snapshot(state, lease_epoch, save_seq);
            SANDAL_ASSERT_EQ(incremental_replica, full_replica);
            until_flush = 1U + ctx.prng.next_u32(25U);
        }
    }
    return 0;
}

int test_diff_save_fencing_epoch_and_rollback(sandal::test::TestContext& ctx) {
    TrackedCharacterState state{};
    CharacterSnapshot committed_replica{};

    mutate_bags_step(state, ctx.prng, 0U);
    mutate_wallet_and_gear_step(state, ctx.prng);
    mutate_progression_step(state, ctx.prng, 500);

    const auto first = flush_incremental_diff(state, committed_replica, 3U, 10U, false);
    SANDAL_ASSERT(first.ok && first.sql_statements == 7U);
    const CharacterSnapshot baseline_committed = committed_replica;

    mutate_bags_step(state, ctx.prng, 0U);
    const auto stale_epoch = flush_incremental_diff(state, committed_replica, 2U, 99U, false);
    SANDAL_ASSERT(!stale_epoch.ok);
    SANDAL_ASSERT_EQ(committed_replica, baseline_committed);

    const auto stale_seq = flush_incremental_diff(state, committed_replica, 3U, 10U, false);
    SANDAL_ASSERT(!stale_seq.ok);
    SANDAL_ASSERT_EQ(committed_replica, baseline_committed);

    mutate_wallet_and_gear_step(state, ctx.prng);
    const std::bitset<64> dirty_before_fail = state.domain_dirty;
    const auto transient_fail = flush_incremental_diff(state, committed_replica, 3U, 11U, true);
    SANDAL_ASSERT(!transient_fail.ok);
    SANDAL_ASSERT_EQ(committed_replica, baseline_committed);
    SANDAL_ASSERT_EQ(state.domain_dirty, dirty_before_fail);

    const auto retry_ok = flush_incremental_diff(state, committed_replica, 3U, 11U, false);
    SANDAL_ASSERT(retry_ok.ok && retry_ok.sql_statements <= 7U);
    SANDAL_ASSERT_EQ(committed_replica, serialize_full_snapshot(state, 3U, 11U));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    constexpr std::array<sandal::test::SuiteCase, 2> kCases{{
        {"dirty_slot_diff_save_10k_parity", &test_dirty_slot_diff_save_10k_parity},
        {"diff_save_fencing_epoch_and_rollback", &test_diff_save_fencing_epoch_and_rollback},
    }};
    return sandal::test::run_suite(argc, argv, kCases);
}
