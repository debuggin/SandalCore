// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "npc/npc_world.h"

#include "protocol/emit/intrepid_netguid.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/value.h"

#include <algorithm>
#include <cmath>

namespace aoc::worldd::npc {

namespace repl = ::aoc::protocol::repl;
namespace emit = ::aoc::protocol::emit;

namespace {

struct RootWriteArgs {
    bool active = false;
    uint64_t montage = 0;
    double position = 0, x = 0, y = 0, z = 0, yaw = 0;
    nav::Vec3 vel{};
    bool obstructed = false;
};

void write_root_motion(world::WorldActor& a, const RootWriteArgs& w) {
    repl::ObjectState& obj = a.actor.obj;
    if (!obj.schema) return;
    const auto set = [&](const char* name, repl::Value v) {
        if (obj.schema->command(name)) obj.state.set(*obj.schema, name, std::move(v));
    };
    set("bRootMotionObstructed", repl::Value::boolean(w.obstructed));
    set("bReplicateMovement", repl::Value::boolean(true));
    set("RepRootMotion.Animation", repl::Value(emit::FIntrepidNetworkGUID::from_u64(w.active ? w.montage : 0)));
    set("RepRootMotion.bIsActive", repl::Value::boolean(w.active));
    set("RepRootMotion.Position", repl::Value::f32(static_cast<float>(w.position)));
    set("RepRootMotion.Location", repl::Value(repl::quantize_vector(100, w.x, w.y, w.z)));
    set("RepRootMotion.Rotation", repl::Value(repl::Rot{0, angle16(w.yaw), 0}));
    set("RepRootMotion.LinearVelocity", repl::Value(repl::quantize_vector(10, w.vel.x, w.vel.y, w.vel.z)));
}

std::optional<uint64_t> swap_movement_mode(world::WorldActor& a, uint64_t mode) {
    std::optional<uint64_t> old;
    repl::ObjectState& obj = a.actor.obj;
    if (!obj.schema || !obj.schema->command("ReplicatedMovementMode")) return old;
    if (const repl::Value* v = obj.state.get(*obj.schema, "ReplicatedMovementMode")) {
        if (const uint64_t* p = v->get_if<uint64_t>()) old = *p;
    }
    obj.state.set(*obj.schema, "ReplicatedMovementMode", repl::Value::u64(mode));
    return old;
}

void root_at(const std::vector<NpcWorld::RootMotionStep>& steps, double at, double* x, double* y) {
    *x = *y = 0;
    if (steps.empty()) return;
    if (at <= steps.front().time) {
        *x = steps.front().x;
        *y = steps.front().y;
        return;
    }
    for (size_t i = 1; i < steps.size(); ++i) {
        if (at > steps[i].time) continue;
        const double span = steps[i].time - steps[i - 1].time;
        const double f = span > 0 ? (at - steps[i - 1].time) / span : 1.0;
        *x = steps[i - 1].x + (steps[i].x - steps[i - 1].x) * f;
        *y = steps[i - 1].y + (steps[i].y - steps[i - 1].y) * f;
        return;
    }
    *x = steps.back().x;
    *y = steps.back().y;
}

struct FreezeContext {
    const NpcWorld::RootMotionPlay& play;
    const std::vector<NpcWorld::RootMotionBlock>& blocks;
    double yaw = 0;
};

bool in_any_freeze_box(const FreezeContext& fc, double cx, double cy) {
    if (!fc.play.freeze_stop && !fc.play.freeze_pause) return false;
    const double wide = fc.play.freeze_scale * fc.play.own_radius;
    for (const NpcWorld::RootMotionBlock& b : fc.blocks) {
        if (fc.play.ignore_non_target && !b.target) continue;
        if (NpcWorld::in_freeze_box(cx, cy, fc.yaw, fc.play.freeze_distance, wide, b.x, b.y, b.body)) return true;
    }
    return false;
}

double find_freeze_entry(const FreezeContext& fc, const nav::Vec3& g, double nx, double ny) {
    if (in_any_freeze_box(fc, g.x, g.y)) return 0.0;
    constexpr int kSub = 16;
    for (int i = 1; i <= kSub; ++i) {
        const double hi_f = static_cast<double>(i) / kSub;
        if (!in_any_freeze_box(fc, g.x + (nx - g.x) * hi_f, g.y + (ny - g.y) * hi_f)) continue;
        double lo = static_cast<double>(i - 1) / kSub, hi = hi_f;
        for (int k = 0; k < 14; ++k) {
            const double mid = 0.5 * (lo + hi);
            (in_any_freeze_box(fc, g.x + (nx - g.x) * mid, g.y + (ny - g.y) * mid) ? hi : lo) = mid;
        }
        return hi;
    }
    return 2.0;
}

struct FreezeStepState {
    double rx = 0, ry = 0, ox = 0, oy = 0, fx = 0, fy = 0;
    double nx = 0, ny = 0, skip_x = 0, skip_y = 0;
    bool frozen = false, obstructed = false;
};

void apply_freeze_box(const FreezeContext& fc, const nav::Vec3& g, FreezeStepState& st) {
    if (fc.play.freeze_stop && !st.frozen && in_any_freeze_box(fc, g.x, g.y)) st.frozen = true;
    if (st.frozen) {
        st.nx = g.x;
        st.ny = g.y;
        st.obstructed = true;
        return;
    }
    if (!fc.play.freeze_pause) return;
    const double entry = find_freeze_entry(fc, g, st.nx, st.ny);
    if (entry > 1.0) return;
    st.nx = g.x + (st.nx - g.x) * entry;
    st.ny = g.y + (st.ny - g.y) * entry;
    st.obstructed = true;
    const double ex = st.nx - st.ox, ey = st.ny - st.oy;
    st.skip_x = st.rx - (ex * st.fx + ey * st.fy);
    st.skip_y = st.ry - (-ex * st.fy + ey * st.fx);
}

void clamp_to_blockers(const std::vector<NpcWorld::RootMotionBlock>& blocks, const nav::Vec3& g, double& nx, double& ny) {
    for (const NpcWorld::RootMotionBlock& b : blocks) {
        if (!std::isfinite(b.x) || !std::isfinite(b.y) || !(b.radius > 0)) continue;
        const double sx = g.x - b.x, sy = g.y - b.y, dx = nx - g.x, dy = ny - g.y;
        const double a2 = dx * dx + dy * dy, was2 = sx * sx + sy * sy;
        if (a2 < 1e-12) continue;
        const double keep = std::min(b.radius, std::sqrt(was2));
        const double bq = sx * dx + sy * dy, cq = was2 - keep * keep, disc = bq * bq - a2 * cq;
        if (bq >= 0 || disc < 0) continue;
        const double f = std::max(0.0, (-bq - std::sqrt(disc)) / a2);
        if (f >= 1) continue;
        nx = g.x + dx * f;
        ny = g.y + dy * f;
    }
}

nav::Vec3 project_root_step(const nav::AreaNav& nav, const nav::Vec3& g, double nx, double ny) {
    nav::Vec3 to{nx, ny, g.z};
    if (nav.on_navmesh(g)) {
        nav::Vec3 edge;
        if (!nav.raycast(g, to, &edge)) {
            to.x = edge.x;
            to.y = edge.y;
        }
    }
    double gz = g.z;
    const double window = 80.0 + 2.0 * std::hypot(to.x - g.x, to.y - g.y);
    if (nav.ground_height(to.x, to.y, g.z, &gz, window, window)) {
        to.z = gz;
    } else {
        to = g;
    }
    return to;
}

void step_root_tracking(Mover& mover, NpcWorld::Where& track, double track_rate, double dt) {
    nav::Vec3 at_target;
    const nav::Vec3 g = mover.ground();
    if (track && track(at_target) && std::hypot(at_target.x - g.x, at_target.y - g.y) > 1.0)
        mover.turn_toward(std::atan2(at_target.y - g.y, at_target.x - g.x) * 180.0 / 3.14159265358979323846, dt, track_rate);
}

}  // namespace

bool NpcWorld::warp(uint64_t id, double x, double y, double ground_z) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.pursue = nullptr;
    n.track = nullptr;
    n.mover.stop();
    n.mover.place(x, y, ground_z, n.mover.yaw_degrees());
    n.mode = Mode::Stand;
    n.ordered_return = false;
    commit(n, now_, true);
    return true;
}

