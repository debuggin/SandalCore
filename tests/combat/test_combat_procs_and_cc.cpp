#include "../world/test_harness.h"

#include <cstdint>

namespace {

constexpr uint8_t kMaxProcDepth = 3U;

struct ProcGuardState {
    uint8_t active_depth{0};
    int64_t last_proc_ms{-10'000};
    int64_t icd_ms{500};
    uint32_t fired_count{0};
};

[[nodiscard]] bool try_fire_proc(
    ProcGuardState& state,
    bool is_reaction_source,
    int64_t now_ms) noexcept {
    if (is_reaction_source || state.active_depth >= kMaxProcDepth) {
        return false;
    }
    if ((now_ms - state.last_proc_ms) < state.icd_ms) {
        return false;
    }
    ++state.active_depth;
    state.last_proc_ms = now_ms;
    ++state.fired_count;
    return true;
}

struct CcTracker {
    uint8_t dr_stacks{0};
    int64_t lockout_until_ms{0};
    int64_t window_reset_ms{0};
    int32_t active_debuff_stacks{0};
};

[[nodiscard]] int64_t apply_cc_or_debuff(
    CcTracker& tracker,
    bool is_hard_cc,
    int64_t base_duration_ms,
    int64_t now_ms) noexcept {
    if (base_duration_ms <= 0) {
        return 0;
    }
    if (!is_hard_cc) {
        ++tracker.active_debuff_stacks;
        return base_duration_ms;
    }
    if (now_ms < tracker.lockout_until_ms) {
        return 0;
    }
    if (now_ms >= tracker.window_reset_ms) {
        tracker.dr_stacks = 0U;
    }
    const int64_t scaled = base_duration_ms >> tracker.dr_stacks;
    ++tracker.dr_stacks;
    tracker.window_reset_ms = now_ms + 15'000;
    if (tracker.dr_stacks >= 3U) {
        tracker.lockout_until_ms = now_ms + 10'000;
    }
    return scaled;
}

int test_proc_chain_depth_guard() {
    sandal::test::ManualClock clock{0};
    ProcGuardState state{};

    SANDAL_ASSERT(!try_fire_proc(state, true, clock.now_ms()));
    SANDAL_ASSERT(state.fired_count == 0U);

    SANDAL_ASSERT(try_fire_proc(state, false, clock.now_ms()));
    SANDAL_ASSERT(state.active_depth == 1U && state.fired_count == 1U);
    SANDAL_ASSERT(!try_fire_proc(state, false, clock.now_ms()));

    clock.advance_ms(499);
    SANDAL_ASSERT(!try_fire_proc(state, false, clock.now_ms()));
    clock.advance_ms(1);
    SANDAL_ASSERT(try_fire_proc(state, false, clock.now_ms()));

    clock.advance_ms(500);
    SANDAL_ASSERT(try_fire_proc(state, false, clock.now_ms()));
    SANDAL_ASSERT(state.active_depth == kMaxProcDepth);

    clock.advance_ms(500);
    SANDAL_ASSERT(!try_fire_proc(state, false, clock.now_ms()));
    SANDAL_ASSERT(state.fired_count == 3U);
    return 0;
}

int test_cc_diminishing_returns_clock() {
    sandal::test::ManualClock clock{1'000};
    CcTracker tracker{};

    SANDAL_ASSERT(apply_cc_or_debuff(tracker, false, 4'000, clock.now_ms()) == 4'000);
    SANDAL_ASSERT(tracker.active_debuff_stacks == 1 && tracker.dr_stacks == 0U);

    SANDAL_ASSERT(apply_cc_or_debuff(tracker, true, 4'000, clock.now_ms()) == 4'000);
    clock.advance_ms(1'000);
    SANDAL_ASSERT(apply_cc_or_debuff(tracker, true, 4'000, clock.now_ms()) == 2'000);
    clock.advance_ms(1'000);
    SANDAL_ASSERT(apply_cc_or_debuff(tracker, true, 4'000, clock.now_ms()) == 1'000);

    clock.advance_ms(9'999);
    SANDAL_ASSERT(apply_cc_or_debuff(tracker, true, 4'000, clock.now_ms()) == 0);

    clock.advance_ms(1);
    SANDAL_ASSERT(apply_cc_or_debuff(tracker, true, 0, clock.now_ms()) == 0);
    clock.advance_ms(5'000);
    SANDAL_ASSERT(apply_cc_or_debuff(tracker, true, 4'000, clock.now_ms()) == 4'000);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto cfg = sandal::test::parse_test_cli(argc, argv);
    int ran = 0;
    if (sandal::test::should_run_case(cfg, "proc_chain_depth_guard")) {
        if (test_proc_chain_depth_guard() != 0) {
            return 1;
        }
        ++ran;
    }
    if (sandal::test::should_run_case(cfg, "cc_diminishing_returns_clock")) {
        if (test_cc_diminishing_returns_clock() != 0) {
            return 1;
        }
        ++ran;
    }
    SANDAL_ASSERT(ran > 0);
    return 0;
}
