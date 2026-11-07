// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "npc/npc_world.h"

#include "bootstrap/constants.h"
#include "protocol/repl/codecs.h"
#include "protocol/repl/move_packed.h"
#include "protocol/repl/value.h"

#include <algorithm>
#include <limits>

namespace aoc::worldd::npc {

namespace repl = ::aoc::protocol::repl;

namespace {

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double dist2d(const nav::Vec3& a, const nav::Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

size_t nearest_waypoint_index(const std::vector<nav::Vec3>& route, const nav::Vec3& home) {
    size_t best_idx = 0;
    double best = 1e30;
    for (size_t i = 0; i < route.size(); ++i) {
        if (const double d = dist2d(route[i], home); d < best) { best = d; best_idx = i; }
    }
    return best_idx;
}

bool finite_setup(const NpcMotionSetup& s) {
    for (double v : {s.home_yaw, s.foot_offset, s.yaw_rate, s.wander_min, s.wander_max, s.wander_delay, s.wander_delay_deviation, s.walk_speed, s.patrol_speed, s.run_speed, s.leg_median, s.leg_p90,
                     s.arrival_distance, s.awake_radius}) {
        if (!std::isfinite(v)) return false;
    }
    for (const nav::Vec3& p : s.route) if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    for (double p : s.pauses) if (!std::isfinite(p)) return false;
    return s.foot_offset >= 0 && s.foot_offset < 5000 && s.wander_min >= 0 && s.wander_max >= s.wander_min && s.walk_speed >= 0 && s.patrol_speed >= 0 && s.run_speed >= 0;
}

bool parse_env_simulate_all_npcs(bool* explicit_out = nullptr) noexcept {
    if (const char* s = std::getenv("AOC_WORLD_SIMULATE_ALL_NPCS"); s && *s) {
        const std::string_view v(s);
        const bool on = v != "0" && v != "false" && v != "FALSE" && v != "off" && v != "OFF";
        if (explicit_out) *explicit_out = on;
        return on;
    }
    const char* r = std::getenv("AOC_WORLD_REGION");
    const char* w = std::getenv("AOC_WORLD_WORKER_ID");
    const char* m = std::getenv("AOC_WORLD_STREAM_ADDR");
    if (explicit_out) *explicit_out = (r && *r) || (w && *w) || (m && *m);
    return true;
}

std::atomic<bool> g_global_simulate_all_npcs{parse_env_simulate_all_npcs()};
std::atomic<uint64_t> g_global_sim_epoch{0};

bool is_hostile_world_actor(const world::WorldActor& a) {
    if (a.kind != world::ActorKind::Npc || a.npc.is_gm_vendor || a.npc.vendor_guid != 0 || a.npc.dialogue_root != 0 || a.npc.aggro_off ||
        a.npc.ground_by_data || a.npc.spawn_ref >= 0x80000000u) {
        return false;
    }
    if (a.npc.aggro_radius_cm > 0.0 || !a.npc.loot_tables.empty()) return true;
    const std::string& p = a.npc.archetype;
    return p.find("/NPC/Monster/") != std::string::npos || p.find("/NPC/Beast/") != std::string::npos || p.find("/NPC/Humanoid/") != std::string::npos ||
           p.find("/Bandits") != std::string::npos || p.find("/Cultists") != std::string::npos;
}

}  // namespace

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

NpcWorld::NpcWorld(world::ActorRegistry& registry, const nav::AreaNav* nav, NpcWorldConfig config) : registry_(registry), nav_(nav), cfg_(config) {
    simulate_all_npcs_.store(parse_env_simulate_all_npcs(), std::memory_order_relaxed);
    seen_global_sim_epoch_ = g_global_sim_epoch.load(std::memory_order_relaxed);
    remove_listener_ = registry_.on_remove([this](const world::WorldActor& a) { remove(a.id); });
}

NpcWorld::~NpcWorld() { registry_.off_remove(remove_listener_); }

void NpcWorld::set_simulate_all_npcs(bool v) noexcept {
    simulate_all_npcs_.store(v, std::memory_order_relaxed);
    set_global_simulate_all_npcs(v);
    seen_global_sim_epoch_ = g_global_sim_epoch.load(std::memory_order_relaxed);
}

bool NpcWorld::global_simulate_all_npcs() noexcept {
    return g_global_simulate_all_npcs.load(std::memory_order_relaxed);
}

void NpcWorld::set_global_simulate_all_npcs(bool v) noexcept {
    g_global_simulate_all_npcs.store(v, std::memory_order_relaxed);
    g_global_sim_epoch.fetch_add(1, std::memory_order_relaxed);
}

bool NpcWorld::dispatch_gm_work_item(std::string_view kind, std::string_view payload, NpcWorld* world) noexcept {
    if (kind == "gm.SetTimeOfDay" || kind == "set_time_of_day" || kind == "time_of_day") {
        double hours = 12.0;
        for (size_t i = 0; i < payload.size(); ++i) {
            if ((payload[i] >= '0' && payload[i] <= '9') || (payload[i] == '-' && i + 1 < payload.size() && payload[i + 1] >= '0' && payload[i + 1] <= '9')) {
                hours = std::strtod(payload.data() + i, nullptr);
                break;
            }
        }
        bootstrap::set_global_time_of_day_hours(hours);
        return true;
    }
    if (kind != "gm.SetSimulateAllNpcs" && kind != "set_simulate_all_npcs" && kind != "simulate_all_npcs") return false;
    bool enabled = true;
    if (payload.find("false") != std::string_view::npos || payload.find("FALSE") != std::string_view::npos ||
        payload.find("\"value\":0") != std::string_view::npos || payload.find("\"value\": 0") != std::string_view::npos ||
        payload.find("\"enabled\":0") != std::string_view::npos || payload.find("enabled=0") != std::string_view::npos || payload == "0") {
        enabled = false;
    }
    if (world) world->set_simulate_all_npcs(enabled);
    else set_global_simulate_all_npcs(enabled);
    return true;
}

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
    n.hostile = is_hostile_world_actor(*a);
    const bool real_npc = !a->npc.archetype.empty() || a->npc.asset_guid != 0 || a->npc.instance_guid != 0;
    const bool non_stationary_role = !a->npc.is_gm_vendor && a->npc.vendor_guid == 0 && a->npc.dialogue_root == 0 && !a->npc.ground_by_data;
    n.can_ambient_roam = real_npc && (n.hostile || setup.ambient == Ambient::Wander || (setup.ambient == Ambient::Patrol && setup.route.size() >= 2) || non_stationary_role);
    n.mover.init(nav_, setup.foot_offset, setup.yaw_rate);
    if (!nav_->ready_at(a->x, a->y)) {
        // City node NPCs (ground_by_data) keep their data height because baked terrain under a city bowl is 50 m below its floor.
        double terrain = 0;
        const bool have_terrain = !a->npc.ground_by_data && nav_->terrain_height(a->x, a->y, &terrain);
        const double hint = have_terrain ? terrain : a->z - setup.foot_offset;
        n.mover.place(a->x, a->y, hint, setup.home_yaw);
        n.mode = Mode::Stand;
        n.home = {a->x, a->y, hint};
        n.wp = (setup.route.size() >= 2 && setup.start_closest) ? nearest_waypoint_index(setup.route, n.home) : 0;
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

void NpcWorld::enqueue_chunks_resident(const std::vector<nav::ChunkKey>& chunks) {
    for (const nav::ChunkKey& key : chunks) {
        const auto it = pending_by_chunk_.find(key);
        if (it == pending_by_chunk_.end()) continue;
        for (const uint64_t id : it->second) ready_ground_queue_.push_back(id);
        pending_by_chunk_.erase(it);
    }
}

size_t NpcWorld::pump_grounding(size_t max_npcs, double budget_ms) {
    if (ready_ground_queue_.empty() || max_npcs == 0) return 0;
    const auto start = std::chrono::steady_clock::now();
    size_t grounded = 0;
    while (!ready_ground_queue_.empty() && grounded < max_npcs) {
        if (grounded > 0 && budget_ms > 0.0) {
            const double elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (elapsed_ms >= budget_ms) break;
        }
        const uint64_t id = ready_ground_queue_.front();
        ready_ground_queue_.pop_front();
        const auto n = npcs_.find(id);
        if (n == npcs_.end() || !n->second.pending_ground) continue;
        world::WorldActor* a = registry_.find(id);
        if (!a) {
            n->second.pending_ground = false;
            if (pending_count_ > 0) --pending_count_;
            continue;
        }
        if (max_npcs != std::numeric_limits<size_t>::max() && !nav_->ready_at(a->x, a->y)) {
            pending_by_chunk_[nav_->chunk_at(a->x, a->y)].push_back(id);
            continue;
        }
        n->second.pending_ground = false;
        n->second.pending_ground_done = true;
        if (pending_count_ > 0) --pending_count_;
        const double z_before = a->z;
        ground_npc(n->second, *a);
        if (!n->second.relocated) n->second.spawn_home = n->second.home;
        // Snapshot/Dormant views only resend when a reliable state change occurs, so touch State when grounding shifts height significantly.
        if (std::fabs(a->z - z_before) > kRegroundReliableCm) registry_.touch(*a, world::Change::State);
        ++grounded;
    }
    return grounded;
}

void NpcWorld::chunks_resident(const std::vector<nav::ChunkKey>& chunks) {
    enqueue_chunks_resident(chunks);
    pump_grounding(std::numeric_limits<size_t>::max(), 1e9);
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
    const uint64_t g_ep = g_global_sim_epoch.load(std::memory_order_relaxed);
    if (seen_global_sim_epoch_ != g_ep) {
        seen_global_sim_epoch_ = g_ep;
        simulate_all_npcs_.store(g_global_simulate_all_npcs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
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
    if (simulate_all_npcs_.load(std::memory_order_relaxed)) {
        step_all_ambient(t);
    }
    stats_.max_step_ms = std::max(stats_.max_step_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - step_start).count());
    stats_.max_active = std::max(stats_.max_active, count);
    stats_.max_pathfinds_in_step = std::max(stats_.max_pathfinds_in_step, pathfinds_this_step_);
}

void NpcWorld::step_roam(Npc& n, double t) {
    world::WorldActor* a = registry_.find(n.id);
    if (!a) return;
    if (n.pending_ground && n.can_ambient_roam) {
        n.pending_ground = false;
        if (pending_count_ > 0) --pending_count_;
    }
    if (n.setup.ambient == Ambient::Patrol && n.setup.route.size() >= 2) {
        advance_ambient_patrol(n, *a, t, cfg_.step_seconds);
    } else {
        advance_ambient_wander(n, *a, t, cfg_.step_seconds);
    }
}

void NpcWorld::step_all_ambient(double t) {
    bool explicit_env = false;
    (void)parse_env_simulate_all_npcs(&explicit_env);
    const bool allow_when_players = explicit_env || seen_global_sim_epoch_ > 0;
    if (!players_.empty() && !allow_when_players) return;
    const size_t total = npcs_.size();
    if (total == 0) return;
    const uint32_t eff_hz = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(1.0 / std::max(1e-3, cfg_.step_seconds))), 10, 60));
    const size_t min_for_1s_backlog = (total + eff_hz - 1) / eff_hz;
    const size_t max_slice = (cfg_.sim_step_cap == 0) ? total : std::min(total, std::max<size_t>(cfg_.sim_step_cap, min_for_1s_backlog));
    const double budget_ms = std::max(0.5, 0.375 * cfg_.step_seconds * 1000.0);
    const auto slice_start = std::chrono::steady_clock::now();
    auto it = npcs_.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(ambient_cursor_ % total));
    size_t stepped = 0;
    for (size_t visited = 0; visited < total && stepped < max_slice; ++visited) {
        if (it == npcs_.end()) it = npcs_.begin();
        Npc& n = it->second;
        ++it;
        if (n.active || n.rm || (!n.can_ambient_roam && !allow_when_players)) continue;
        if (n.mode == Mode::Ordered || n.pursue || n.track || n.lock_walk || n.lock_turn) continue;
        if (n.setup.ambient == Ambient::Stationary && !n.hostile && n.setup.route.size() < 2) continue;
        step_roam(n, t);
        ++stepped;
        if (stepped >= min_for_1s_backlog && (stepped & 63u) == 0 &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - slice_start).count() >= budget_ms) break;
    }
    ambient_cursor_ = (ambient_cursor_ + std::max<size_t>(1, stepped)) % total;
}

