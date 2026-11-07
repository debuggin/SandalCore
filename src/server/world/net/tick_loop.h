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
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>

namespace aoc { namespace worldd {

enum class TickMode : uint8_t {
    Dynamic = 0,
    Manual = 1,
};

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

struct TickLoopConfig {
    int hz = 60;
    int max_catch_up = 4;
    size_t max_batch = 256;
};

class TickLoop {
public:
    using TickMode = ::aoc::worldd::TickMode;
    using Config = TickLoopConfig;

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

    static constexpr int snap_manual_tick_hz(int hz) noexcept {
        constexpr int kAllowed[8] = {10, 15, 20, 25, 30, 40, 50, 60};
        int best = kAllowed[0];
        int best_diff = hz > best ? (hz - best) : (best - hz);
        for (int candidate : kAllowed) {
            const int d = hz > candidate ? (hz - candidate) : (candidate - hz);
            if (d < best_diff) { best = candidate; best_diff = d; }
        }
        return best;
    }

    static TickMode default_env_tick_mode() noexcept {
        for (const char* k : {"AOC_WORLD_TICK_MODE", "WORLD_TICK_MODE"}) {
            if (const char* v = std::getenv(k); v && *v) {
                if (v[0] == 'm' || v[0] == 'M') return TickMode::Manual;
                if (v[0] == 'd' || v[0] == 'D') return TickMode::Dynamic;
            }
        }
        return TickMode::Dynamic;
    }

    TickLoop(Config config = {}, Clock::time_point start = Clock::now())
        : config_(config),
          schedule_(config.hz > 0 ? config.hz : 60, start, config.max_catch_up),
          tick_mode_(default_env_tick_mode()),
          effective_hz_(tick_mode_ == TickMode::Dynamic ? 60 : snap_manual_tick_hz(config.hz > 0 ? config.hz : 60)),
          budget_(std::chrono::microseconds(1000000 / std::max(1, effective_hz_))) {}

    static std::atomic<int>& runtime_tick_hz_slot() noexcept {
        static std::atomic<int> slot{0};
        return slot;
    }
    static std::atomic<uint8_t>& runtime_tick_mode_slot() noexcept {
        static std::atomic<uint8_t> slot{static_cast<uint8_t>(default_env_tick_mode())};
        return slot;
    }
    static std::atomic<int>& runtime_dynamic_hz_slot() noexcept {
        static std::atomic<int> slot{60};
        return slot;
    }
    static std::atomic<bool>& runtime_dynamic_active_slot() noexcept {
        static std::atomic<bool> slot{false};
        return slot;
    }
    static std::atomic<double>& runtime_ewma_work_ms_slot() noexcept {
        static std::atomic<double> slot{0.0};
        return slot;
    }
    static std::atomic<int>& runtime_dwell_over_slot() noexcept {
        static std::atomic<int> slot{0};
        return slot;
    }
    static std::atomic<int>& runtime_dwell_under_slot() noexcept {
        static std::atomic<int> slot{0};
        return slot;
    }

    static TickMode runtime_tick_mode() noexcept {
        return static_cast<TickMode>(runtime_tick_mode_slot().load(std::memory_order_relaxed));
    }

    static void set_global_runtime_tick_mode(TickMode mode) noexcept {
        runtime_tick_mode_slot().store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
        runtime_dwell_over_slot().store(0, std::memory_order_relaxed);
        runtime_dwell_under_slot().store(0, std::memory_order_relaxed);
        if (mode == TickMode::Dynamic) {
            runtime_tick_hz_slot().store(0, std::memory_order_relaxed);
            runtime_dynamic_hz_slot().store(60, std::memory_order_relaxed);
            runtime_dynamic_active_slot().store(true, std::memory_order_relaxed);
        }
    }