bool NpcWorld::order_move(uint64_t id, const nav::Vec3& dest, double speed_cm_s, double stop_distance) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.pursue = nullptr;
    if (!n.mover.move_to(dest, speed_cm_s, stop_distance, 0)) return false;
    n.mode = Mode::Ordered;
    ++stats_.moves_started;
    return true;
}

bool NpcWorld::set_yaw_rate(uint64_t id, double degrees_per_second) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    it->second.mover.set_yaw_rate(degrees_per_second);
    return true;
}

bool NpcWorld::set_speed_scale(uint64_t id, double scale) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    it->second.mover.set_speed_scale(scale);
    return true;
}

bool NpcWorld::release(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || it->second.mode != Mode::Ordered) return false;
    it->second.pursue = nullptr;
    go_idle(it->second);
    return true;
}

bool NpcWorld::halt(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.pursue = nullptr;
    n.track = nullptr;
    n.mover.stop();
    n.mode = Mode::Stand;
    n.ordered_return = false;
    commit(n, now_, true);
    return true;
}

bool NpcWorld::face_toward(uint64_t id, double x, double y) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    const world::WorldActor* a = registry_.find(id);
    if (!a) return false;
    const double dx = x - a->x, dy = y - a->y;
    if (std::fabs(dx) < 1e-6 && std::fabs(dy) < 1e-6) return true;
    Npc& n = it->second;
    if (n.rm) return false;
    n.mover.face(std::atan2(dy, dx) * 180.0 / 3.14159265358979323846);
    commit(n, now_, true);
    return true;
}