void NpcWorld::advance_ambient_patrol(Npc& n, world::WorldActor& a, double t, double dt) {
    if (t < n.wait_until) return;
    const size_t count = n.setup.route.size();
    if (count < 2) return;
    if (n.wp >= count) n.wp = 0;
    const nav::Vec3& wp = n.setup.route[n.wp];
    const double dx = wp.x - a.x, dy = wp.y - a.y, dist = std::hypot(dx, dy);
    const double step_cm = std::max(n.setup.patrol_speed, 360.0) * dt;
    if (dist <= 80.0 || dist <= step_cm) {
        const double pause = n.wp < n.setup.pauses.size() ? std::max(0.0, n.setup.pauses[n.wp]) : 0.2;
        if (n.setup.loop) n.wp = (n.wp + 1) % count;
        else {
            if (n.wp_dir > 0 && n.wp + 1 >= count) n.wp_dir = -1;
            else if (n.wp_dir < 0 && n.wp == 0) n.wp_dir = 1;
            n.wp = static_cast<size_t>(static_cast<long long>(n.wp) + n.wp_dir);
        }
        n.wait_until = t + pause;
        n.mover.place(wp.x, wp.y, wp.z, a.yaw_degrees);
        registry_.move(n.id, wp.x, wp.y, n.mover.origin_z(), n.mover.yaw_degrees());
        ++stats_.writes;
        return;
    }
    const double nx = a.x + (dx / dist) * step_cm, ny = a.y + (dy / dist) * step_cm;
    const double nyaw = std::atan2(dy, dx) * (180.0 / 3.14159265358979323846);
    double feet_z = wp.z;
    if (!a.npc.ground_by_data && nav_) (void)nav_->terrain_height(nx, ny, &feet_z);
    n.mover.place(nx, ny, feet_z, nyaw);
    registry_.move(n.id, nx, ny, n.mover.origin_z(), nyaw);
    ++stats_.writes;
}

