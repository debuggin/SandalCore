// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// net/tick_profile.h: per-phase tick timing counters for `world --loop-stats` (disabled by default; worker threads write to thread-local redirects).
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace aoc { namespace worldd {

struct TickProfile {
    bool on = false;
    double world_us = 0;       // PlayerWorld::tick_world and its parts
    double npcs_us = 0;        // the NPC world's steps and the towns' checks
    double combat_us = 0;      // the fights: pending hits, missiles, effects, the creatures' decisions
    double people_us = 0;      // the towns' people and roads following the nodes' levels
    double window_us = 0;      // the ground window (the chunks around the players)
    double aftermath_us = 0;   // corpses, loot and respawns
    double sessions_us = 0;    // PlayerSession::tick of every session (its view included)
    double view_us = 0;        // ActorView::tick (the view of the world of every player)
    double view_scan_us = 0;   // of which the passes' registry queries, rings and ordering (the rest is opens, updates and closes)
    uint64_t view_passes = 0;
    double open_us = 0;        // of which the opens (an actor's whole state, new to the client, and its exports)
    uint64_t opens = 0;
    double update_us = 0;      // of which the updates of actors the client holds awake that changed
    double update_build_us = 0;  // ... of which the building of what changed (the rest: the sending)
    uint64_t updates = 0;
    double flush_us = 0;       // Connection::flush of every session: the packets built and sent
    std::string top_name;      // the session that cost the most since the profile started (its tick and its sends)
    double top_us = 0, top_view_us = 0;
    size_t top_awake = 0, top_dormant = 0;

    void restart() {
        const bool keep = on;
        *this = TickProfile{};
        on = keep;
    }

    void add_view(const TickProfile& o) {
        view_us += o.view_us;
        view_scan_us += o.view_scan_us;
        view_passes += o.view_passes;
        open_us += o.open_us;
        opens += o.opens;
        update_us += o.update_us;
        update_build_us += o.update_build_us;
        updates += o.updates;
    }
};

// Worker threads running view passes redirect tick_profile() to their own local instance so no two threads write the same counters.
inline TickProfile*& tick_profile_redirect() {
    thread_local TickProfile* redirect = nullptr;
    return redirect;
}

inline TickProfile& tick_profile() {
    if (TickProfile* mine = tick_profile_redirect()) return *mine;
    static TickProfile profile;
    return profile;
}

inline double tick_profile_us_since(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - from).count();
}

class TickProfileSpan {
public:
    explicit TickProfileSpan(double* slot) : slot_(tick_profile().on ? slot : nullptr) {
        if (slot_) started_ = std::chrono::steady_clock::now();
    }
    ~TickProfileSpan() {
        if (slot_) *slot_ += tick_profile_us_since(started_);
    }
    TickProfileSpan(const TickProfileSpan&) = delete;
    TickProfileSpan& operator=(const TickProfileSpan&) = delete;

private:
    double* slot_;
    std::chrono::steady_clock::time_point started_{};
};

}}  // namespace aoc::worldd
