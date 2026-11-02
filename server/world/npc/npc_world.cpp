// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "npc/npc_world.h"

#include "protocol/repl/codecs.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/value.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aoc::worldd::npc {

namespace repl = ::aoc::protocol::repl;

namespace {

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint16_t angle16(double degrees) {
    double d = std::fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    return static_cast<uint16_t>(std::lround(d / 360.0 * 65536.0) & 0xFFFF);
}

uint16_t angle8(double degrees) {
    double d = std::fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    return static_cast<uint16_t>(std::lround(d / 360.0 * 256.0) & 0xFF);
}

double dist2d(const nav::Vec3& a, const nav::Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

size_t nearest_waypoint_index(const std::vector<nav::Vec3>& route, const nav::Vec3& home) {
    size_t best_idx = 0;
    double best = 1e30;
    for (size_t i = 0; i < route.size(); ++i) {
        const double d = dist2d(route[i], home);
        if (d < best) {
            best = d;
            best_idx = i;
        }
    }
    return best_idx;
}

bool finite_setup(const NpcMotionSetup& s) {
    for (double v : {s.home_yaw, s.foot_offset, s.yaw_rate, s.wander_min, s.wander_max, s.wander_delay, s.wander_delay_deviation, s.walk_speed, s.patrol_speed, s.run_speed, s.leg_median, s.leg_p90,
                     s.arrival_distance, s.awake_radius}) {
        if (!std::isfinite(v)) return false;
    }
    for (const nav::Vec3& p : s.route) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    }
    for (double p : s.pauses) {
        if (!std::isfinite(p)) return false;
    }
    return s.foot_offset >= 0 && s.foot_offset < 5000 && s.wander_min >= 0 && s.wander_max >= s.wander_min && s.walk_speed >= 0 && s.patrol_speed >= 0 && s.run_speed >= 0;
}

struct RepWriteArgs {
    double x = 0, y = 0, z = 0, yaw = 0;
    nav::Vec3 vel{};
    bool moving = false;
    double stamp = 0;
    int again = 0;
};

// Ensure bReplicateMovement is enabled on every write because retail NPC templates do not always enable it by default.
void write_replicated(world::WorldActor& a, const RepWriteArgs& w) {
    repl::ObjectState& obj = a.actor.obj;
    if (!obj.schema) return;
    repl::RepMovement m;
    if (const repl::Value* v = obj.state.get(7)) {
        if (const auto* old = v->get_if<repl::RepMovement>()) m = *old;
    }
    if (obj.schema->command("bReplicateMovement")) obj.state.set(*obj.schema, "bReplicateMovement", repl::Value::boolean(true));
    m.simulated_physic_sleep = false;
    m.rep_physics = false;
    m.location = repl::quantize_location(obj.opts, w.x, w.y, w.z);
    m.linear_velocity = repl::quantize_velocity(obj.opts, w.vel.x, w.vel.y, w.vel.z);
    m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, w.again, 0, 0};
    m.acceleration.reset();
    m.rotation = repl::Rot{0, obj.opts.rep_movement_short_rotation ? angle16(w.yaw) : angle8(w.yaw), 0};
    if (obj.schema->command("ReplicatedMovement")) obj.state.set(*obj.schema, "ReplicatedMovement", repl::Value(m));
    if (obj.schema->command("ReplicatedServerLastTransformUpdateTimeStamp"))
        obj.state.set(*obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp", repl::Value::f32(static_cast<float>(w.stamp)));
    if (obj.schema->command("CurrentMovementInput"))
        obj.state.set(*obj.schema, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{static_cast<uint8_t>(w.moving ? repl::PlayerMovementInput::kForward : 0)}));
    a.actor.open.spawn.location = repl::quantize_vector(10, w.x, w.y, w.z);
    a.actor.open.spawn.rotation = repl::Rot{0, angle16(w.yaw), 0};
    a.actor.open.spawn.velocity.reset();
}

}  // namespace

