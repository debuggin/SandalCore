// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "worldd_loop.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <vector>

#if defined(_MSC_VER)
// Windows timer resolution defaults to 15.6 ms unless 1 ms is requested for fixed-rate socket waits.
#pragma comment(lib, "winmm.lib")
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int period_ms);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int period_ms);
#endif

namespace aoc { namespace worldd {

void LoopStats::note_datagram(size_t bytes, double us) {
    ++datagrams;
    bytes_in += bytes;
    receive_us += us;
}

void LoopStats::note_unit(double us) {
    ++units;
    busy_us += us;
    unit_us += us;
    max_us = std::max(max_us, us);
    int i = us < 1.0 ? 0 : static_cast<int>(8.0 * std::log2(us));
    if (i >= kBuckets) i = kBuckets - 1;
    ++hist[static_cast<size_t>(i)];
}

double LoopStats::percentile_us(double p) const {
    if (units == 0) return 0;
    const uint64_t want = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(p * static_cast<double>(units))));
    uint64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
        seen += hist[static_cast<size_t>(i)];
        if (seen >= want) return std::min(std::exp2(static_cast<double>(i + 1) / 8.0), max_us);
    }
    return max_us;
}

std::string LoopStats::buckets() const {
    std::string out;
    for (int i = 0; i < kBuckets; ++i) {
        if (hist[static_cast<size_t>(i)] == 0) continue;
        if (!out.empty()) out += ',';
        out += std::to_string(i) + ":" + std::to_string(hist[static_cast<size_t>(i)]);
    }
    return out;
}

namespace {

void guarded_datagram(Endpoint& endpoint, const Address& from, const uint8_t* data, size_t n, Clock::time_point now) {
    try {
        endpoint.on_datagram(from, data, n, now);
    } catch (const std::exception& e) {
        spdlog::error("a datagram from {} threw {}: dropping its session, if any", from.to_string(), e.what());
        endpoint.drop(from, "threw an exception");
    }
}

void guarded_tick(Endpoint& endpoint, size_t& tick_errors, Clock::time_point now) {
    try {
        endpoint.drain_inbound(now);
        endpoint.tick(now);
        endpoint.drain_outbound();
        tick_errors = 0;
    } catch (const std::exception& e) {
        if (++tick_errors == 1 || tick_errors % 1000 == 0) {
            spdlog::error("the tick threw {} ({} in a row): going on", e.what(), tick_errors);
        }
    }
}

void record_tick_budget(const WorldLoopContext& ctx, const TickProfile& profile, double unit_us) {
    if (!ctx.tick_budget) return;
    const auto state = world::evaluate_border_handover(ctx.signed_border_dist_cm, ctx.border_state);
    (void)world::complete_border_handover(state, ctx.border_lease_epoch, ctx.border_lease_epoch);
    const uint64_t sim_us = static_cast<uint64_t>(std::max(0.0, profile.world_us));
    const uint64_t aoi_us = static_cast<uint64_t>(std::max(0.0, profile.view_us));
    const uint64_t out_us = static_cast<uint64_t>(std::max(0.0, profile.flush_us));
    const uint64_t total_us = static_cast<uint64_t>(std::max(0.0, unit_us));
    const uint64_t Accounted = sim_us + aoi_us + out_us;
    const uint64_t in_us = total_us > Accounted ? total_us - Accounted : 0;
    ctx.tick_budget->record_tick_phases(in_us, sim_us, aoi_us, out_us);
}

void run_unpaced_world_loop(const WorldLoopContext& ctx, TickProfile& profile, WorldLoopStatsWindow& window) {
    std::vector<uint8_t> buf(4096);
    size_t tick_errors = 0;
    while (!ctx.stop_flag) {
        Address from;
        const auto n = ctx.socket.receive(buf.data(), buf.size(), from, std::chrono::milliseconds(10));
        const auto now = Clock::now();
        if (!n) {
            spdlog::error("the UDP socket failed; stopping");
            ctx.exit_code = 1;
            break;
        }
        if (*n > 0) {
            guarded_datagram(ctx.endpoint, from, buf.data(), *n, now);
            if (profile.on) ctx.loop_stats.note_datagram(*n, us_between(now, Clock::now()));
        }
        guarded_tick(ctx.endpoint, tick_errors, now);
        ctx.admin_queue.drain();
        if (ctx.stream) {
            ctx.stream->publish();
            ctx.stream->note_loop(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now));
        }
        const auto done = Clock::now();
        const double unit_us = us_between(now, done);
        record_tick_budget(ctx, profile, unit_us);
        if (profile.on) {
            ctx.loop_stats.note_unit(unit_us);
            log_world_loop_stats(ctx, nullptr, profile, window, done);
        }
    }
}