bool NpcWorld::turn_toward(uint64_t id, double x, double y, double rate) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !std::isfinite(x) || !std::isfinite(y)) return false;
    const world::WorldActor* a = registry_.find(id);
    if (!a) return false;
    const double dx = x - a->x, dy = y - a->y;
    if (std::fabs(dx) < 1e-6 && std::fabs(dy) < 1e-6) return true;
    Npc& n = it->second;
    if (n.rm) return false;
    n.turning = true;
    n.turn_yaw = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
    n.turn_rate = std::isfinite(rate) && rate > 0 ? rate : 0.0;
    return true;
}

bool NpcWorld::track_target(uint64_t id, Where where, double rate, double until) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !where) return false;
    Npc& n = it->second;
    n.track = std::move(where);
    n.track_rate = std::isfinite(rate) && rate > 0 ? rate : 0.0;
    n.track_until = std::isfinite(until) ? until : 0.0;
    return true;
}

void NpcWorld::stop_tracking(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it != npcs_.end()) it->second.track = nullptr;
}

bool NpcWorld::order_pursue(uint64_t id, Where where, double speed_cm_s, double stop_distance) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !where) return false;
    Npc& n = it->second;
    nav::Vec3 at;
    if (!where(at)) return false;
    if (!order_move(id, at, speed_cm_s, stop_distance)) return false;
    n.pursue = std::move(where);
    n.pursue_speed = speed_cm_s;
    n.pursue_stop = std::max(0.0, stop_distance);
    n.pursue_x = n.pursue_plan_x = at.x;
    n.pursue_y = n.pursue_plan_y = at.y;
    return true;
}