NpcWorld::NpcWorld(world::ActorRegistry& registry, const nav::AreaNav* nav, NpcWorldConfig config) : registry_(registry), nav_(nav), cfg_(config) {
    remove_listener_ = registry_.on_remove([this](const world::WorldActor& a) { remove(a.id); });
}

NpcWorld::~NpcWorld() { registry_.off_remove(remove_listener_); }

bool NpcWorld::remove(uint64_t actor_id) {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return false;
    if (it->second.pending_ground && pending_count_ > 0) --pending_count_;
    npcs_.erase(it);
    active_.erase(std::remove(active_.begin(), active_.end(), actor_id), active_.end());
    rooted_.erase(std::remove(rooted_.begin(), rooted_.end(), actor_id), rooted_.end());
    settling_.erase(std::remove(settling_.begin(), settling_.end(), actor_id), settling_.end());
    return true;
}

double NpcWorld::next_unit(Npc& n) {
    return static_cast<double>(splitmix(n.rng) >> 11) / 9007199254740992.0;
}

bool NpcWorld::add(uint64_t actor_id, const NpcMotionSetup& setup, bool* ground_found, bool* pending) {
    if (ground_found) *ground_found = false;
    if (pending) *pending = false;
    if (!nav_ || npcs_.count(actor_id) || !finite_setup(setup)) return false;
    world::WorldActor* a = registry_.find(actor_id);
    if (!a || (a->kind != world::ActorKind::Npc && a->kind != world::ActorKind::Station)) return false;
    Npc n;
    n.id = actor_id;
    n.setup = setup;
    n.rng = actor_id * 0x9E3779B97F4A7C15ull ^ 0xA5A5A5A5A5A5A5A5ull;
    n.mover.init(nav_, setup.foot_offset, setup.yaw_rate);
    if (!nav_->ready_at(a->x, a->y)) {
        // City node NPCs (ground_by_data) keep their data height because baked terrain under a city bowl is 50 m below its floor.
        double terrain = 0;
        const bool have_terrain = !a->npc.ground_by_data && nav_->terrain_height(a->x, a->y, &terrain);
        const double hint = have_terrain ? terrain : a->z - setup.foot_offset;
        n.mover.place(a->x, a->y, hint, setup.home_yaw);
        n.mode = Mode::Stand;
        n.home = {a->x, a->y, hint};
        n.pending_ground = true;
        n.data_z = a->z;
        if (have_terrain) {
            registry_.move(actor_id, a->x, a->y, n.mover.origin_z(), n.mover.yaw_degrees());
            write_replicated(*a, {a->x, a->y, n.mover.origin_z(), n.mover.yaw_degrees(), {}, false, now_, 0});
        }
        pending_by_chunk_[nav_->chunk_at(a->x, a->y)].push_back(actor_id);
        ++pending_count_;
        n.spawn_home = n.home;
        n.spawn_ai_home = {a->npc.home_x, a->npc.home_y, a->npc.home_z};
        npcs_.emplace(actor_id, std::move(n));
        if (pending) *pending = true;
        return true;
    }
    const bool found = ground_npc(n, *a);
    if (ground_found) *ground_found = found;
    n.spawn_home = n.home;
    n.spawn_ai_home = {a->npc.home_x, a->npc.home_y, a->npc.home_z};
    npcs_.emplace(actor_id, std::move(n));
    return true;
}

