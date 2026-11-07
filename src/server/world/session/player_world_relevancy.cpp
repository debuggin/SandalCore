// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "session/player_world.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc { namespace worldd {

namespace {

void sample_area_points(const nav::AreaNav& nav, const spawns::AreaConfig& area, size_t& sampled, size_t& outside) {
    const auto probe = [&](double x, double y) {
        ++sampled;
        if (!nav.in_region(x, y)) ++outside;
    };
    if (area.region) {
        for (size_t i = 0; i < spawns::region::kRiverlandsRectCount; ++i) {
            const spawns::region::Rect& r = spawns::region::kRiverlandsRects[i];
            probe((r.x0 + r.x1) * 0.5, (r.y0 + r.y1) * 0.5);
            probe(r.x0 + 1.0, r.y0 + 1.0);
            probe(r.x1 - 1.0, r.y1 - 1.0);
        }
        return;
    }
    for (double dy = -area.radius_cm; dy <= area.radius_cm; dy += 10000.0) {
        for (double dx = -area.radius_cm; dx <= area.radius_cm; dx += 10000.0) {
            if (std::hypot(dx, dy) <= area.radius_cm) probe(area.center_x + dx, area.center_y + dy);
        }
    }
}

std::vector<std::pair<double, double>> sample_small_area_window(const nav::AreaNav& nav, const spawns::AreaConfig& area, int& radius) {
    const double r = area.radius_cm;
    if (area.region || r <= 0 || r > 200000.0) return {};
    std::map<std::pair<int, int>, uint32_t> tiles_of;
    for (const nav::NavChunkEntry& c : nav.file().chunks) tiles_of[{c.cx, c.cy}] = c.tiles;
    std::set<std::pair<int, int>> keys;
    std::vector<std::pair<double, double>> points;
    size_t tiles = 0;
    for (double dy = -r; dy <= r + 1; dy += 8000.0) {
        for (double dx = -r; dx <= r + 1; dx += 8000.0) {
            if (std::hypot(dx, dy) > r + 6000.0) continue;
            const nav::ChunkKey k = nav.chunk_at(area.center_x + dx, area.center_y + dy);
            if (!keys.insert({k.x, k.y}).second) continue;
            const auto it = tiles_of.find({k.x, k.y});
            if (it != tiles_of.end()) tiles += it->second;
            points.emplace_back(area.center_x + dx, area.center_y + dy);
        }
    }
    if (tiles * 5 <= static_cast<size_t>(nav.file().settings.max_tiles) * 4) {
        radius = 0;
        return points;
    }
    return {};
}

bool valid_floor_range(double x, double y, double z_lo, double z_hi) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z_lo) && std::isfinite(z_hi) && z_hi >= z_lo;
}

std::optional<double> near_wall_floor(const nav::AreaNav& nav, double x, double y, double z_lo, double z_hi) {
    nav::Vec3 stand;
    if (nav.nearest_point({x, y, 0.5 * (z_lo + z_hi)}, PlayerWorld::kNearWallCm, 0.5 * (z_hi - z_lo), &stand) &&
        std::hypot(stand.x - x, stand.y - y) <= PlayerWorld::kNearWallCm && stand.z >= z_lo && stand.z <= z_hi) {
        return stand.z;
    }
    return std::nullopt;
}

bool finite_xyz(double x, double y, double z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

std::string node_display_name(const world::CityNodes* nodes, int64_t node) {
    const world::CityNodeInfo* n = nodes ? nodes->by_guid(node) : nullptr;
    return n ? n->display_name : std::to_string(node);
}

std::optional<int> baked_pit_depth_m(const world::TownPlatforms* platforms, const nav::AreaNav* nav, double x, double y, double z) {
    double baked = 0;
    const auto platform = platforms ? platforms->floor_at(x, y) : std::nullopt;
    if (!platform || !nav || !nav->terrain_height(x, y, &baked) || !std::isfinite(baked)) return std::nullopt;
    if (platform->z - baked <= PlayerWorld::kPlatformUnderCm) return std::nullopt;
    if (std::fabs(z - PlayerWorld::kCapsuleHalfHeightCm - baked) >= PlayerWorld::kPitStandCm) return std::nullopt;
    return static_cast<int>((platform->z - baked) / 100.0);
}

double nav_pump_budget_ms() {
    static const double v = []() -> double {
        const char* s = std::getenv("AOC_WORLD_NAV_PUMP_BUDGET_MS");
        if (!s || !*s) return 4.0;
        char* end = nullptr;
        const double d = std::strtod(s, &end);
        return (end != s && std::isfinite(d)) ? std::clamp(d, 0.5, 50.0) : 4.0;
    }();
    return v;
}

size_t npc_ground_per_tick() {
    static const size_t v = []() -> size_t {
        const char* s = std::getenv("AOC_WORLD_NPC_GROUND_PER_TICK");
        if (!s || !*s) return 32;
        char* end = nullptr;
        const long n = std::strtol(s, &end, 10);
        return end != s ? static_cast<size_t>(std::clamp<long>(n, 1, 4096)) : 32;
    }();
    return v;
}

double npc_ground_budget_ms() {
    static const double v = []() -> double {
        const char* s = std::getenv("AOC_WORLD_NPC_GROUND_BUDGET_MS");
        if (!s || !*s) return 2.0;
        char* end = nullptr;
        const double d = std::strtod(s, &end);
        return (end != s && std::isfinite(d)) ? std::clamp(d, 0.25, 50.0) : 2.0;
    }();
    return v;
}

}  // namespace