void NpcWorld::advance_ambient_wander(Npc& n, world::WorldActor& a, double t, double dt) {
    const double roam_r = n.hostile ? std::max<double>(n.setup.wander_max, 8500.0) : std::max<double>(n.setup.wander_max, 2800.0);
    const double speed = std::max<double>(n.setup.walk_speed, n.hostile ? 460.0 : 320.0);
    const bool was_goal = n.has_goal;
    const double dist_goal = was_goal ? std::hypot(n.goal_x - a.x, n.goal_y - a.y) : 0.0;
    if (!was_goal || dist_goal < 120.0) {
        if (was_goal && t < n.wait_until) return;
        const double ang = next_unit(n) * 6.283185307179586, rad = (0.25 + 0.75 * std::sqrt(next_unit(n))) * roam_r;
        n.goal_x = n.home.x + std::cos(ang) * rad; n.goal_y = n.home.y + std::sin(ang) * rad; n.goal_z = n.home.z;
        n.has_goal = true; n.wait_until = was_goal ? (t + 0.2 + next_unit(n) * 1.0) : t; ++stats_.moves_started;
    }
    if (t < n.wait_until) return;
    const double dx = n.goal_x - a.x, dy = n.goal_y - a.y, d = std::hypot(dx, dy);
    if (d < 1e-3) { n.has_goal = false; return; }
    const double step_cm = std::min(d, speed * dt);
    const double nx = a.x + (dx / d) * step_cm, ny = a.y + (dy / d) * step_cm;
    const double nyaw = std::atan2(dy, dx) * (180.0 / 3.14159265358979323846);
    double feet_z = n.home.z;
    if (!a.npc.ground_by_data && nav_) (void)nav_->terrain_height(nx, ny, &feet_z);
    n.mover.place(nx, ny, feet_z, nyaw);
    registry_.move(n.id, nx, ny, n.mover.origin_z(), nyaw);
    ++stats_.writes;
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
        default: n.mode = Mode::Stand; break;
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
    const bool moving = n.mover.moving() && n.active && !n.lock_walk;
    const nav::Vec3 vel = moving ? n.mover.velocity() : nav::Vec3{};
    const bool moved = std::fabs(a->x - g.x) > 1e-3 || std::fabs(a->y - g.y) > 1e-3 || std::fabs(a->z - z) > 1e-3 || std::fabs(a->yaw_degrees - yaw) > 1e-3;
    if (!force && !moved && moving == n.reported_moving) return;
    registry_.move(n.id, g.x, g.y, z, yaw);
    write_replicated(*a, {g.x, g.y, z, yaw, vel, moving, t, 0});
    n.reported_moving = moving;
    ++stats_.writes;
}