bool NpcWorld::ground_npc(Npc& n, world::WorldActor& actor) {
    world::WorldActor* a = &actor;
    const NpcMotionSetup& setup = n.setup;
    double gz = 0, terrain = 0;
    double hint = (n.pending_ground_done ? n.data_z : a->z) - setup.foot_offset;
    bool found = false;
    if (a->npc.ground_by_terrain && nav_->terrain_height(a->x, a->y, &terrain)) {
        hint = terrain;
        found = nav_->ground_height(a->x, a->y, hint, &gz, 1500.0, 1500.0);
    } else {
        found = a->npc.ground_by_data ? ground_by_data_at(a->x, a->y, hint, &gz)
                                      : nav_->ground_height(a->x, a->y, hint, &gz, 1500.0, 1500.0);
        if (!found && !a->npc.ground_by_data && nav_->terrain_height(a->x, a->y, &terrain)) {
            hint = terrain;
            found = nav_->ground_height(a->x, a->y, hint, &gz, 1500.0, 1500.0);
        }
        double wide = 0;
        if (!found && a->npc.snaps_to_ground && nav_->ground_height(a->x, a->y, hint, &wide, 1500.0, 1500.0) && (!a->npc.flies || wide >= hint)) {
            gz = wide;
            found = true;
        }
    }
    n.grounded = found;
    n.data_feet = hint;
    n.ground_dz = found ? gz - hint : 0.0;
    if (!found) {
        n.mover.place(a->x, a->y, hint, setup.home_yaw);
        n.mode = Mode::Stand;
        n.home = {a->x, a->y, hint};
        return false;
    }
    n.mover.place(a->x, a->y, gz, setup.home_yaw);
    n.home = {a->x, a->y, gz};
    switch (setup.ambient) {
        case Ambient::Stationary:
            n.mode = Mode::Stand;
            break;
        case Ambient::Wander:
            n.mode = Mode::Idle;
            n.timer = cfg_.min_idle + next_unit(n) * (cfg_.max_idle - cfg_.min_idle) * 0.5;
            break;
        case Ambient::Patrol:
            if (setup.route.size() >= 2) {
                n.mode = Mode::PatrolPause;
                n.timer = next_unit(n) * 3.0;
                n.wp = setup.start_closest ? nearest_waypoint_index(setup.route, n.home) : 0;
                n.wp_dir = 1;
                n.first_leg = true;
            } else {
                n.mode = Mode::Stand;
            }
            break;
    }
    const double yaw = n.mover.yaw_degrees();
    registry_.move(n.id, a->x, a->y, n.mover.origin_z(), yaw);
    write_replicated(*a, {a->x, a->y, n.mover.origin_z(), yaw, {}, false, now_, 0});
    return true;
}

bool NpcWorld::ground_by_data_at(double x, double y, double hint, double* gz) const {
    if (nav_->ground_height(x, y, hint, gz, cfg_.data_ground_below_cm, cfg_.data_ground_above_cm)) return true;
    // within the agent's radius of a wall the map has no polygon at the exact place (a villager posted at a stall): the floor of the nearest polygon within a small radius, in the same window
    const double lo = hint - cfg_.data_ground_below_cm, hi = hint + cfg_.data_ground_above_cm;
    nav::Vec3 stand;
    if (!nav_->nearest_point({x, y, 0.5 * (lo + hi)}, cfg_.data_ground_near_cm, 0.5 * (hi - lo), &stand) || std::hypot(stand.x - x, stand.y - y) > cfg_.data_ground_near_cm || stand.z < lo || stand.z > hi)
        return false;   // (the nearest polygon is found by bounding boxes: it may be farther than the radius)
    *gz = stand.z;
    return true;
}

NpcWorld::StandReport NpcWorld::stand_report(const std::vector<uint64_t>& ids) const {
    StandReport r;
    for (const uint64_t id : ids) {
        const auto it = npcs_.find(id);
        if (it == npcs_.end()) continue;
        const Npc& n = it->second;
        ++r.people;
        if (n.pending_ground) {
            ++r.waiting;
            continue;
        }
        if (n.grounded) {
            if (std::fabs(n.ground_dz) <= kStandNearCm) {
                ++r.on_floor;
            } else {
                ++r.moved;
                r.max_moved_cm = std::max(r.max_moved_cm, std::fabs(n.ground_dz));
            }
            continue;
        }
        ++r.no_floor;
        if (n.setup.ambient != Ambient::Stationary) ++r.stuck;
        // no floor in reach: is there any walkable ground near it at all?  If not it stands in a wall, a stall, under a floor or in the air
        const world::WorldActor* a = registry_.find(id);
        nav::Vec3 near_ground;
        if (a) {
            const bool ground_near = nav_->nearest_point({a->x, a->y, n.data_feet + 0.5 * (kBlockedAboveCm - kBlockedBelowCm)}, kBlockedCm, 0.5 * (kBlockedAboveCm + kBlockedBelowCm), &near_ground) &&
                                     std::hypot(near_ground.x - a->x, near_ground.y - a->y) <= kBlockedCm && near_ground.z >= n.data_feet - kBlockedBelowCm && near_ground.z <= n.data_feet + kBlockedAboveCm;
            if (!ground_near) ++r.blocked;
        }
    }
    return r;
}

