// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/player_session.h"

namespace aoc::worldd {

PlayerSession::TickPlan PlayerSession::tick_logic(Connection& conn, Clock::time_point now, bool plan_view) {
    TickPlan plan;
    last_now_ = now;
    pump(conn);
    send_move_ack(conn, now);
    if (finished_ || failed_) {
        plan.stopped = true;
        return plan;
    }
    session_seconds(now);
    run_abilities(conn, now);
    run_pets(conn, now);
    run_mounts(conn, now);
    run_ride(conn, now);
    run_timers(conn, now);
    run_gather(conn, now);
    run_foliage(conn, now);
    run_floor_net(conn, now);
    run_embersprings(conn, now);
    run_props(conn, now);
    run_dialogue(conn, now);
    run_quests(conn, now);
    run_warehouse(conn, now);
    run_crafting(conn, now);
    run_quick_make(conn, now);
    run_cargo(conn, now);
    run_nodes(conn);
    sync_item_abilities(conn);
    // Update the registry before view passes run so concurrent view threads observe a stable snapshot.
    sync_presence(now);
    if (phase_in_done_ && !time_syncs_.empty() && now >= time_syncs_.front() && room_for(1)) {
        time_syncs_.pop_front();
        if (time_syncs_.empty()) time_syncs_.push_back(now + kTimeSyncInterval);
        const double world_time = pb_.setup().world_time + session_seconds(now);
        ++time_syncs_sent_;
        queue("clock", [world_time](bootstrap::PlayerBootstrap& pb) { return pb.set_time(world_time); }, conn);
    }
    // Open town scenery before phase-in so the client's 10 m downward ground trace hits town bowl geometry without waiting out the 120 s cap.
    if (view_ && !phase_in_done_ && !scenery_done_ && join_.empty()) {
        const GoodPosition at = checker_.good();
        size_t opened = 0;
        scenery_done_ = view_->open_scenery(conn, at.x, at.y, now, &opened);
        if (opened > 0) {
            scenery_opened_ += opened;
            scenery_at_ = session_seconds(now);
        }
    }
    if (view_ && phase_in_done_ && join_.empty() && replies_.empty() && delayed_.empty()) {
        const GoodPosition at = checker_.good();
        plan.view = true;
        plan.x = at.x;
        plan.y = at.y;
        if (plan_view) plan.view_due = view_->plan(now);
    }
    return plan;
}

void PlayerSession::run_view(Connection& conn, const TickPlan& plan, Clock::time_point now) {
    if (view_ && plan.view) view_->run(conn, plan.x, plan.y, now, plan.view_due);
}

void PlayerSession::tick_after(Clock::time_point now, const TickPlan& plan) {
    if (plan.view) {
        if (npc_world_) npc_world_->note_player(static_cast<uint64_t>(pb_.setup().player_id), plan.x, plan.y);
        if (!position_log_started_) {
            position_log_started_ = true;
            last_position_log_ = now;
        } else if (now - last_position_log_ >= kPositionLogInterval) {
            last_position_log_ = now;
            log_position(now);
        }
    }
    if (!save_clock_started_) {
        save_clock_started_ = true;
        last_save_ = now;
        return;
    }
    if (now - last_save_ < save_interval_) return;
    last_save_ = now;
    if (position_changed(/*with_yaw=*/false)) hand_over("periodic save", /*periodic=*/true);
    if (inventory_dirty_) hand_over_inventory("periodic save", /*periodic=*/true);
}

}  // namespace aoc::worldd