bool PlayerWorld::load_ground(const std::string& nav_path, const std::string& motion_path, std::string& error) {
    if (!area_report_) {
        error = "the area is not loaded";
        return false;
    }
    if (npcs_) {
        error = "the ground is loaded already";
        return false;
    }
    std::string e;
    auto nav = nav::AreaNav::load(nav_path, &e);
    if (!nav) {
        error = e;
        return false;
    }
    if (!motion_loaded_) {
        std::string me;
        if (!motion_.load(motion_path, me)) {
            spdlog::warn("[nav] no NPC motion table ({}): every NPC stands one capsule half height of 88 cm above its ground and walks at the NPCPawn defaults (run tools/mapc: mapc build)", me);
        } else {
            motion_loaded_ = true;
            spdlog::info("[nav] NPC motion table {}: {} archetypes", motion_path, motion_.size());
        }
    }
    nav_ = std::move(nav);
    preload_window();
    spdlog::info("[nav] ground {}: {} terrain grids, {} navmesh tiles, {} polygons; centre ({}, {}), radius {} m", nav_path, nav_->terrain_grids(), nav_->tile_count(), nav_->polygon_count(),
                 static_cast<long long>(nav_->center_x()), static_cast<long long>(nav_->center_y()), nav_->radius() / 100.0);
    check_ground_covers_area();
    npcs_ = std::make_unique<npc::NpcWorld>(registry_, nav_.get());
    ground_report_ = npc::populate(registry_, *npcs_, motion_, *area_report_, speeds());
    spdlog::info("[nav] {}", ground_report_->describe());
    for (const std::string& line : ground_report_->describe_types()) spdlog::info("[nav] by type (standing/wandering/patrolling): {}", line);
    start_combat();
    if (gm_garden_) place_gm_garden();
    return true;
}

void PlayerWorld::check_ground_covers_area() const {
    size_t sampled = 0;
    size_t outside = 0;
    sample_area_points(*nav_, area_, sampled, outside);
    if (area_.region && !nav_->has_region()) {
        spdlog::error("[nav] the ground file is a FLAT file (E1's 1 km circle), but the area is the Riverlands: most NPCs have no ground and stand where the data puts them; copy the Riverlands' "
                      "area_nav.bin, area_geom.bin, roads.tsv and roads_raw.tsv into dist/content (E1's flat ones belong in dist/content/e1/), or run world with --area-radius 1000 for E1's circle");
    }
    if (outside == 0) {
        spdlog::info("[nav] the ground's footprint holds the whole area ({} points sampled)", sampled);
    } else {
        spdlog::warn("[nav] the ground's footprint {} does not hold {} of {} sampled points of the area ({}): NPCs there have terrain at most and stand where the data puts them "
                     "(rebuild the ground for this area: tools/mapc build{})",
                     nav_->has_region() ? "(the chunked file's rectangles)" : "(a flat file's circle)", outside, sampled, area_.region ? "the Riverlands" : "a circle around the login spot",
                     area_.region ? "" : " --center/--radius");
    }
}

bool PlayerWorld::adopt_ground(std::unique_ptr<nav::AreaNav> nav, std::string& error) {
    if (npcs_ || !nav) {
        error = npcs_ ? "the ground is loaded already" : "no ground";
        return false;
    }
    nav_ = std::move(nav);
    preload_window();
    npcs_ = std::make_unique<npc::NpcWorld>(registry_, nav_.get());
    start_combat();
    return true;
}