void NpcWorld::chunks_resident(const std::vector<nav::ChunkKey>& chunks) {
    for (const nav::ChunkKey& key : chunks) {
        const auto it = pending_by_chunk_.find(key);
        if (it == pending_by_chunk_.end()) continue;
        for (const uint64_t id : it->second) {
            const auto n = npcs_.find(id);
            if (n == npcs_.end() || !n->second.pending_ground) continue;
            world::WorldActor* a = registry_.find(id);
            if (!a) continue;
            n->second.pending_ground = false;
            n->second.pending_ground_done = true;
            if (pending_count_ > 0) --pending_count_;
            const double z_before = a->z;
            ground_npc(n->second, *a);
            if (!n->second.relocated) n->second.spawn_home = n->second.home;
            // Snapshot/Dormant views only resend when a reliable state change occurs, so touch State when grounding shifts height significantly.
            if (std::fabs(a->z - z_before) > kRegroundReliableCm) registry_.touch(*a, world::Change::State);
        }
        pending_by_chunk_.erase(it);
    }
}

std::vector<std::pair<double, double>> NpcWorld::player_points(double now) const {
    std::vector<std::pair<double, double>> pts;
    for (const auto& [id, p] : players_)
        if (p.at >= now - cfg_.player_ttl) pts.emplace_back(p.x, p.y);
    return pts;
}

void NpcWorld::note_player(uint64_t player, double x, double y, double now) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    players_[player] = {x, y, now};
}

void NpcWorld::forget_player(uint64_t player) { players_.erase(player); }

bool NpcWorld::awake_for_someone(const world::WorldActor& a) const {
    const double r = (a.awake_radius_cm > 0 ? a.awake_radius_cm : cfg_.awake_cm) + cfg_.awake_margin_cm;
    for (const auto& [id, p] : players_) {
        if (p.at < now_ - cfg_.player_ttl) continue;
        if (std::hypot(p.x - a.x, p.y - a.y) <= r) return true;
    }
    return false;
}