void run_paced_world_loop(const WorldLoopContext& ctx, TickLoop& tick_loop, TickProfile& profile,
                          WorldLoopStatsWindow& window) {
#if defined(_MSC_VER)
    timeBeginPeriod(1);
#endif
    std::vector<uint8_t> buf(4096);
    size_t tick_errors = 0;
    Address from;
    TickLoop::Ports ports;
    ports.now = [] { return Clock::now(); };
    ports.receive = [&](std::chrono::microseconds timeout) {
        return ctx.socket.receive(buf.data(), buf.size(), from, timeout);
    };
    ports.datagram = [&](size_t n, Clock::time_point now) {
        guarded_datagram(ctx.endpoint, from, buf.data(), n, now);
        if (profile.on) ctx.loop_stats.note_datagram(n, us_between(now, Clock::now()));
    };
    ports.tick = [&](Clock::time_point t) {
        const auto started = Clock::now();
        guarded_tick(ctx.endpoint, tick_errors, t);
        ctx.admin_queue.drain();
        if (ctx.stream) ctx.stream->publish();
        const auto done = Clock::now();
        if (ctx.stream) ctx.stream->note_loop(std::chrono::duration_cast<std::chrono::microseconds>(done - started));
        const double unit_us = us_between(started, done);
        record_tick_budget(ctx, profile, unit_us);
        if (profile.on) ctx.loop_stats.note_unit(unit_us);
    };
    while (!ctx.stop_flag) {
        if (!tick_loop.run_once(ports)) {
            spdlog::error("the UDP socket failed; stopping");
            ctx.exit_code = 1;
            break;
        }
        if (profile.on) log_world_loop_stats(ctx, &tick_loop, profile, window, Clock::now());
    }
#if defined(_MSC_VER)
    timeEndPeriod(1);
#endif
}

}  // namespace

