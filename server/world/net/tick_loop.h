// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include <cstdint>

namespace aoc::worldd {

struct TickPhaseBudgets {
    uint64_t drain_inbound = 0;
    uint64_t simulate_world = 0;
    uint64_t replicate_aoi = 0;
    uint64_t flush_outbound = 0;

    [[nodiscard]] constexpr uint64_t total_us() const noexcept {
        return drain_inbound + simulate_world + replicate_aoi + flush_outbound;
    }
};

class TickLoopBudget {
public:
    static constexpr uint32_t kTickRateHz = 30;
    static constexpr uint64_t kTickIntervalUs = 33333;
    static constexpr uint64_t kTickSloP99Us = 15000;
    static constexpr TickPhaseBudgets kDefaultPhaseBudgets{2000, 6000, 5000, 2000};

    constexpr TickLoopBudget() noexcept = default;
    constexpr explicit TickLoopBudget(TickPhaseBudgets budgets) noexcept : phase_budgets_(budgets) {}

    constexpr void record_tick_duration_us(uint64_t duration_us) noexcept {
        ++tick_index_;
        last_duration_us_ = duration_us;
        if (duration_us > max_duration_us_) max_duration_us_ = duration_us;
        if (duration_us > kTickSloP99Us) ++slo_violation_count_;
        if (duration_us > kTickIntervalUs) ++overrun_count_;
    }

    constexpr void record_tick_phases(const TickPhaseBudgets& phases) noexcept {
        last_phases_ = phases;
        record_tick_duration_us(phases.total_us());
    }

    constexpr void record_tick_phases(uint64_t drain_inbound_us, uint64_t simulate_world_us,
                                      uint64_t replicate_aoi_us, uint64_t flush_outbound_us) noexcept {
        record_tick_phases(TickPhaseBudgets{drain_inbound_us, simulate_world_us, replicate_aoi_us, flush_outbound_us});
    }

    [[nodiscard]] constexpr uint64_t tick_index() const noexcept { return tick_index_; }
    [[nodiscard]] constexpr uint64_t overrun_count() const noexcept { return overrun_count_; }
    [[nodiscard]] constexpr uint64_t slo_violation_count() const noexcept { return slo_violation_count_; }
    [[nodiscard]] constexpr uint64_t max_duration_us() const noexcept { return max_duration_us_; }
    [[nodiscard]] constexpr uint64_t last_duration_us() const noexcept { return last_duration_us_; }
    [[nodiscard]] constexpr const TickPhaseBudgets& last_phases() const noexcept { return last_phases_; }
    [[nodiscard]] constexpr const TickPhaseBudgets& phase_budgets() const noexcept { return phase_budgets_; }
    [[nodiscard]] constexpr bool last_exceeded_slo() const noexcept { return last_duration_us_ > kTickSloP99Us; }
    [[nodiscard]] constexpr bool last_overran_tick() const noexcept { return last_duration_us_ > kTickIntervalUs; }
    constexpr void reset() noexcept { *this = TickLoopBudget{phase_budgets_}; }

private:
    TickPhaseBudgets phase_budgets_ = kDefaultPhaseBudgets;
    TickPhaseBudgets last_phases_{};
    uint64_t tick_index_ = 0;
    uint64_t overrun_count_ = 0;
    uint64_t slo_violation_count_ = 0;
    uint64_t max_duration_us_ = 0;
    uint64_t last_duration_us_ = 0;
};

using FixedTickTracker = TickLoopBudget;

}  // namespace aoc::worldd