void NpcWorld::refresh_active(double t) {
    std::vector<uint64_t> next;
    std::vector<uint64_t> near;
    for (const auto& [pid, p] : players_) {
        if (p.at < t - cfg_.player_ttl) continue;
        near.clear();
        registry_.query(p.x, p.y, cfg_.awake_cm + cfg_.awake_margin_cm + 6000.0, near);
        for (const uint64_t id : near) {
            const auto it = npcs_.find(id);
            if (it == npcs_.end() || (it->second.mode == Mode::Stand && !it->second.turning && !it->second.track) || it->second.pending_ground) continue;
            const world::WorldActor* a = registry_.find(id);
            if (a && awake_for_someone(*a) && nav_->ready_at(a->x, a->y)) next.push_back(id);
        }
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    for (const uint64_t id : active_) {
        if (std::binary_search(next.begin(), next.end(), id)) continue;
        Npc& n = npcs_.find(id)->second;
        n.active = false;
        if (n.reported_moving) commit(n, t, true);
    }
    for (const uint64_t id : next) npcs_.find(id)->second.active = true;
    active_ = std::move(next);
}

void NpcWorld::tick(double now) {
    if (!std::isfinite(now) || now < now_) return;
    now_ = now;
    if (next_step_ < 0) {
        next_step_ = now;
        // Initialize timestamps to the first tick so an NPC's first movement does not jump by the server's startup age.
        for (auto& [id, n] : npcs_) {
            world::WorldActor* a = registry_.find(id);
            if (a && a->actor.obj.schema && a->actor.obj.schema->command("ReplicatedServerLastTransformUpdateTimeStamp"))
                a->actor.obj.state.set(*a->actor.obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp", repl::Value::f32(static_cast<float>(now)));
        }
    }
    int steps = 0;
    while (next_step_ <= now && steps < cfg_.max_catch_up_steps) {
        step(next_step_);
        next_step_ += cfg_.step_seconds;
        ++steps;
    }
    if (next_step_ <= now) next_step_ = now + cfg_.step_seconds;
}

void NpcWorld::step(double t) {
    const auto step_start = std::chrono::steady_clock::now();
    pathfinds_this_step_ = 0;
    iterations_this_step_ = 0;
    ++stats_.steps;
    refresh_active(t);
    stats_.max_refresh_ms = std::max(stats_.max_refresh_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - step_start).count());
    // Rotate starting offset each step so one unpathable patrol cannot starve pathfinding for other active NPCs.
    const size_t count = active_.size();
    const size_t first = count ? static_cast<size_t>(stats_.steps % count) : 0;
    for (size_t i = 0; i < count; ++i) {
        Npc& n = npcs_.find(active_[(first + i) % count])->second;
        if (!n.rm) advance_npc(n, t);
    }
    const std::vector<uint64_t> carried = rooted_;
    for (const uint64_t id : carried) {
        const auto it = npcs_.find(id);
        if (it != npcs_.end() && it->second.rm) advance_root_motion(it->second, t);
    }
    const std::vector<uint64_t> settling = settling_;
    for (const uint64_t id : settling) {
        const auto it = npcs_.find(id);
        if (it != npcs_.end()) settle(it->second, t);
    }
    stats_.max_step_ms = std::max(stats_.max_step_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - step_start).count());
    stats_.max_active = std::max(stats_.max_active, count);
    stats_.max_pathfinds_in_step = std::max(stats_.max_pathfinds_in_step, pathfinds_this_step_);
}

void NpcWorld::go_idle(Npc& n) {
    n.mover.stop();
    n.ordered_return = false;
    switch (n.setup.ambient) {
        case Ambient::Wander: {
            n.mode = Mode::Idle;
            const double base = n.setup.wander_delay * cfg_.idle_per_delay, spread = n.setup.wander_delay_deviation * cfg_.idle_spread;
            n.timer = std::clamp(base + (next_unit(n) * 2.0 - 1.0) * spread, cfg_.min_idle, cfg_.max_idle);
            break;
        }
        case Ambient::Patrol:
            if (n.setup.route.size() >= 2) { n.mode = Mode::PatrolPause; n.timer = 0.2; break; }
            [[fallthrough]];
        default:
            n.mode = Mode::Stand;
            break;
    }
}

