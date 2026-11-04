// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/player_session.h"

#include "protocol/repl/move_packed.h"
#include "spawns/mount_state.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {
// Twice the fastest allowed mount dash speed (1,741.5 cm/s) treats teleports/respawns as instant jumps without velocity.
constexpr double kPresenceJumpCmPerS = 3500.0;
constexpr auto kPresenceStill = std::chrono::milliseconds(200);

uint8_t shown_movement_mode(uint8_t mode) {
    switch (mode) {
        case repl::kMoveWalking:
        case repl::kMoveFalling:
        case repl::kMoveSwimming:
        case repl::kMoveFlying:
        case 16:
        case 17:
        case 18:
            return mode;
        default:
            return repl::kMoveWalking;
    }
}

void show_movement_mode(world::WorldActor& a, uint8_t mode) {
    repl::ObjectState& obj = a.actor.obj;
    obj.state.set(*obj.schema, "ReplicatedMovementMode", repl::Value::u64(shown_movement_mode(mode)));
}

world::WorldActor build_presence_pawn(std::optional<repl::Actor>& pawn, uint64_t player_id, const std::string& player_name,
                                      const GoodPosition& g, double stamp) {
    world::WorldActor p;
    if (!pawn) return p;
    p.id = pawn->guid.ObjectId;
    p.kind = world::ActorKind::Player;
    p.actor = std::move(*pawn);
    p.never_dormant = true;
    p.owner = player_id;
    p.player.name = player_name;
    p.label = player_name;
    p.x = g.x, p.y = g.y, p.z = g.z, p.yaw_degrees = g.yaw_degrees;
    spawns::write_motion(p, g.x, g.y, g.z, g.yaw_degrees, 0, 0, false, stamp);
    show_movement_mode(p, g.movement_mode);
    return p;
}
}  // namespace

void PlayerSession::sync_presence(Clock::time_point now) {
    if (!registry_ || finished_ || failed_ || !phase_in_done_ || presence_failed_) return;
    const GoodPosition here = checker_.good();
    const double stamp = pb_.setup().world_time + session_seconds(now);
    if (world_pawn_ == 0) {
        auto pawn = pb_.observer_pawn();
        world::WorldActor p = build_presence_pawn(pawn, static_cast<uint64_t>(pb_.setup().player_id), name(), here, stamp);
        const uint64_t pid = p.id;
        if (!pawn || !registry_->add(std::move(p))) {
            presence_failed_ = true;
            spdlog::warn("[players] {}: the pawn the other players are shown does not build or is refused by the registry: nobody sees this player", name());
            return;
        }
        world_pawn_ = pid;
        presence_at_ = here;
        presence_moving_ = false;
        presence_moved_at_ = now;
        spdlog::info("[players] {}: in the world for the other players (pawn {}) at ({:.0f}, {:.0f}, {:.0f}) cm", name(), pid, here.x, here.y, here.z);
        return;
    }
    // While mounted, the move checker tracks the mount's position, so keep the observer pawn stationary at the mount-up spot until dismount.
    const GoodPosition g = mounted() ? presence_at_ : here;
    const bool moved = g.x != presence_at_.x || g.y != presence_at_.y || g.z != presence_at_.z || g.yaw_degrees != presence_at_.yaw_degrees ||
                       g.movement_mode != presence_at_.movement_mode;
    if (!moved) {
        if (presence_moving_ && now - presence_moved_at_ >= kPresenceStill) {
            presence_moving_ = false;
            registry_->modify(world_pawn_, world::Change::Movement, [&](world::WorldActor& a) { spawns::write_motion(a, g.x, g.y, g.z, g.yaw_degrees, 0, 0, false, stamp); });
        }
        return;
    }
    double vx = 0, vy = 0;
    const double dt = (g.has_time_stamp && presence_at_.has_time_stamp) ? static_cast<double>(g.time_stamp) - static_cast<double>(presence_at_.time_stamp) : 0.0;
    if (dt > 0 && dt < 1.0) {
        vx = (g.x - presence_at_.x) / dt;
        vy = (g.y - presence_at_.y) / dt;
        if (std::hypot(vx, vy) > kPresenceJumpCmPerS) vx = vy = 0;
    }
    const bool moving = vx != 0 || vy != 0;
    presence_at_ = g;
    presence_moving_ = moving;
    presence_moved_at_ = now;
    registry_->move(world_pawn_, g.x, g.y, g.z, g.yaw_degrees);
    registry_->modify(world_pawn_, world::Change::Movement, [&](world::WorldActor& a) {
        spawns::write_motion(a, g.x, g.y, g.z, g.yaw_degrees, vx, vy, moving, stamp);
        show_movement_mode(a, g.movement_mode);
    });
}

void PlayerSession::leave_presence(const char* why) {
    if (!registry_ || world_pawn_ == 0) return;
    registry_->remove(world_pawn_);
    spdlog::info("[players] {}: out of the world for the other players ({})", name(), why);
    world_pawn_ = 0;
}

}}  // namespace aoc::worldd
