// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "net/endpoint.h"
#include "net/tick_loop.h"
#include "net/udp_socket.h"
#include "session/position_saver.h"
#include "session/shared_updates.h"
#include "svc/world_stream.h"
#include "world/mesh_border.h"

namespace aoc { namespace worldd {

inline double us_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

struct LoopStats {
    static constexpr int kBuckets = 8 * 24;
    std::array<uint32_t, kBuckets> hist{};
    uint64_t units = 0, datagrams = 0, bytes_in = 0, bytes_out = 0, packets_out = 0;
    double busy_us = 0, max_us = 0, unit_us = 0, receive_us = 0;

    void note_datagram(size_t bytes, double us);
    void note_unit(double us);
    double percentile_us(double p) const;
    std::string buckets() const;
};

struct WorldLoopContext {
    UdpSocket& socket;
    Endpoint& endpoint;
    CharacterSaver& saver;
    svc::AdminQueue& admin_queue;
    svc::WorldStreamServer* stream = nullptr;
    const SharedUpdates* shared_updates = nullptr;
    LoopStats& loop_stats;
    std::atomic<bool>& stop_flag;
    std::atomic<int>& exit_code;
    int tick_hz = 30;
    double loop_stats_seconds = 0.0;
    TickLoopBudget* tick_budget = nullptr;
    world::AuthorityState border_state = world::AuthorityState::Local;
    float signed_border_dist_cm = -world::kBorderHysteresisBandCm * 2.0f;
    uint64_t border_lease_epoch = 0;
};

struct WorldLoopStatsWindow {
    Clock::time_point window_start = Clock::now();
    SharedUpdates::Stats share_before{};
    double view_busy_before = 0.0;
    uint64_t view_jobs_before = 0;
};

void log_world_loop_stats(const WorldLoopContext& ctx, TickLoop* tick_loop, TickProfile& profile,
                          WorldLoopStatsWindow& window, Clock::time_point at);

void run_world_loop(const WorldLoopContext& ctx);

void drain_shutdown_window(UdpSocket& socket, Endpoint& endpoint,
                           std::chrono::milliseconds duration = std::chrono::milliseconds(500));

}}  // namespace aoc::worldd