void NpcWorld::start_wander(Npc& n) {
    const NpcMotionSetup& s = n.setup;
    const double lo = std::max(0.0, s.wander_min), hi = std::max(lo + 1.0, s.wander_max);
    const double median = std::clamp(s.leg_median, 50.0, hi), p90 = std::max(s.leg_p90, median * 1.2);
    const double sigma = std::log(p90 / median) / 1.2815515655446004;
    ++stats_.wander_asks;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (pathfinds_this_step_ >= cfg_.max_pathfinds_per_step || iterations_this_step_ >= cfg_.max_search_iterations_per_step) { n.timer = cfg_.step_seconds; ++stats_.wander_deferred; return; }
        const double z = (next_unit(n) + next_unit(n) + next_unit(n) + next_unit(n) - 2.0) * 1.7320508075688772;
        const double leg = std::clamp(median * std::exp(sigma * z), std::min(lo, 100.0) + 50.0, hi);
        const nav::Vec3 here = n.mover.ground();
        double a = next_unit(n) * 6.283185307179586;
        nav::Vec3 want{here.x + std::cos(a) * leg, here.y + std::sin(a) * leg, here.z};
        if (std::hypot(want.x - n.home.x, want.y - n.home.y) > hi) {
            a = std::atan2(n.home.y - here.y, n.home.x - here.x) + (next_unit(n) - 0.5) * 1.5707963267948966;
            const double back = std::min(leg, std::max(dist2d(here, n.home), 150.0));
            want = {here.x + std::cos(a) * back, here.y + std::sin(a) * back, here.z};
        }
        nav::Vec3 np;
        if (!nav_->nearest_point(want, 150, 300, &np) || std::hypot(np.x - n.home.x, np.y - n.home.y) > hi + 100.0) { ++stats_.wander_no_point; continue; }
        ++pathfinds_this_step_;
        const double straight = dist2d(n.mover.ground(), np);
        if (straight < 50.0) continue;
        const bool walks = n.mover.move_to(np, s.walk_speed, 0, straight * cfg_.max_wander_path_factor + 500.0);
        iterations_this_step_ += n.mover.last_search_iterations();
        if (walks) {
            n.mode = Mode::Wander;
            n.fails = 0;
            ++stats_.moves_started;
            return;
        }
        ++stats_.no_path;
        ++stats_.wander_no_path;
        ++stats_.wander_fail[static_cast<size_t>(n.mover.last_fail())];
    }
    ++n.fails;
    n.mode = Mode::Idle;
    n.timer = std::min(30.0, 2.0 + 2.0 * n.fails);
}

void NpcWorld::start_patrol_leg(Npc& n) {
    const NpcMotionSetup& s = n.setup;
    const size_t count = s.route.size();
    ++stats_.patrol_asks;
    for (size_t tries = 0; tries < count; ++tries) {
        if (pathfinds_this_step_ >= cfg_.max_pathfinds_per_step || iterations_this_step_ >= cfg_.max_search_iterations_per_step) { n.timer = cfg_.step_seconds; n.mode = Mode::PatrolPause; ++stats_.patrol_deferred; return; }
        if (!n.first_leg) {
            if (s.loop) {
                n.wp = (n.wp + 1) % count;
            } else {
                if (n.wp_dir > 0 && n.wp + 1 >= count) n.wp_dir = -1;
                else if (n.wp_dir < 0 && n.wp == 0) n.wp_dir = 1;
                n.wp = static_cast<size_t>(static_cast<long long>(n.wp) + n.wp_dir);
            }
        }
        ++pathfinds_this_step_;
        const bool walks = n.mover.move_to(s.route[n.wp], s.patrol_speed, 0, 0);
        iterations_this_step_ += n.mover.last_search_iterations();
        if (walks) {
            n.first_leg = false;
            n.mode = Mode::Patrol;
            n.fails = 0;
            ++stats_.moves_started;
            return;
        }
        ++stats_.no_path;
        ++stats_.patrol_no_path;
        ++stats_.patrol_fail[static_cast<size_t>(n.mover.last_fail())];
        n.first_leg = false;
    }
    ++n.fails;
    n.mode = Mode::PatrolPause;
    n.timer = std::min(60.0, 5.0 * n.fails);
}

void NpcWorld::decide(Npc& n, double) {
    switch (n.mode) {
        case Mode::Idle: {
            const double far = std::max(2.0 * n.setup.wander_max, 1500.0);
            if (dist2d(n.mover.ground(), n.home) > far) {
                if (n.mover.move_to(n.home, n.setup.run_speed, 0, 0)) {
                    n.mode = Mode::Return;
                    n.ordered_return = false;
                    ++stats_.returns_home;
                    return;
                }
            }
            start_wander(n);
            break;
        }
        case Mode::PatrolPause:
            start_patrol_leg(n);
            break;
        default:
            break;
    }
}