void PlayerWorld::preload_window() {
    if (!nav_ || !nav_->windowed()) return;
    const auto t0 = std::chrono::steady_clock::now();
    int radius = nav::AreaNav::kLoadRadius;
    std::vector<std::pair<double, double>> points = sample_small_area_window(*nav_, area_, radius);
    if (points.empty()) points.emplace_back(area_.center_x, area_.center_y);
    const bool ok = nav_->load_around(points, radius);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    spdlog::info("[nav] chunked ground: {} chunks ({} tiles) on disk, the terrain ({} grids) resident; the window keeps the chunks within {} of each player and drops one that is more than {} away (at most {} tiles in the mesh); "
                 "{} tiles in {} chunks are in after {:.0f} ms{}",
                 nav_->chunk_count(), nav_->total_tiles(), nav_->terrain_grids(), nav::AreaNav::kLoadRadius, nav::AreaNav::kKeepRadius,
                 nav_->file().settings.max_tiles, nav_->tile_count(), nav_->window_stats().resident_chunks, ms, ok ? "" : " (NOT all of them: the ground there is incomplete)");
}

void PlayerWorld::log_window_errors_and_repairs(const nav::WindowStats& w) {
    for (const std::string& e : nav_->take_load_errors()) spdlog::warn("[nav] ground: {}", e);
    if (w.load_failures > window_failures_logged_) {
        spdlog::warn("[nav] {} chunk(s) of the ground could not be loaded or added (damaged or missing file, or no room in the mesh): the NPCs there stay on the terrain",
                     w.load_failures - window_failures_logged_);
        window_failures_logged_ = w.load_failures;
    }
    if (w.tile_repair.polys_repaired > window_repairs_logged_.polys_repaired || w.tile_repair.polys_hidden > window_repairs_logged_.polys_hidden) {
        spdlog::warn("[nav] ground: {} polygon(s) of the tiles that came in had no detail edge marked on their border (Recast's float test loses it far from the origin; Detour's closest-point query would "
                     "dereference null) and have one now; {} cannot answer a closest-point query and are hidden from queries; {} edges marked in all so far",
                     w.tile_repair.polys_repaired - window_repairs_logged_.polys_repaired, w.tile_repair.polys_hidden - window_repairs_logged_.polys_hidden, w.tile_repair.edges_flagged);
        window_repairs_logged_ = w.tile_repair;
    }
}

void PlayerWorld::drive_window(double now_seconds) {
    const auto a = std::chrono::steady_clock::now();
    nav_->set_interest(npcs_->player_points(now_seconds));
    const auto b = std::chrono::steady_clock::now();
    const std::vector<nav::ChunkKey> arrived = nav_->pump(nav_pump_budget_ms());
    const auto c = std::chrono::steady_clock::now();
    if (!arrived.empty()) {
        npcs_->enqueue_chunks_resident(arrived);
        window_cost_.grounded_chunks += arrived.size();
    }
    if (npcs_->has_ready_grounding()) {
        const auto g0 = std::chrono::steady_clock::now();
        npcs_->pump_grounding(npc_ground_per_tick(), npc_ground_budget_ms());
        window_cost_.max_ground_ms = std::max(window_cost_.max_ground_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count());
    }
    window_cost_.max_interest_ms = std::max(window_cost_.max_interest_ms, std::chrono::duration<double, std::milli>(b - a).count());
    window_cost_.max_pump_ms = std::max(window_cost_.max_pump_ms, std::chrono::duration<double, std::milli>(c - b).count());
    const nav::WindowStats w = nav_->window_stats();
    log_window_errors_and_repairs(w);
    if (now_seconds - last_window_log_ >= kWindowLogSeconds) {
        last_window_log_ = now_seconds;
        log_window(now_seconds, w);
    }
}

void PlayerWorld::log_window(double now_seconds, const nav::WindowStats& w) {
    std::string where;
    size_t players = 0;
    for (const auto& [px, py] : npcs_->player_points(now_seconds)) {
        const nav::ChunkKey k = nav_->chunk_at(px, py);
        const bool here = nav_->ready_at(px, py);
        ++players;
        if (players <= 8) where += (where.empty() ? "" : ", ") + std::string("(") + std::to_string(k.x) + "," + std::to_string(k.y) + (here ? " ready)" : " LOADING)");
    }
    const npc::NpcWorld::Stats ns = npcs_->stats();
    spdlog::info("[nav] window: {} chunks ({} of {} tiles) resident, {} committed, {} players{}{}; NPCs waiting for their chunk {} of {}, {} awake, {} walking; chunks loaded {} / dropped {} / failed {} / retried {}, "
                 "{} deferred for room, {} dropped for a nearer one; planned {} times, {} skipped",
                 w.resident_chunks, w.resident_tiles, nav_->file().settings.max_tiles, w.committed_tiles, players, players ? " in chunks " : "", where, npcs_->pending_ground(), ns.npcs, ns.active_now,
                 ns.moving_now, w.chunk_loads, w.chunk_unloads, w.load_failures, w.retries, w.deferred_by_cap, w.evicted_for_room, w.plans, w.plans_skipped);
}