void log_world_loop_stats(const WorldLoopContext& ctx, TickLoop* tick_loop, TickProfile& profile,
                          WorldLoopStatsWindow& window, Clock::time_point at) {
    const double window_s = std::chrono::duration<double>(at - window.window_start).count();
    if (ctx.loop_stats_seconds <= 0 || window_s < ctx.loop_stats_seconds) return;
    const double busy_us = ctx.loop_stats.busy_us + (tick_loop ? ctx.loop_stats.receive_us : 0.0);
    spdlog::info("[loop] {:.1f} s: passes={} busy={:.2f}% p50={:.3f} p99={:.3f} max={:.3f} ms datagrams={} in={} out={} packets_out={} sessions={} save_queue={} saved={} save_failed={} "
                 "save_dropped={} hist={}",
                 window_s, ctx.loop_stats.units, busy_us / (window_s * 1e6) * 100.0, ctx.loop_stats.percentile_us(0.50) / 1000.0,
                 ctx.loop_stats.percentile_us(0.99) / 1000.0, ctx.loop_stats.max_us / 1000.0, ctx.loop_stats.datagrams,
                 ctx.loop_stats.bytes_in, ctx.loop_stats.bytes_out, ctx.loop_stats.packets_out, ctx.endpoint.session_count(),
                 ctx.saver.queued(), ctx.saver.saved(), ctx.saver.failed(), ctx.saver.dropped(), ctx.loop_stats.buckets());
    ctx.endpoint.take_costliest(profile);
    const TickLoop::Stats ls = tick_loop ? tick_loop->take_stats() : TickLoop::Stats{};
    const uint64_t ticks = tick_loop ? ls.ticks : ctx.loop_stats.units;
    const double late_ms = tick_loop ? std::chrono::duration<double, std::milli>(tick_loop->schedule().late_max()).count() : 0.0;
    spdlog::info("[tick] {:.1f} s: rate={} ticks={} ({:.1f}/s) datagrams={:.1f}/s ({:.2f} a tick, at most {} between two ticks, at most {} in one read); ms: receive={:.1f} tick={:.1f} "
                 "world={:.1f} (npcs={:.1f} combat={:.1f} people={:.1f} window={:.1f} aftermath={:.1f}) sessions={:.1f} view={:.1f} (scan={:.1f}, {} passes; opens={:.1f}, {}; updates={:.1f}, {}: "
                 "build={:.1f}) flush={:.1f}; late_max={:.1f} ms "
                 "caught_up={} skipped={} restarts={}; costliest session: {} {:.1f} ms (view {:.1f}; {} awake, {} dormant)",
                 window_s, ctx.tick_hz, ticks, static_cast<double>(ticks) / window_s, static_cast<double>(ctx.loop_stats.datagrams) / window_s,
                 ticks ? static_cast<double>(ctx.loop_stats.datagrams) / static_cast<double>(ticks) : 0.0, ls.max_datagrams_per_tick, ls.max_batch,
                 ctx.loop_stats.receive_us / 1000.0, ctx.loop_stats.unit_us / 1000.0, profile.world_us / 1000.0, profile.npcs_us / 1000.0,
                 profile.combat_us / 1000.0, profile.people_us / 1000.0, profile.window_us / 1000.0, profile.aftermath_us / 1000.0,
                 profile.sessions_us / 1000.0, profile.view_us / 1000.0, profile.view_scan_us / 1000.0, profile.view_passes,
                 profile.open_us / 1000.0, profile.opens, profile.update_us / 1000.0, profile.updates, profile.update_build_us / 1000.0,
                 profile.flush_us / 1000.0, late_ms, ls.caught_up, ls.skipped, ls.restarts,
                 profile.top_name.empty() ? std::string("none") : profile.top_name, profile.top_us / 1000.0, profile.top_view_us / 1000.0,
                 profile.top_awake, profile.top_dormant);
    if (ctx.shared_updates) {
        const SharedUpdates::Stats now = ctx.shared_updates->stats();
        spdlog::info("[share] {:.1f} s: mode={} updates built once for several players: used={} built={}; built for one player only={}; checked={} differed={}",
                     window_s, SharedUpdates::name(ctx.shared_updates->mode()), now.hits - window.share_before.hits,
                     now.builds - window.share_before.builds, now.private_builds - window.share_before.private_builds,
                     now.verified - window.share_before.verified, now.mismatches - window.share_before.mismatches);
        window.share_before = now;
    }
    if (ctx.endpoint.view_threads() > 0) {
        const double busy = ctx.endpoint.view_busy_seconds();
        const uint64_t jobs = ctx.endpoint.view_jobs();
        spdlog::info("[view-threads] {:.1f} s: {} thread(s) ran the players' views: {:.1f} ms of thread time in {} jobs ({:.1f} us each; view= in the [tick] line is this time added up, not wall time)",
                     window_s, ctx.endpoint.view_threads(), (busy - window.view_busy_before) * 1000.0, jobs - window.view_jobs_before,
                     jobs > window.view_jobs_before ? (busy - window.view_busy_before) * 1e6 / static_cast<double>(jobs - window.view_jobs_before) : 0.0);
        window.view_busy_before = busy;
        window.view_jobs_before = jobs;
    }
    if (tick_loop) tick_loop->schedule().reset_late_max();
    ctx.loop_stats = LoopStats{};
    profile.restart();
    window.window_start = at;
}

void run_world_loop(const WorldLoopContext& ctx) {
    TickProfile& profile = tick_profile();
    profile.on = ctx.loop_stats_seconds > 0;
    WorldLoopStatsWindow window{Clock::now(), {}, 0.0, 0};
    spdlog::info("listening on UDP {} (receive buffer {} KB)", ctx.socket.local_address().to_string(),
                 ctx.socket.receive_buffer_bytes() / 1024);
    if (ctx.tick_hz <= 0) {
        run_unpaced_world_loop(ctx, profile, window);
    } else {
        TickLoop tick_loop(TickLoop::Config{ctx.tick_hz, 4, 256}, Clock::now());
        run_paced_world_loop(ctx, tick_loop, profile, window);
    }
}

void drain_shutdown_window(UdpSocket& socket, Endpoint& endpoint, std::chrono::milliseconds duration) {
    std::vector<uint8_t> buf(4096);
    size_t tick_errors = 0;
    for (const auto until = Clock::now() + duration; Clock::now() < until;) {
        Address from;
        const auto n = socket.receive(buf.data(), buf.size(), from, std::chrono::milliseconds(10));
        if (n && *n > 0) guarded_datagram(endpoint, from, buf.data(), *n, Clock::now());
        guarded_tick(endpoint, tick_errors, Clock::now());
    }
}

}}  // namespace aoc::worldd