void NpcWorld::commit(Npc& n, double t, bool force) {
    world::WorldActor* a = registry_.find(n.id);
    if (!a) return;
    const nav::Vec3 g = n.mover.ground();
    const double z = n.mover.origin_z(), yaw = n.mover.yaw_degrees();
    const bool moving = n.mover.moving() && n.active;
    const nav::Vec3 vel = moving ? n.mover.velocity() : nav::Vec3{};
    const bool moved = std::fabs(a->x - g.x) > 1e-3 || std::fabs(a->y - g.y) > 1e-3 || std::fabs(a->z - z) > 1e-3 || std::fabs(a->yaw_degrees - yaw) > 1e-3;
    if (!force && !moved && moving == n.reported_moving) return;
    registry_.move(n.id, g.x, g.y, z, yaw);
    write_replicated(*a, {g.x, g.y, z, yaw, vel, moving, t, 0});
    n.reported_moving = moving;
    ++stats_.writes;
}

void NpcWorld::steer(Npc& n, double t) {
    const double dt = cfg_.step_seconds;
    bool turned = false;
    if (n.track) {
        nav::Vec3 at;
        if (n.track_until > 0 && t >= n.track_until) {
            n.track = nullptr;
        } else if (n.track(at)) {
            const nav::Vec3 g = n.mover.ground();
            if (std::hypot(at.x - g.x, at.y - g.y) > 1.0) {
                n.mover.turn_toward(std::atan2(at.y - g.y, at.x - g.x) * 180.0 / 3.14159265358979323846, dt, n.track_rate);
                turned = true;
            }
        }
    } else if (n.turning && !n.mover.moving()) {
        if (n.mover.turn_toward(n.turn_yaw, dt, n.turn_rate)) n.turning = false;
        turned = true;
    }
    if (turned) commit(n, t, false);
}

void NpcWorld::pursue_step(Npc& n, double) {
    if (!n.pursue) return;
    nav::Vec3 at;
    if (n.mode != Mode::Ordered || !n.pursue(at)) {
        n.pursue = nullptr;
        return;
    }
    const nav::Vec3 g = n.mover.ground();
    if (std::hypot(at.x - g.x, at.y - g.y) <= n.pursue_stop) return;
    if (n.mover.moving() && std::hypot(at.x - n.pursue_plan_x, at.y - n.pursue_plan_y) < 200.0 && n.mover.set_end(at.x, at.y)) {
        n.pursue_x = at.x;
        n.pursue_y = at.y;
        return;
    }
    if (pathfinds_this_step_ >= cfg_.max_pathfinds_per_step || iterations_this_step_ >= cfg_.max_search_iterations_per_step) return;
    ++pathfinds_this_step_;
    const bool found = n.mover.move_to(at, n.pursue_speed, n.pursue_stop, 0);
    iterations_this_step_ += n.mover.last_search_iterations();
    n.pursue_x = n.pursue_plan_x = at.x;
    n.pursue_y = n.pursue_plan_y = at.y;
    if (!found) n.pursue = nullptr;
}

void NpcWorld::advance_npc(Npc& n, double t) {
    const double dt = cfg_.step_seconds;
    pursue_step(n, t);
    if (n.mode == Mode::Stand) {
        steer(n, t);
        return;
    }
    if (n.mode == Mode::Idle || n.mode == Mode::PatrolPause) {
        n.timer -= dt;
        if (n.timer <= 0) decide(n, t);
        if (n.mode == Mode::Idle || n.mode == Mode::PatrolPause) {
            if (n.reported_moving) commit(n, t, false);
            return;
        }
    }
    const Mover::Status st = n.mover.advance(dt);
    steer(n, t);
    commit(n, t, false);
    if (st == Mover::Status::Arrived) {
        if (n.mode == Mode::Ordered) {
            n.mover.stop();
            return;
        }
        if (n.mode == Mode::Patrol) {
            const size_t i = n.wp;
            const double pause = i < n.setup.pauses.size() ? std::max(0.0, n.setup.pauses[i]) : 0.0;
            n.mover.stop();
            n.mode = Mode::PatrolPause;
            n.timer = pause;
        } else {
            go_idle(n);
        }
    } else if (st == Mover::Status::Blocked || st == Mover::Status::Idle) {
        if (n.mode != Mode::Ordered) go_idle(n);
    }
}

}  // namespace aoc::worldd::npc