void PlayerWorld::check_town_node(int64_t node, const spawns::NodePopulations::Entry& entry, double now_seconds) {
    if (entry.npcs == 0) return;
    std::vector<uint64_t> people = node_pops_->actors_of(node);
    people.erase(std::remove_if(people.begin(), people.end(), [&](uint64_t id) {
        const world::WorldActor* a = registry_.find(id);
        return a && a->kind == world::ActorKind::Station;
    }), people.end());
    const npc::NpcWorld::StandReport r = npcs_->stand_report(people);
    const size_t ready = r.people - r.waiting;
    TownCheck& c = town_checks_[node];
    if (c.level != entry.level || c.people != r.people || c.ready != ready) {
        c.level = entry.level;
        c.people = r.people;
        c.ready = ready;
        c.changed_at = now_seconds;
        c.logged = false;
    }
    if (c.logged || ready == 0 || now_seconds - c.changed_at < kTownSettleSeconds || (c.level == c.logged_level && now_seconds - c.logged_at < kTownRelogSeconds)) return;
    c.logged = true;
    c.logged_at = now_seconds;
    c.logged_level = c.level;
    const world::CityNodeInfo* info = city_nodes_ ? city_nodes_->by_guid(node) : nullptr;
    spdlog::info("[towns] {} ({}): {} people, {} on a floor within {:.0f} cm of where the data puts them, {} on a floor up to {:.0f} cm from it, {} with no floor in reach ({} of them with no walkable "
                 "ground within {:.0f} cm either: in a wall, a stall, under a floor or in the air; {} of the {} would walk if they had ground), {} waiting for their ground to load",
                 info ? info->display_name : std::to_string(node), spawns::node_level_name(entry.level), r.people, r.on_floor, npc::NpcWorld::kStandNearCm, r.moved, r.max_moved_cm, r.no_floor,
                 r.blocked, npc::NpcWorld::kBlockedCm, r.stuck, r.no_floor, r.waiting);
}

void PlayerWorld::check_towns(double now_seconds) {
    if (!node_pops_ || !npcs_ || !nav_ || npcs_->has_ready_grounding() || now_seconds - last_town_check_ < kTownCheckSeconds) return;
    last_town_check_ = now_seconds;
    for (const auto& [node, entry] : node_pops_->entries()) {
        check_town_node(node, entry, now_seconds);
    }
}

PlayerWorld::RespawnSpot PlayerWorld::respawn_spot(double from_x, double from_y, bool nearest_point) const {
    RespawnSpot s;
    s.x = area_.center_x;
    s.y = area_.center_y;
    s.z = spawns::kLoginSpotZ;
    s.yaw = spawns::kLoginSpotYaw;
    s.name = "the login spot";
    double best = 1e300;
    if (area_report_ && nearest_point) {
        for (const spawns::RespawnPoint& p : area_report_->respawn_points) {
            if (!p.attuneable) continue;
            const double d = std::hypot(p.x - from_x, p.y - from_y);
            if (d < best) {
                best = d;
                s.x = p.x;
                s.y = p.y;
                s.z = p.z;
                s.yaw = p.yaw;
                s.shrine = true;
                s.name = p.name;
            }
        }
    }
    if (s.shrine) s.z = spawn_height(s.x, s.y, s.z);
    return s;
}

double PlayerWorld::spawn_height(double x, double y, double data_z, bool* grounded) const {
    if (const auto bowl = bowl_floor_at(x, y)) {
        if (grounded) *grounded = true;
        return bowl->z + kCapsuleHalfHeightCm + kBowlSlackCm;
    }
    return spot_height(nav_.get(), x, y, data_z, grounded);
}

double PlayerWorld::spot_height(const nav::AreaNav* nav, double x, double y, double data_z, bool* grounded) {
    if (grounded) *grounded = false;
    if (!nav || !nav->ready_at(x, y)) return data_z;
    double ground = 0;
    if (!nav->ground_height(x, y, data_z, &ground)) return data_z;
    if (grounded) *grounded = true;
    return ground + 98.0;
}

world::Places PlayerWorld::places() const {
    world::Places out = world::Places::build(city_nodes_, area_.center_x, area_.center_y, spawns::kLoginSpotZ);
    if (garden_report_ && !garden_report_->nodes.empty()) {
        world::Place garden;
        garden.name = garden.display_name = garden.kind = "garden";
        for (const spawns::GardenNode& n : garden_report_->nodes) {
            garden.x += n.x;
            garden.y += n.y;
            garden.z += n.z;
        }
        const double count = static_cast<double>(garden_report_->nodes.size());
        garden.x /= count;
        garden.y /= count;
        garden.z /= count;
        out.add(std::move(garden));
    }
    return out;
}