    static void set_global_runtime_tick_hz(int hz) noexcept {
        runtime_dwell_over_slot().store(0, std::memory_order_relaxed);
        runtime_dwell_under_slot().store(0, std::memory_order_relaxed);
        if (hz <= 0) {
            runtime_tick_mode_slot().store(static_cast<uint8_t>(TickMode::Dynamic), std::memory_order_relaxed);
            runtime_tick_hz_slot().store(0, std::memory_order_relaxed);
            runtime_dynamic_hz_slot().store(60, std::memory_order_relaxed);
            runtime_dynamic_active_slot().store(false, std::memory_order_relaxed);
        } else {
            runtime_tick_mode_slot().store(static_cast<uint8_t>(TickMode::Manual), std::memory_order_relaxed);
            runtime_tick_hz_slot().store(std::clamp(hz, 1, TickSchedule::kMaxHz), std::memory_order_relaxed);
            runtime_dynamic_hz_slot().store(snap_manual_tick_hz(hz), std::memory_order_relaxed);
        }
    }

    static void set_global_target_hz(int hz) noexcept {
        if (hz < 0) {
            set_global_runtime_tick_mode(TickMode::Dynamic);
        } else if (hz == 0) {
            runtime_tick_mode_slot().store(static_cast<uint8_t>(TickMode::Manual), std::memory_order_relaxed);
            runtime_tick_hz_slot().store(-1, std::memory_order_relaxed);
            runtime_dynamic_hz_slot().store(0, std::memory_order_relaxed);
            runtime_dwell_over_slot().store(0, std::memory_order_relaxed);
            runtime_dwell_under_slot().store(0, std::memory_order_relaxed);
        } else {
            const int snapped = snap_manual_tick_hz(hz);
            runtime_tick_mode_slot().store(static_cast<uint8_t>(TickMode::Manual), std::memory_order_relaxed);
            runtime_tick_hz_slot().store(snapped, std::memory_order_relaxed);
            runtime_dynamic_hz_slot().store(snapped, std::memory_order_relaxed);
            runtime_dwell_over_slot().store(0, std::memory_order_relaxed);
            runtime_dwell_under_slot().store(0, std::memory_order_relaxed);
        }
    }

    static double runtime_ewma_work_ms() noexcept {
        return runtime_ewma_work_ms_slot().load(std::memory_order_relaxed);
    }

    static int note_runtime_tick_work_ms(double work_ms) noexcept {
        if (!std::isfinite(work_ms) || work_ms < 0.0) work_ms = 0.0;
        const double prev = runtime_ewma_work_ms_slot().load(std::memory_order_relaxed);
        const double ewma = (prev <= 0.0) ? work_ms : (0.15 * work_ms + 0.85 * prev);
        runtime_ewma_work_ms_slot().store(ewma, std::memory_order_relaxed);
        runtime_dynamic_active_slot().store(true, std::memory_order_relaxed);
        if (runtime_tick_mode() != TickMode::Dynamic) {
            return effective_tick_hz(runtime_dynamic_hz_slot().load(std::memory_order_relaxed));
        }
        int cur_hz = std::clamp(runtime_dynamic_hz_slot().load(std::memory_order_relaxed), 30, 60);
        const double period_ms = 1000.0 / static_cast<double>(cur_hz);
        const double util = ewma / period_ms;
        if (util > 0.45 && cur_hz > 30) {
            runtime_dwell_under_slot().store(0, std::memory_order_relaxed);
            if (runtime_dwell_over_slot().fetch_add(1, std::memory_order_relaxed) + 1 >= 15) {
                cur_hz = std::max(30, cur_hz - 5);
                runtime_dynamic_hz_slot().store(cur_hz, std::memory_order_relaxed);
                runtime_dwell_over_slot().store(0, std::memory_order_relaxed);
            }
        } else if (util < 0.30 && cur_hz < 60) {
            runtime_dwell_over_slot().store(0, std::memory_order_relaxed);
            if (runtime_dwell_under_slot().fetch_add(1, std::memory_order_relaxed) + 1 >= 15) {
                cur_hz = std::min(60, cur_hz + 5);
                runtime_dynamic_hz_slot().store(cur_hz, std::memory_order_relaxed);
                runtime_dwell_under_slot().store(0, std::memory_order_relaxed);
            }
        } else {
            runtime_dwell_over_slot().store(0, std::memory_order_relaxed);
            runtime_dwell_under_slot().store(0, std::memory_order_relaxed);
        }
        return cur_hz;
    }