bool NpcWorld::order_return_home(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    Npc& n = it->second;
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.pursue = nullptr;
    n.track = nullptr;
    if (!n.mover.move_to(n.home, n.setup.run_speed, 0, 0)) return false;
    n.mode = Mode::Return;
    n.ordered_return = true;
    ++stats_.returns_home;
    return true;
}

bool NpcWorld::in_freeze_box(double cx, double cy, double yaw_degrees, double distance, double wide, double bx, double by, double body) {
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(bx) || !std::isfinite(by)) return false;
    constexpr double kPi = 3.14159265358979323846;
    const double fx = std::cos(yaw_degrees * kPi / 180.0), fy = std::sin(yaw_degrees * kPi / 180.0);
    const double ex = bx - cx, ey = by - cy;
    const double ahead = ex * fx + ey * fy, aside = -ex * fy + ey * fx;
    return ahead >= -body && ahead <= distance + wide + body && std::fabs(aside) <= wide + body;
}

bool NpcWorld::begin_root_motion(uint64_t id, RootMotionPlay play) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || it->second.pending_ground || play.steps.empty() || !(play.length > 0) || !std::isfinite(play.length) || !std::isfinite(play.rate) || !std::isfinite(play.blend_out)) return false;
    for (const RootMotionStep& s : play.steps) {
        if (!std::isfinite(s.time) || !std::isfinite(s.x) || !std::isfinite(s.y)) return false;
    }
    Npc& n = it->second;
    if (!registry_.find(id)) return false;
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.pursue = nullptr;
    RootMotion r;
    r.play = std::move(play);
    r.start = now_;
    r.yaw = n.mover.yaw_degrees();
    const nav::Vec3 g = n.mover.ground();
    r.ox = g.x;
    r.oy = g.y;
    n.rm = std::move(r);
    rooted_.push_back(id);
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    const uint64_t montage = n.rm->play.montage;
    std::optional<uint64_t> saved;
    registry_.modify(id, world::Change::Movement, [&](world::WorldActor& a) {
        saved = swap_movement_mode(a, repl::kMoveWalking);
        write_root_motion(a, {true, montage, 0.0, g.x, g.y, z, yaw, {}, false});
    });
    n.rm->saved_mode = saved;
    return true;
}

bool NpcWorld::end_root_motion(uint64_t id) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !it->second.rm) return false;
    finish_root_motion(it->second, now_);
    return true;
}

bool NpcWorld::root_motion_active(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it != npcs_.end() && it->second.rm.has_value();
}