std::optional<double> PlayerWorld::ground_at(double x, double y) const {
    if (const auto floor = bowl_floor_at(x, y)) return floor->z + kBowlSlackCm;
    double z = 0;
    if (nav_ && nav_->terrain_height(x, y, &z) && std::isfinite(z)) {
        if (const auto town = town_ground_at(x, y)) return town->z + kBowlSlackCm;
        return z;
    }
    return std::nullopt;
}

void PlayerWorld::refresh_town_floor() const {
    if (!node_pops_ || !nav_) return;
    const uint64_t stamp = (node_pops_->made() << 32) ^ node_pops_->removed();
    if (stamp == town_floor_stamp_) return;
    town_floor_stamp_ = stamp;
    for (const auto& entry : node_pops_->entries()) {
        if (platforms_ && platforms_->has_node(entry.first)) {
            town_floor_.set(entry.first, {});
            continue;
        }
        std::vector<world::TownFloor::Person> people;
        for (const uint64_t id : node_pops_->actors_of(entry.first)) {
            const world::WorldActor* a = registry_.find(id);
            if (!a || a->kind != world::ActorKind::Npc) continue;
            world::TownFloor::Person p;
            p.x = a->npc.home_x;
            p.y = a->npc.home_y;
            p.z = a->npc.home_z;
            if (!nav_->terrain_height(p.x, p.y, &p.ground)) continue;
            people.push_back(p);
        }
        town_floor_.set(entry.first, people);
    }
}

world::TownFloor& PlayerWorld::town_floor() const {
    refresh_town_floor();
    return town_floor_;
}

std::optional<PlayerWorld::TownGround> PlayerWorld::bowl_floor_at(double x, double y) const {
    if (!platforms_) return std::nullopt;
    const auto hit = platforms_->floor_at(x, y);
    if (!hit) return std::nullopt;
    // Where a natural hill pokes through the town footprint (e.g. Joeva), the higher surface is the ground.
    double z = hit->z;
    double baked = 0;
    if (nav_ && nav_->terrain_height(x, y, &baked) && std::isfinite(baked) && baked > z) z = baked;
    return TownGround{hit->node, z, true};
}

std::optional<PlayerWorld::TownGround> PlayerWorld::town_ground_at(double x, double y) const {
    if (const auto floor = bowl_floor_at(x, y)) return floor;
    double ground = 0;
    if (!nav_ || !nav_->terrain_height(x, y, &ground) || !std::isfinite(ground)) return std::nullopt;
    const auto floor = town_floor().at(x, y, ground);
    if (!floor) return std::nullopt;
    return TownGround{floor->node, ground + floor->lift, false};
}

std::vector<double> PlayerWorld::floors_at(double x, double y, double z_lo, double z_hi) const {
    std::vector<double> out;
    if (!valid_floor_range(x, y, z_lo, z_hi)) return out;
    if (nav_ && nav_->ready_at(x, y)) {
        out = nav_->floors_at(x, y, z_lo, z_hi);
        if (out.empty()) {
            if (const auto z = near_wall_floor(*nav_, x, y, z_lo, z_hi)) out.push_back(*z);
        }
    }
    if (const auto bowl = bowl_floor_at(x, y); bowl && bowl->z >= z_lo && bowl->z <= z_hi) out.push_back(bowl->z);
    std::sort(out.begin(), out.end(), std::greater<double>());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return a - b < 25.0; }), out.end());
    return out;
}

std::optional<double> PlayerWorld::floor_under(double x, double y, double z, double below, double above) const {
    if (!std::isfinite(z) || !(below >= 0) || !(above >= 0)) return std::nullopt;
    const std::vector<double> floors = floors_at(x, y, z - below, z + above);
    if (floors.empty()) return std::nullopt;
    return floors.front();
}

std::optional<PlayerWorld::TownGround> PlayerWorld::town_ground_at(double x, double y, double z) const {
    const auto street = town_ground_at(x, y);
    if (!street || !street->platform || !std::isfinite(z)) return street;
    const auto floor = floor_under(x, y, z, 60000.0, kFloorReachCm);
    if (floor && *floor > street->z + 25.0) return TownGround{street->node, *floor, true, true};
    return street;
}

std::optional<PlayerWorld::BowlLift> PlayerWorld::lift_out_of_bowl(double x, double y, double z) const {
    const auto town = town_ground_at(x, y);
    if (!town || !std::isfinite(z)) return std::nullopt;
    const double stands = town->z + kCapsuleHalfHeightCm;
    const double under = town->platform ? kPlatformUnderCm : world::TownFloor::kBowlCm;
    if (stands - z < under) return std::nullopt;
    return BowlLift{town->node, stands + kBowlSlackCm, stands - z};
}

