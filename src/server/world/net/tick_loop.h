// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "net/connection.h"
#include "world/relevancy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>

namespace aoc { namespace worldd {

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

class TickSchedule {
public:
    static constexpr int kMinHz = 10;
    static constexpr int kMaxHz = 120;

    static Clock::duration period_of(int hz) {
        // +1 ns ensures timers of n/hz seconds set at a tick fall due at the n-th tick without rounding one tick late.
        return std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(1000000000LL / std::max(hz, 1) + 1));
    }

    TickSchedule(int hz, Clock::time_point start, int max_catch_up = 4)
        : hz_(std::max(hz, 1)), period_(period_of(hz_)), max_catch_up_(std::max(max_catch_up, 0)), origin_(start), next_(start + period_) {}

    int hz() const noexcept { return hz_; }
    void set_hz(int hz, Clock::time_point now) noexcept {
        hz_ = std::clamp(hz, 1, kMaxHz);
        period_ = period_of(hz_);
        origin_ = now;
        k_ = 1;
        next_ = now + period_;
    }

    Clock::duration period() const { return period_; }
    Clock::time_point next() const { return next_; }
    bool due(Clock::time_point now) const { return now >= next_; }

    Clock::time_point take(Clock::time_point now) {
        Clock::time_point t = next_;
        if (now > t) {
            late_max_ = std::max(late_max_, now - t);
            const int64_t behind = static_cast<int64_t>((now - t) / period_);
            if (behind > max_catch_up_) {
                skipped_ += static_cast<uint64_t>(behind);
                ++restarts_;
                origin_ = now;
                k_ = 0;
                t = now;
            } else if (behind >= 1) {
                ++caught_up_;
            }
        }
        ++k_;
        next_ = origin_ + period_ * k_;
        ++ticks_;
        return t;
    }

    void restart(Clock::time_point now) {
        if (now >= next_) {
            skipped_ += static_cast<uint64_t>((now - next_) / period_) + 1;
            late_max_ = std::max(late_max_, now - next_);
        }
        ++restarts_;
        origin_ = now;
        k_ = 1;
        next_ = now + period_;
    }

    uint64_t ticks() const { return ticks_; }
    uint64_t caught_up() const { return caught_up_; }
    uint64_t skipped() const { return skipped_; }
    uint64_t restarts() const { return restarts_; }
    Clock::duration late_max() const { return late_max_; }
    void reset_late_max() { late_max_ = Clock::duration::zero(); }

private:
    int hz_ = 30;
    Clock::duration period_;
    int64_t max_catch_up_;
    Clock::time_point origin_;
    int64_t k_ = 1;
    Clock::time_point next_;
    uint64_t ticks_ = 0, caught_up_ = 0, skipped_ = 0, restarts_ = 0;
    Clock::duration late_max_{};
};

inline int64_t tick_view_timing(world::RelevancyConfig& r, Clock::duration period) {
    using std::chrono::duration;
    using std::chrono::milliseconds;
    const double ratio = duration<double>(r.tick).count() / duration<double>(period).count();
    const int64_t n = std::max<int64_t>(1, static_cast<int64_t>(std::llround(ratio)));
    // Set gates half a period short of grid ticks so ticks on the grid always meet them.
    r.tick = std::chrono::duration_cast<milliseconds>(period * n - period / 2);
    const Clock::duration close = std::chrono::duration_cast<Clock::duration>(r.dormancy_delay) - period / 2;
    r.dormancy_delay = std::chrono::duration_cast<milliseconds>(std::max(close, Clock::duration::zero()));
    return n;
}

class TickLoop {
public:
    struct Config {
        int hz = 30;
        int max_catch_up = 4;
        size_t max_batch = 256;
    };

    struct Ports {
        std::function<Clock::time_point()> now;
        std::function<std::optional<size_t>(std::chrono::microseconds timeout)> receive;
        std::function<void(size_t size, Clock::time_point now)> datagram;
        std::function<void(Clock::time_point t)> tick;
        std::function<void()> after_tick;
    };

    struct Stats {
        uint64_t passes = 0;
        uint64_t ticks = 0;
        uint64_t datagrams = 0;
        size_t max_batch = 0;
        size_t max_datagrams_per_tick = 0;
        uint64_t caught_up = 0;
        uint64_t skipped = 0;
        uint64_t restarts = 0;
    };

    TickLoop(Config config, Clock::time_point start) : config_(config), schedule_(config.hz, start, config.max_catch_up) {}

    static std::atomic<int>& runtime_tick_hz_slot() noexcept {
        static std::atomic<int> slot{0};
        return slot;
    }

    static void set_global_runtime_tick_hz(int hz) noexcept {
        runtime_tick_hz_slot().store(std::clamp(hz, 1, TickSchedule::kMaxHz), std::memory_order_relaxed);
    }

    static int effective_tick_hz(int configured_hz) noexcept {
        const int rt = runtime_tick_hz_slot().load(std::memory_order_relaxed);
        return rt > 0 ? std::clamp(rt, 1, TickSchedule::kMaxHz) : std::clamp(configured_hz, 1, TickSchedule::kMaxHz);
    }

    int hz() const noexcept { return schedule_.hz(); }
    void set_hz(int hz, Clock::time_point now) noexcept {
        config_.hz = std::clamp(hz, 1, TickSchedule::kMaxHz);
        schedule_.set_hz(config_.hz, now);
    }

    bool run_once(Ports& p) {
        ++stats_.passes;
        Clock::time_point now = p.now();
        run_due(p, now);
        const Clock::duration left = schedule_.next() - now;
        const auto wait = left > Clock::duration::zero() ? std::chrono::ceil<std::chrono::microseconds>(left) : std::chrono::microseconds(0);
        std::optional<size_t> n = p.receive(wait);
        size_t batch = 0;
        while (n && *n > 0) {
            now = p.now();
            run_due(p, now);
            p.datagram(*n, now);
            ++stats_.datagrams;
            ++since_tick_;
            if (++batch >= config_.max_batch) break;
            n = p.receive(std::chrono::microseconds(0));
        }
        stats_.max_batch = std::max(stats_.max_batch, batch);
        return n.has_value();
    }

    const TickSchedule& schedule() const { return schedule_; }
    TickSchedule& schedule() { return schedule_; }
    Stats take_stats() {
        Stats s = stats_;
        s.caught_up = schedule_.caught_up() - taken_caught_up_;
        s.skipped = schedule_.skipped() - taken_skipped_;
        s.restarts = schedule_.restarts() - taken_restarts_;
        taken_caught_up_ = schedule_.caught_up();
        taken_skipped_ = schedule_.skipped();
        taken_restarts_ = schedule_.restarts();
        stats_ = Stats{};
        return s;
    }

private:
    void run_due(Ports& p, Clock::time_point& now) {
        int in_a_row = 0;
        while (schedule_.due(now)) {
            if (in_a_row > config_.max_catch_up) {
                schedule_.restart(now);
                break;
            }
            ++in_a_row;
            const Clock::time_point t = schedule_.take(now);
            stats_.max_datagrams_per_tick = std::max(stats_.max_datagrams_per_tick, since_tick_);
            since_tick_ = 0;
            ++stats_.ticks;
            p.tick(t);
            if (p.after_tick) p.after_tick();
            now = p.now();
        }
    }

    Config config_;
    TickSchedule schedule_;
    Stats stats_;
    size_t since_tick_ = 0;
    uint64_t taken_caught_up_ = 0, taken_skipped_ = 0, taken_restarts_ = 0;
};

}}  // namespace aoc::worldd