void NpcWorld::advance_root_motion(Npc& n, double t) {
    RootMotion& r = *n.rm;
    const RootMotionPlay& p = r.play;
    world::WorldActor* a = registry_.find(n.id);
    if (!a) {
        n.rm.reset();
        rooted_.erase(std::remove(rooted_.begin(), rooted_.end(), n.id), rooted_.end());
        return;
    }
    const double dt = cfg_.step_seconds, rate = p.rate > 0 ? p.rate : 1.0;
    const double carry_end = std::max(0.0, p.length - std::max(0.0, p.blend_out));
    const double reached = std::max(0.0, t - r.start) * rate;
    const bool last = reached >= carry_end;
    const double at = last ? carry_end : reached;
    constexpr double kPi = 3.14159265358979323846;
    const nav::Vec3 g = n.mover.ground();
    if (n.track && n.track_until > 0 && t >= n.track_until) n.track = nullptr;
    step_root_tracking(n.mover, n.track, n.track_rate, dt);
    if (std::fabs(n.mover.yaw_degrees() - r.yaw) > 1e-9) {
        r.yaw = n.mover.yaw_degrees();
        r.ox = g.x - (std::cos(r.yaw * kPi / 180.0) * r.last_tx - std::sin(r.yaw * kPi / 180.0) * r.last_ty);
        r.oy = g.y - (std::sin(r.yaw * kPi / 180.0) * r.last_tx + std::cos(r.yaw * kPi / 180.0) * r.last_ty);
    }
    double rx = 0, ry = 0;
    root_at(p.steps, at, &rx, &ry);
    r.position = at;
    const double fx = std::cos(r.yaw * kPi / 180.0), fy = std::sin(r.yaw * kPi / 180.0);
    std::vector<RootMotionBlock> blocks;
    if (p.blockers) p.blockers(blocks);
    const FreezeContext fc{p, blocks, r.yaw};
    FreezeStepState fs{rx, ry, r.ox, r.oy, fx, fy,
                       r.ox + fx * (rx - r.skip_x) - fy * (ry - r.skip_y),
                       r.oy + fy * (rx - r.skip_x) + fx * (ry - r.skip_y),
                       r.skip_x, r.skip_y, r.frozen, false};
    apply_freeze_box(fc, g, fs);
    r.frozen = fs.frozen;
    r.skip_x = fs.skip_x;
    r.skip_y = fs.skip_y;
    r.last_tx = rx - r.skip_x;
    r.last_ty = ry - r.skip_y;
    clamp_to_blockers(blocks, g, fs.nx, fs.ny);
    const nav::Vec3 to = project_root_step(*nav_, g, fs.nx, fs.ny);
    if (p.freeze_stop && !r.frozen && in_any_freeze_box(fc, to.x, to.y)) r.frozen = true;
    n.mover.shift(to.x, to.y, to.z);
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    registry_.move(n.id, to.x, to.y, z, yaw);
    const nav::Vec3 vel = last ? nav::Vec3{} : nav::Vec3{(to.x - g.x) / dt, (to.y - g.y) / dt, 0.0};
    write_replicated(*a, {to.x, to.y, z, yaw, vel, false, t, 0});
    write_root_motion(*a, {!last, p.montage, at, to.x, to.y, z, yaw, vel, fs.obstructed && !last});
    n.reported_moving = false;
    ++stats_.writes;
    if (!last) return;
    if (r.saved_mode) swap_movement_mode(*a, *r.saved_mode);
    registry_.touch(*a, world::Change::State);
    n.rm.reset();
    rooted_.erase(std::remove(rooted_.begin(), rooted_.end(), n.id), rooted_.end());
    begin_settle(n, t);
}

void NpcWorld::finish_root_motion(Npc& n, double t) {
    if (!n.rm) return;
    const double position = n.rm->position;
    const std::optional<uint64_t> saved_mode = n.rm->saved_mode;
    n.rm.reset();
    rooted_.erase(std::remove(rooted_.begin(), rooted_.end(), n.id), rooted_.end());
    world::WorldActor* a = registry_.find(n.id);
    if (!a) return;
    if (saved_mode) swap_movement_mode(*a, *saved_mode);
    const nav::Vec3 g = n.mover.ground();
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    registry_.move(n.id, g.x, g.y, z, yaw);
    write_replicated(*a, {g.x, g.y, z, yaw, {}, false, t, 0});
    write_root_motion(*a, {false, 0, position, g.x, g.y, z, yaw, {}, false});
    registry_.touch(*a, world::Change::State);
    n.reported_moving = false;
    ++stats_.writes;
    begin_settle(n, t);
}

void NpcWorld::begin_settle(Npc& n, double t) {
    if (!(cfg_.settle_seconds > 0)) return;
    n.settle_began = t;
    n.settle_until = t + cfg_.settle_seconds;
    n.settle_sent = 0;
    if (std::find(settling_.begin(), settling_.end(), n.id) == settling_.end()) settling_.push_back(n.id);
}

void NpcWorld::settle(Npc& n, double t) {
    if (t <= n.settle_began + 1e-9) return;
    world::WorldActor* a = registry_.find(n.id);
    if (!a || n.rm || n.reported_moving || t > n.settle_until + 1e-9) {
        settling_.erase(std::remove(settling_.begin(), settling_.end(), n.id), settling_.end());
        return;
    }
    const nav::Vec3 g = n.mover.ground();
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    registry_.move(n.id, g.x, g.y, z, yaw);
    write_replicated(*a, {g.x, g.y, z, yaw, {}, false, t, ++n.settle_sent});
    ++stats_.writes;
}