bool PlayerWorld::load_town_platforms(const std::string& path, std::string& error) {
    auto set = std::make_unique<world::TownPlatforms>();
    if (!set->load(path, &error)) return false;
    size_t placed = 0;
    for (const world::TownBowl& bowl : world::kTownBowls) {
        const world::CityNodeInfo* node = city_nodes_ ? city_nodes_->by_guid(bowl.node) : nullptr;
        if (!node) {
            spdlog::warn("[floor] {} is not in the node registry: its platform is not placed (the interim town floor model answers there)", bowl.name);
            continue;
        }
        if (!set->place(bowl.node, bowl.layout, {node->x, node->y, node->z, node->yaw})) {
            spdlog::warn("[floor] {}: the file has no layout {}: its platform is not placed", node->display_name, bowl.layout);
            continue;
        }
        ++placed;
        std::string pit;
        double terrain = 0;
        const auto floor = set->floor_at(node->x, node->y);
        if (floor && nav_ && nav_->terrain_height(node->x, node->y, &terrain)) pit = fmt::format(", the baked pit under it {:.1f} m lower", (floor->z - terrain) / 100.0);
        spdlog::info("[floor] {} (node {}): platform {} at the record ({:.0f}, {:.0f}, {:.0f}) facing {:.2f}; the floor at its place is z {:.1f}{}",
                     node->display_name, bowl.node, bowl.layout, node->x, node->y, node->z, node->yaw, floor ? floor->z : 0.0, pit);
    }
    spdlog::info("[floor] {} platform layout(s) read from {}: {} bowl(s) placed; the footprint is the platform's non-hole part (a rounded square of about 900 m), the floor inside it the "
                 "platform's, never the baked pit; the interim town floor model answers only for nodes whose layout is not read",
                 set->layouts(), path, placed);
    platforms_ = std::move(set);
    return true;
}

std::optional<PlayerWorld::FloorRescue> PlayerWorld::floor_rescue(double x, double y, double z) const {
    if (!finite_xyz(x, y, z)) return std::nullopt;
    if (const auto bowl = bowl_floor_at(x, y)) {
        const std::string bowl_name = node_display_name(city_nodes_, bowl->node);
        const FloorRescue back{x, y, bowl->z + kCapsuleHalfHeightCm + kBowlSlackCm, bowl->node, "the floor of " + bowl_name};
        if (z < bowl->z - kBelowFloorCm || z < kKillPlaneCm) return back;
        if (const auto depth_m = baked_pit_depth_m(platforms_.get(), nav_.get(), x, y, z)) {
            FloorRescue pit = back;
            pit.where = "the floor of " + bowl_name + " (it stands on the pit " + std::to_string(*depth_m) + " m under it)";
            return pit;
        }
        return std::nullopt;
    }
    if (z >= kKillPlaneCm) return std::nullopt;
    if (const auto town = town_ground_at(x, y)) {
        return FloorRescue{x, y, town->z + kCapsuleHalfHeightCm + kBowlSlackCm, town->node, "the floor of " + node_display_name(city_nodes_, town->node)};
    }
    double ground = 0;
    if (nav_ && nav_->terrain_height(x, y, &ground) && std::isfinite(ground)) return FloorRescue{x, y, ground + kCapsuleHalfHeightCm + kBowlSlackCm, 0, "the ground"};
    return FloorRescue{area_.center_x, area_.center_y, spawns::kLoginSpotZ, 0, "the login spot"};
}

double PlayerWorld::place_height(const world::Place& place) const {
    if (const auto ground = ground_at(place.x, place.y)) return *ground + kCapsuleHalfHeightCm;
    return place.z + kPlaceLiftCm;
}

