#include "../world/test_harness.h"

#include <algorithm>
#include <cstdint>

namespace {

struct QuestProgress {
    uint32_t current_kills{0};
    uint32_t required_kills{5};
    uint32_t delivered_items{0};
    uint32_t required_items{3};
    int64_t completed_at_ms{0};
    bool ready_to_turn_in{false};
};

[[nodiscard]] bool record_quest_objective_step(
    QuestProgress& q,
    uint32_t add_kills,
    uint32_t add_items,
    int64_t now_ms) noexcept {
    if (q.ready_to_turn_in || (add_kills == 0U && add_items == 0U)) {
        return false;
    }
    q.current_kills = std::min(q.required_kills, q.current_kills + add_kills);
    q.delivered_items = std::min(q.required_items, q.delivered_items + add_items);
    if (q.current_kills == q.required_kills && q.delivered_items == q.required_items) {
        q.ready_to_turn_in = true;
        q.completed_at_ms = now_ms;
    }
    return true;
}

struct CommissionPayout {
    uint32_t dim_glint{0};
    uint32_t dull_glint{0};
    uint32_t granted_xp{0};
    bool claimed{false};
};

[[nodiscard]] bool claim_commission_reward(
    CommissionPayout& out,
    bool objective_complete,
    uint32_t total_t1_glint,
    uint32_t base_xp) noexcept {
    if (!objective_complete || out.claimed || total_t1_glint == 0U) {
        return false;
    }
    out.dim_glint = total_t1_glint / 10U;
    out.dull_glint = total_t1_glint % 10U;
    out.granted_xp = base_xp;
    out.claimed = true;
    return true;
}

int test_quest_objective_counter_step() {
    sandal::test::ManualClock clock{1'000'000'000LL};
    QuestProgress q{};

    SANDAL_ASSERT(!record_quest_objective_step(q, 0U, 0U, clock.now_ms()));
    SANDAL_ASSERT(record_quest_objective_step(q, 3U, 2U, clock.now_ms()));
    SANDAL_ASSERT(q.current_kills == 3U && q.delivered_items == 2U && !q.ready_to_turn_in);

    clock.advance_ms(2'500);
    SANDAL_ASSERT(record_quest_objective_step(q, 5U, 4U, clock.now_ms()));
    SANDAL_ASSERT(q.current_kills == 5U && q.delivered_items == 3U);
    SANDAL_ASSERT(q.ready_to_turn_in && q.completed_at_ms == 3'500);

    clock.advance_ms(500);
    SANDAL_ASSERT(!record_quest_objective_step(q, 1U, 1U, clock.now_ms()));
    SANDAL_ASSERT(q.completed_at_ms == 3'500);
    return 0;
}

int test_commission_board_reward_payout(const sandal::test::DataFreeConfig& cfg) {
    SANDAL_ASSERT(cfg.data_free);
    sandal::test::ManualClock clock{0};
    CommissionPayout payout{};

    SANDAL_ASSERT(!claim_commission_reward(payout, false, 27U, 500U));
    SANDAL_ASSERT(!claim_commission_reward(payout, true, 0U, 500U));
    SANDAL_ASSERT(!payout.claimed && payout.granted_xp == 0U);

    clock.advance_ticks(30, 30);
    SANDAL_ASSERT(claim_commission_reward(payout, true, 27U, 500U));
    SANDAL_ASSERT(payout.claimed);
    SANDAL_ASSERT(payout.dim_glint == 2U && payout.dull_glint == 7U);
    SANDAL_ASSERT(payout.granted_xp == 500U);

    SANDAL_ASSERT(!claim_commission_reward(payout, true, 27U, 500U));
    SANDAL_ASSERT(payout.dim_glint == 2U && payout.dull_glint == 7U);
    SANDAL_ASSERT(clock.now_ms() == 1000);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "quest_objective_counter_step")) {
        if (test_quest_objective_counter_step() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "commission_board_reward_payout")) {
        if (test_commission_board_reward_payout(cfg) != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