const Mover* NpcWorld::mover(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it == npcs_.end() ? nullptr : &it->second.mover;
}

bool NpcWorld::stands(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it == npcs_.end() || it->second.mode == Mode::Stand;
}

bool NpcWorld::is_ordered(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it != npcs_.end() && it->second.mode == Mode::Ordered;
}

nav::Vec3 NpcWorld::home_of(uint64_t id) const {
    const auto it = npcs_.find(id);
    return it == npcs_.end() ? nav::Vec3{} : it->second.home;
}

bool NpcWorld::set_home(uint64_t id, const nav::Vec3& home) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end() || !std::isfinite(home.x) || !std::isfinite(home.y) || !std::isfinite(home.z)) return false;
    it->second.home = home;
    return true;
}

bool NpcWorld::spawn_point(uint64_t id, nav::Vec3* ground_home, nav::Vec3* ai_home) const {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    if (ground_home) *ground_home = it->second.spawn_home;
    if (ai_home) *ai_home = it->second.spawn_ai_home;
    return true;
}

bool NpcWorld::restore_home(uint64_t id, nav::Vec3* ground_home) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    it->second.home = it->second.spawn_home;
    if (ground_home) *ground_home = it->second.home;
    return true;
}

NpcWorld::RelocateResult NpcWorld::relocate(uint64_t id, double x, double y, double z, bool z_is_origin) {
    RelocateResult r;
    const auto it = npcs_.find(id);
    world::WorldActor* a = registry_.find(id);
    if (it == npcs_.end() || !a) {
        r.status = Relocated::UnknownNpc;
        return r;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        r.status = Relocated::NotANumber;
        return r;
    }
    Npc& n = it->second;
    if (n.pending_ground) {
        r.status = Relocated::WaitingForGround;
        return r;
    }
    if (!nav_->ready_at(x, y)) {
        r.status = Relocated::NotLoaded;
        return r;
    }
    const double foot = n.mover.foot_offset();
    double gz = z_is_origin ? z - foot : z;
    if (!z_is_origin) {
        double floor = 0;
        if (nav_->ground_height(x, y, z, &floor, 400.0, 400.0)) gz = floor;
    }
    if (n.rm) finish_root_motion(n, now_);
    n.turning = false;
    n.track = nullptr;
    n.mover.stop();
    n.mover.place(x, y, gz, n.mover.yaw_degrees());
    n.home = {x, y, gz};
    n.relocated = true;
    if (n.setup.ambient == Ambient::Patrol) n.setup.ambient = Ambient::Wander;
    n.grounded = true;
    n.data_feet = gz;
    n.ground_dz = 0;
    if (n.mode != Mode::Ordered) go_idle(n);
    commit(n, now_, true);
    registry_.touch(*a, world::Change::State);
    r.ground = n.mover.ground();
    r.origin_z = n.mover.origin_z();
    return r;
}

NpcWorld::Stats NpcWorld::stats() const {
    Stats s = stats_;
    s.npcs = npcs_.size();
    s.active_now = active_.size();
    for (const uint64_t id : active_) {
        const auto it = npcs_.find(id);
        if (it != npcs_.end() && it->second.mover.moving()) ++s.moving_now;
    }
    for (const auto& [id, n] : npcs_) {
        switch (n.mode) {
            case Mode::Stand: ++s.standing; break;
            case Mode::Idle: ++s.idling; break;
            case Mode::Wander: ++s.wandering; break;
            case Mode::Patrol: ++s.patrolling; break;
            case Mode::PatrolPause: ++s.patrol_pausing; break;
            case Mode::Return: ++s.returning; break;
            case Mode::Ordered: ++s.ordered; break;
        }
    }
    return s;
}

}  // namespace aoc::worldd::npc