world::BorderDecision evaluate_sub_area_handover(
    world::RegionalHandoverState& s, const spawns::AreaConfig& area, double x, double y, int64_t eff_ms) noexcept {
    const float signed_dist = area.signed_border_distance_cm(x, y);
    const bool inside = area.contains(x, y);
    const spawns::RegionBit pt_reg = spawns::resolve_point_region_bit(x, y);
    s.last_signed_border_distance_cm = signed_dist;
    s.outside_worker_area = !inside;
    const float hyst_cm = std::max(0.0f, area.handover_hysteresis_cm);
    if (inside) {
        s.dwell_ticks_beyond_hysteresis = 0;
        s.candidate_region = s.authoritative_region;
        if (s.authority_state == world::AuthorityState::HandoverPending && signed_dist > -hyst_cm) {
            s.last_suppressed_by_hysteresis = true;
        } else {
            s.authority_state = world::AuthorityState::Local;
            s.last_suppressed_by_hysteresis = false;
        }
        return (s.authority_state == world::AuthorityState::HandoverPending)
                   ? world::BorderDecision::HandoverPending
                   : world::BorderDecision::StayLocal;
    }
    s.candidate_region = (pt_reg != spawns::RegionBit::None) ? pt_reg : s.authoritative_region;
    const bool in_cd = (eff_ms > 0 && s.last_handover_commit_ms >= 0 &&
                        (eff_ms - s.last_handover_commit_ms) < world::runtime_handover_cooldown_ms());
    if (in_cd || signed_dist < hyst_cm) {
        s.dwell_ticks_beyond_hysteresis = 0;
        s.last_suppressed_by_hysteresis = true;
        ++s.suppressed_oscillations;
        return (s.authority_state == world::AuthorityState::HandoverPending)
                   ? world::BorderDecision::HandoverPending
                   : world::BorderDecision::StayLocal;
    }
    if (s.dwell_ticks_beyond_hysteresis < 255) ++s.dwell_ticks_beyond_hysteresis;
    if (s.dwell_ticks_beyond_hysteresis < world::runtime_handover_dwell_samples()) {
        s.last_suppressed_by_hysteresis = true;
        return world::BorderDecision::StayLocal;
    }
    s.last_suppressed_by_hysteresis = false;
    if (s.authority_state != world::AuthorityState::HandoverPending) {
        s.authority_state = world::AuthorityState::HandoverPending;
        ++s.lease_epoch;
        ++s.handover_count;
        s.last_handover_commit_ms = eff_ms;
    }
    return world::BorderDecision::HandoverPending;
}

world::BorderDecision PlayerWorld::step_player_border_handover(
    uint64_t player_id, double x, double y, int64_t now_ms) {
    if (!std::isfinite(x) || !std::isfinite(y)) return world::BorderDecision::StayLocal;
    auto [it, inserted] = regional_handover_states_.try_emplace(player_id);
    if (inserted) {
        const uint8_t mask = area_.region_mask;
        it->second.authoritative_region =
            (mask != 0 && mask != static_cast<uint8_t>(spawns::RegionBit::All))
                ? static_cast<spawns::RegionBit>(mask)
                : spawns::resolve_point_region_bit(x, y);
        it->second.candidate_region = it->second.authoritative_region;
    }
    const int64_t eff_ms = (now_ms >= 0) ? now_ms : static_cast<int64_t>(world_clock_ * 1000.0);
    const bool sub_area = !area_.zoi_guids.empty() || !area_.exclude_zoi_guids.empty() ||
                          !area_.include_node_plugs.empty() || !area_.exclude_node_plugs.empty() ||
                          area_.polygon_cutout_cm.size() >= 3 || !area_.excluded_polygons_cm.empty() || !area_.region;
    world::BorderDecision decision = world::BorderDecision::StayLocal;
    if (!sub_area) {
        decision = world::evaluate_regional_handover(it->second, x, y, eff_ms, area_.handover_hysteresis_cm);
        it->second.outside_worker_area = !area_.contains(x, y);
    } else {
        decision = evaluate_sub_area_handover(it->second, area_, x, y, eff_ms);
    }
    authority_states_[player_id] = it->second.authority_state;
    (void)try_advance_lease_epoch(player_id, it->second.lease_epoch);
    return decision;
}

void PlayerWorld::commit_player_handover(uint64_t player_id, int64_t now_ms) noexcept {
    auto [it, _] = regional_handover_states_.try_emplace(player_id);
    const int64_t eff_ms = (now_ms >= 0) ? now_ms : static_cast<int64_t>(world_clock_ * 1000.0);
    world::complete_border_handover(it->second, eff_ms);
    authority_states_[player_id] = it->second.authority_state;
    (void)try_advance_lease_epoch(player_id, it->second.lease_epoch);
}

size_t PlayerWorld::sync_boundary_halo_actors(PlayerWorld& other, double halo_cm) {
    if (&other == this) return 0;
    const double eff_halo = (halo_cm >= 0.0) ? halo_cm : area_.aoi_border_halo_cm;
    const auto sync_one_way = [eff_halo](PlayerWorld& src, PlayerWorld& dst) -> size_t {
        size_t synced = 0;
        src.registry().for_each([&](const world::WorldActor& a) {
            if (a.kind != world::ActorKind::Player && a.kind != world::ActorKind::Npc &&
                a.kind != world::ActorKind::Mount && a.kind != world::ActorKind::Pile) {
                return;
            }
            if (!dst.area().contains_with_halo(a.x, a.y, eff_halo)) return;
            if (world::WorldActor* ex = dst.registry().find(a.id)) {
                if (std::hypot(ex->x - a.x, ex->y - a.y) > 0.1 || std::abs(ex->z - a.z) > 0.1 || std::abs(ex->yaw_degrees - a.yaw_degrees) > 0.01) {
                    dst.registry().move(a.id, a.x, a.y, a.z, a.yaw_degrees);
                }
                if (ex->reliable_version < a.reliable_version || ex->version < a.version) {
                    dst.registry().modify(a.id, world::Change::State, [&](world::WorldActor& m) {
                        m.actor = a.actor;
                        m.components = a.components;
                    });
                }
                ++synced;
            } else {
                world::WorldActor copy = a;
                if (dst.registry().add(std::move(copy))) ++synced;
            }
            if (src.aftermath() && dst.aftermath()) {
                if (const combat::Corpse* sc = src.aftermath()->corpse_of(a.id)) {
                    dst.aftermath()->upsert_halo_corpse(*sc, dst.world_clock());
                    if (const combat::Corpse* dc = dst.aftermath()->corpse_of(a.id)) {
                        src.aftermath()->upsert_halo_corpse(*dc, src.world_clock());
                    }
                }
            }
        });
        return synced;
    };
    return sync_one_way(*this, other) + sync_one_way(other, *this);
}

