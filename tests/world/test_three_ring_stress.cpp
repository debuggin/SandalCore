// SPDX-License-Identifier: AGPL-3.0-only

#include "test_harness.h"
#include "net/spsc_ring.h"
#include "net/tick_loop.h"

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

int drain_spsc_batch(
    aoc::worldd::InboundNetRing& in_ring,
    aoc::worldd::OutboundNetRing& out_ring,
    aoc::worldd::AsyncTaskCompletionRing& task_ring,
    uint32_t& popped_in,
    uint32_t& popped_out) {
    using namespace aoc::worldd;
    InboundNetFrame in_frame{};
    while (in_ring.try_pop(in_frame)) {
        const uint32_t seq = static_cast<uint32_t>(in_frame.payload[0]) |
                             (static_cast<uint32_t>(in_frame.payload[1]) << 8U);
        SANDAL_ASSERT(seq == popped_in && in_frame.len == 4U && in_frame.payload[2] == 0xA5U);
        ++popped_in;
    }
    OutboundNetFrame out_frame{};
    while (out_ring.try_pop(out_frame)) {
        const uint32_t seq = static_cast<uint32_t>(out_frame.payload[0]) |
                             (static_cast<uint32_t>(out_frame.payload[1]) << 8U);
        SANDAL_ASSERT(seq == popped_out && out_frame.len == 4U && out_frame.payload[3] == 0x5AU);
        ++popped_out;
    }
    AsyncTaskCompletion task_item{};
    while (task_ring.try_pop(task_item)) {
        SANDAL_ASSERT(task_item.session_id > 0U && task_item.result.accepted);
    }
    return 0;
}

int test_spsc_high_volume_fifo() {
    using namespace aoc::worldd;
    InboundNetRing in_ring;
    OutboundNetRing out_ring;
    AsyncTaskCompletionRing task_ring;

    constexpr uint32_t kTotalFrames = 10000U;
    uint32_t popped_in = 0U;
    uint32_t popped_out = 0U;
    uint32_t pushed_task = 0U;

    for (uint32_t i = 0U; i < kTotalFrames; ++i) {
        const std::array<uint8_t, 4> payload{{
            static_cast<uint8_t>(i & 0xFFU),
            static_cast<uint8_t>((i >> 8U) & 0xFFU),
            0xA5U,
            0x5AU,
        }};
        const Address peer{0x7F000001U, static_cast<uint16_t>(9000U + (i % 500U))};
        SANDAL_ASSERT(in_ring.try_push(InboundNetFrame::make(peer, payload.data(), payload.size())));
        SANDAL_ASSERT(out_ring.try_push(OutboundNetFrame::make(peer, payload.data(), payload.size())));
        SANDAL_ASSERT(task_ring.try_push(AsyncTaskCompletion{
            static_cast<uint64_t>(i + 1U),
            LoginResult{true, "", {}},
        }));
        ++pushed_task;
        if (((i + 1U) % 32U) == 0U || (i + 1U) == kTotalFrames) {
            SANDAL_ASSERT(drain_spsc_batch(in_ring, out_ring, task_ring, popped_in, popped_out) == 0);
        }
    }

    SANDAL_ASSERT(popped_in == kTotalFrames && in_ring.dropped_count() == 0U);
    SANDAL_ASSERT(popped_out == kTotalFrames && out_ring.dropped_count() == 0U);
    SANDAL_ASSERT(pushed_task == kTotalFrames && task_ring.empty() && task_ring.dropped_count() == 0U);
    return 0;
}