bool NpcWorld::track_running(const Npc& n, double t) { return n.track && !n.lock_turn && !(n.track_until > 0 && t >= n.track_until); }

bool NpcWorld::set_locks(uint64_t id, bool no_walk, bool no_turn) {
    const auto it = npcs_.find(id);
    if (it == npcs_.end()) return false;
    if (no_turn && !it->second.lock_turn) { it->second.turning = false; it->second.track = nullptr; }
    it->second.lock_walk = no_walk; it->second.lock_turn = no_turn;
    return true;
}

void NpcWorld::steer(Npc& n, double t) {
    if (n.lock_turn) return;
    const double dt = cfg_.step_seconds;
    bool turned = false;
    if (n.track && !track_running(n, t)) n.track = nullptr;
    if (n.track) {
        if (nav::Vec3 at; n.track(at)) {
            const nav::Vec3 g = n.mover.ground();
            if (std::hypot(at.x - g.x, at.y - g.y) > 1.0) {
                n.mover.turn_toward(std::atan2(at.y - g.y, at.x - g.x) * 180.0 / 3.14159265358979323846, dt, n.track_rate);
                turned = true;
            }
        } else n.track = nullptr;
    } else if (n.turning && !(n.mover.moving() && !n.lock_walk)) {
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
    if (n.lock_walk) { steer(n, t); commit(n, t, false); return; }
    pursue_step(n, t);
    if (n.mode == Mode::Stand) { steer(n, t); return; }
    if (n.mode == Mode::Idle || n.mode == Mode::PatrolPause) {
        if ((n.timer -= dt) <= 0) decide(n, t);
        if (n.mode == Mode::Idle || n.mode == Mode::PatrolPause) {
            if (n.reported_moving) commit(n, t, false);
            return;
        }
    }
    bool tracked = track_running(n, t);
    if (nav::Vec3 target_at; tracked && !n.track(target_at)) { n.track = nullptr; tracked = false; }
    const Mover::Status st = n.mover.advance(dt, !tracked);
    steer(n, t);
    commit(n, t, false);
    if (st == Mover::Status::Arrived) {
        if (n.mode == Mode::Ordered) { n.mover.stop(); return; }
        if (n.mode == Mode::Patrol) {
            const size_t i = n.wp;
            n.mover.stop();
            n.mode = Mode::PatrolPause;
            n.timer = i < n.setup.pauses.size() ? std::max(0.0, n.setup.pauses[i]) : 0.0;
        } else go_idle(n);
    } else if ((st == Mover::Status::Blocked || st == Mover::Status::Idle) && n.mode != Mode::Ordered) {
        go_idle(n);
    }
}

}  // namespace aoc::worldd::npc