void PlayerWorld::note_player(uint64_t player_id, double x, double y) {
    if (npcs_) npcs_->note_player(player_id, x, y, world_clock_);
    if (std::isfinite(x) && std::isfinite(y)) {
        (void)step_player_border_handover(player_id, x, y);
    }
    if (!nav_ || !nav_->has_region() || !std::isfinite(x) || !std::isfinite(y)) return;
    const bool outside = !nav_->in_region(x, y);
    bool& was = outside_[player_id];
    if (outside && !was) {
        was = true;
        ++region_exits_;
        const auto last = outside_logged_at_.find(player_id);
        const bool loud = last == outside_logged_at_.end() || world_clock_ - last->second >= kRegionLogSeconds;
        if (loud) outside_logged_at_[player_id] = world_clock_;
        exit_loud_[player_id] = loud;
        spdlog::log(loud ? spdlog::level::warn : spdlog::level::debug,
                    "[region] player {} left the Riverlands at ({:.0f}, {:.0f}): no navmesh and no spawns beyond the edge; there is no wall, the move checks go on without a ground (logged once per exit, at most every {:.0f} s)",
                    player_id, x, y, kRegionLogSeconds);
    } else if (!outside && was) {
        was = false;
        spdlog::log(exit_loud_[player_id] ? spdlog::level::info : spdlog::level::debug, "[region] player {} is back inside the Riverlands at ({:.0f}, {:.0f})", player_id, x, y);
    }
}

void PlayerWorld::forget_player(uint64_t player_id) {
    if (npcs_) npcs_->forget_player(player_id);
    outside_.erase(player_id);
    outside_logged_at_.erase(player_id);
    exit_loud_.erase(player_id);
    forget_border_authority(player_id);
}

int PlayerWorld::item_quality(int64_t item) const {
    const auto it = quality_cache_.find(item);
    if (it != quality_cache_.end()) return it->second;
    int q = 1;
    if (content_) {
        std::string err;
        if (const auto rec = content_->item(item, err)) q = std::max(1, rec->default_quality);
    }
    quality_cache_[item] = q;
    return q;
}

const items::ItemRarityFacts& PlayerWorld::item_rarity_facts(int64_t item) const {
    auto it = rarity_cache_.find(item);
    if (it == rarity_cache_.end()) {
        items::ItemRarityFacts facts = items::default_rarity_facts();
        if (content_) {
            std::string err;
            if (const auto rec = content_->item(item, err)) facts = *rec;   // (its rarity facts: the quality curve and RarityMin..RarityMax)
        }
        it = rarity_cache_.emplace(item, facts).first;
    }
    return it->second;
}

int PlayerWorld::item_rarity(int64_t item, int quality) const { return item_rarity_facts(item).rarity_of(static_cast<double>(quality)); }

std::string PlayerWorld::item_name(int64_t item) const {
    const auto it = name_cache_.find(item);
    if (it != name_cache_.end()) return it->second;
    std::string n;
    if (content_) {
        std::string err;
        if (const auto rec = content_->item(item, err)) n = rec->name;
    }
    name_cache_[item] = n;
    return n;
}

npc::SpeedModel PlayerWorld::speeds() const {
    npc::SpeedModel s;
    if (generic_config_.hunting) s.standing_defs = &generic_config_.hunting_defs;   // (empty without --gathering: nothing stands)
    if (!standing_archetypes_.empty()) s.standing_archetypes = &standing_archetypes_;
    return s;
}

bootstrap::SessionValues PlayerWorld::new_session(double world_time) {
    bootstrap::SessionValues s;
    s.server_id = server_id_;
    s.world_time = world_time;
    s.player_id = next_player_id_++;
    s.guids = bootstrap::mint_session_guids(guids_, game_state_);
    return s;
}

}}  // namespace aoc::worldd