int test_spsc_saturation_and_overflow() {
    using namespace aoc::worldd;
    InboundNetRing in_ring;
    OutboundNetRing out_ring;
    AsyncTaskCompletionRing task_ring;
    const Address peer{0x0A000001U, 7777U};
    const std::array<uint8_t, 2> bytes{{0x01U, 0x02U}};

    for (std::size_t i = 0U; i < in_ring.capacity(); ++i) {
        SANDAL_ASSERT(in_ring.try_push(InboundNetFrame::make(peer, bytes.data(), bytes.size())));
        SANDAL_ASSERT(out_ring.try_push(OutboundNetFrame::make(peer, bytes.data(), bytes.size())));
    }
    for (std::size_t i = 0U; i < task_ring.capacity(); ++i) {
        SANDAL_ASSERT(task_ring.try_push(AsyncTaskCompletion{i + 1U, LoginResult{true, "", {}}}));
    }

    for (std::size_t over = 0U; over < 16U; ++over) {
        SANDAL_ASSERT(!in_ring.try_push(InboundNetFrame::make(peer, bytes.data(), bytes.size())));
        SANDAL_ASSERT(!out_ring.try_push(OutboundNetFrame::make(peer, bytes.data(), bytes.size())));
        SANDAL_ASSERT(!task_ring.try_push(AsyncTaskCompletion{999U, LoginResult{false, "full", {}}}));
    }
    SANDAL_ASSERT(in_ring.dropped_count() == 16U && out_ring.dropped_count() == 16U);
    SANDAL_ASSERT(task_ring.dropped_count() == 16U);

    in_ring.clear();
    out_ring.clear();
    task_ring.clear();
    SANDAL_ASSERT(in_ring.empty() && in_ring.dropped_count() == 0U && !in_ring.try_pop().has_value());
    SANDAL_ASSERT(out_ring.empty() && out_ring.dropped_count() == 0U && !out_ring.try_pop().has_value());
    SANDAL_ASSERT(task_ring.empty() && task_ring.dropped_count() == 0U && !task_ring.try_pop().has_value());
    return 0;
}

int test_token_snapshot_and_tick_budget() {
    using namespace aoc::worldd;
    SANDAL_ASSERT(TokenSnapshot::constant_time_eq("tok_alpha_123", "tok_alpha_123"));
    SANDAL_ASSERT(!TokenSnapshot::constant_time_eq("tok_alpha_123", "tok_alpha_124"));
    SANDAL_ASSERT(!TokenSnapshot::constant_time_eq("tok_alpha_123", "tok_alpha"));

    TokenSnapshotStore store;
    SANDAL_ASSERT(store.load() != nullptr && store.load()->version == 0U);
    SANDAL_ASSERT(!store.load()->matches(""));

    store.publish(TokenSnapshot{1U, "primary_key_v1", "secondary_key_v0", {}});
    const auto snap = store.load();
    SANDAL_ASSERT(snap != nullptr && snap->version == 1U);
    SANDAL_ASSERT(snap->matches("primary_key_v1") && snap->matches("secondary_key_v0"));
    SANDAL_ASSERT(!snap->matches("forged_key_v1") && !snap->matches(""));

    TickLoopBudget budget;
    budget.record_tick_phases(TickPhaseBudgets{1200U, 4500U, 3100U, 800U});
    SANDAL_ASSERT(!budget.last_exceeded_slo() && !budget.last_overran_tick());
    SANDAL_ASSERT(budget.slo_violation_count() == 0U && budget.overrun_count() == 0U);

    budget.record_tick_phases(TickPhaseBudgets{3000U, 8000U, 4000U, 1000U});
    SANDAL_ASSERT(budget.last_exceeded_slo() && !budget.last_overran_tick());
    SANDAL_ASSERT(budget.slo_violation_count() == 1U && budget.overrun_count() == 0U);

    budget.record_tick_phases(TickPhaseBudgets{5000U, 18000U, 9000U, 3000U});
    SANDAL_ASSERT(budget.last_exceeded_slo() && budget.last_overran_tick());
    SANDAL_ASSERT(budget.slo_violation_count() == 2U && budget.overrun_count() == 1U);
    SANDAL_ASSERT(budget.max_duration_us() == 35000U && budget.tick_index() == 3U);

    budget.reset();
    SANDAL_ASSERT(budget.tick_index() == 0U && budget.overrun_count() == 0U);
    return 0;
}

int run_three_ring_stress(const sandal::test::DataFreeConfig&) {
    if (test_spsc_high_volume_fifo() != 0) {
        return 1;
    }
    if (test_spsc_saturation_and_overflow() != 0) {
        return 1;
    }
    return test_token_snapshot_and_tick_budget();
}

}  // namespace

int main(int argc, char** argv) {
    return sandal::test::run_case_dispatch(argc, argv, "three_ring_stress", run_three_ring_stress);
}