    static int effective_tick_hz(int configured_hz = 60) noexcept {
        const int rt = runtime_tick_hz_slot().load(std::memory_order_relaxed);
        if (rt < 0) return 0;
        if (rt > 0) return std::clamp(rt, 1, TickSchedule::kMaxHz);
        if (runtime_tick_mode() == TickMode::Dynamic && runtime_dynamic_active_slot().load(std::memory_order_relaxed)) {
            return std::clamp(runtime_dynamic_hz_slot().load(std::memory_order_relaxed), 30, 60);
        }
        return std::clamp(configured_hz, 1, TickSchedule::kMaxHz);
    }

    TickMode tick_mode() const noexcept { return tick_mode_; }
    int effective_hz() const noexcept { return effective_hz_; }
    int target_hz() const noexcept { return effective_hz_; }
    double ewma_work_ms() const noexcept { return ewma_work_ms_; }
    std::chrono::microseconds tick_budget() const noexcept { return budget_; }

    void set_target_hz(int hz, Clock::time_point now = Clock::now()) noexcept {
        dwell_over_ = 0;
        dwell_under_ = 0;
        if (hz < 0) {
            tick_mode_ = TickMode::Dynamic;
            effective_hz_ = 60;
            set_global_target_hz(-1);
        } else if (hz == 0) {
            tick_mode_ = TickMode::Manual;
            effective_hz_ = 0;
            set_global_target_hz(0);
        } else {
            tick_mode_ = TickMode::Manual;
            effective_hz_ = snap_manual_tick_hz(hz);
            set_global_target_hz(effective_hz_);
        }
        const int sched_hz = std::max(10, effective_hz_);
        budget_ = std::chrono::microseconds(1000000 / sched_hz);
        set_hz(sched_hz, now);
    }

    int note_tick_work_ms(double work_ms, Clock::time_point now = Clock::now()) noexcept {
        if (!std::isfinite(work_ms) || work_ms < 0.0) work_ms = 0.0;
        ewma_work_ms_ = (ewma_work_ms_ <= 0.0) ? work_ms : (0.15 * work_ms + 0.85 * ewma_work_ms_);
        (void)note_runtime_tick_work_ms(work_ms);
        if (tick_mode_ == TickMode::Dynamic) {
            const double period_ms = 1000.0 / static_cast<double>( std::max(30, effective_hz_) );
            const double util = ewma_work_ms_ / period_ms;
            if (util > 0.45 && effective_hz_ > 30) {
                dwell_under_ = 0;
                if (++dwell_over_ >= 15) {
                    effective_hz_ = std::max(30, effective_hz_ - 5);
                    dwell_over_ = 0;
                    set_hz(effective_hz_, now);
                }
            } else if (util < 0.30 && effective_hz_ < 60) {
                dwell_over_ = 0;
                if (++dwell_under_ >= 15) {
                    effective_hz_ = std::min(60, effective_hz_ + 5);
                    dwell_under_ = 0;
                    set_hz(effective_hz_, now);
                }
            } else {
                dwell_over_ = 0;
                dwell_under_ = 0;
            }
            runtime_dynamic_hz_slot().store(effective_hz_, std::memory_order_relaxed);
        }
        budget_ = std::chrono::microseconds(1000000 / std::max(1, effective_hz_));
        return effective_hz_;
    }

    void record_tick_work_ms(double work_ms) noexcept { (void)note_tick_work_ms(work_ms); }

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
            const Clock::time_point t0 = p.now();
            p.tick(t);
            if (p.after_tick) p.after_tick();
            now = p.now();
            const double work_ms = std::chrono::duration<double, std::milli>(now - t0).count();
            (void)note_tick_work_ms(work_ms, now);
        }
    }

    Config config_;
    TickSchedule schedule_;
    TickMode tick_mode_ = TickMode::Dynamic;
    int effective_hz_ = 60;
    double ewma_work_ms_ = 0.0;
    int dwell_over_ = 0;
    int dwell_under_ = 0;
    std::chrono::microseconds budget_{16666};
    Stats stats_;
    size_t since_tick_ = 0;
    uint64_t taken_caught_up_ = 0, taken_skipped_ = 0, taken_restarts_ = 0;
};

}}  // namespace aoc::worldd
