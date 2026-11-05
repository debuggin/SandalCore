// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_world_node_pop_common.h"

using namespace aoc::worldd;
using namespace aoc::worldd::test_node_pop;
using FloorPerson = world::TownFloor::Person;
namespace fs = std::filesystem;

#ifndef NODES_NAV_PATH
#define NODES_NAV_PATH "dist/content/area_nav.bin"
#endif

static double terrain_at(const nav::AreaNav &nav, double x, double y) {
    double t = 0;
    nav.terrain_height(x, y, &t);
    return t;
}

static const world::CityNodeInfo *find_city_node_by_name(const char *name) {
    const world::CityNodeInfo *info = nullptr;
    for (const world::CityNodeInfo &n : g_env.nodes->nodes()) {
        if (n.name == name) info = &n;
    }
    return info;
}

struct JoinSpot {
    const char *what;
    double x;
    double y;
};

static void check_joeva_bowl_join(const nav::AreaNav &nav, const world::CityNodeInfo &info,
                                  const world::TownFloor &tf) {
    char msg[300];
    const double ux = -1079493.1, uy = 508464.4, uz = 7748.6, ut = terrain_at(nav, ux, uy);
    const auto user = tf.at(ux, uy, ut);
    std::snprintf(msg, sizeof(msg),
                  "Joeva: the user's saved spot (z %.1f, baked ground %.1f) is "
                  "in the bowl: the town's floor %.0f, "
                  "%.0f m over it",
                  uz, ut, user ? ut + user->lift : 0.0, user ? (ut + user->lift - uz) / 100.0 : 0.0);
    CHECK(user && ut + user->lift + 98.0 - uz > 4000.0, msg);
    std::printf("    %s\n", msg);

    const JoinSpot join_spots[] = {
        {"the node's place (worldctl tp <character> joeva)", info.x, info.y},
        {"Joeva's market square (the Market_v3 plot in the node record's frame)", -1087417.6, 503591.5},
        {"a spot saved on the baked ground under the town", ux, uy}};
    for (const JoinSpot &sp : join_spots) {
        const double ground = terrain_at(nav, sp.x, sp.y);
        const auto floor_here = tf.at(sp.x, sp.y, ground);
        if (!floor_here) {
            CHECK(false, sp.what);
            continue;
        }
        const double model_floor = ground + floor_here->lift;
        const bool lifted =
            model_floor + PlayerWorld::kCapsuleHalfHeightCm - (ground + 98.0) >= world::TownFloor::kBowlCm;
        const double join_z = model_floor + PlayerWorld::kCapsuleHalfHeightCm + PlayerWorld::kBowlSlackCm;
        std::snprintf(msg, sizeof(msg),
                      "Joeva, %s (%.0f, %.0f): baked ground %.1f, the model's "
                      "floor %.1f; joined from the pit at z "
                      "%.1f = floor + %.0f cm (the trace needs 98 to 1,000)",
                      sp.what, sp.x, sp.y, ground, model_floor, join_z, join_z - model_floor);
        CHECK(lifted && join_z - model_floor >= 98.0 && join_z - model_floor <= 1000.0, msg);
        std::printf("    %s\n", msg);
    }
}

struct BowlAnchor {
    double x;
    double y;
    double z;
};

static void check_joeva_bowl_anchors(const nav::AreaNav &nav, const world::TownFloor &tf) {
    const BowlAnchor anchors[] = {{-1081432, 516570, 14753}, {-1097474, 509458, 14024}, {-1091333, 510020, 13408},
                                  {-1085254, 503627, 12608}, {-1089840, 503696, 12625}, {-1078157, 512855, 13046},
                                  {-1087121, 515470, 14042}, {-1081577, 505744, 12638}, {-1079600, 509538, 13290}};
    size_t close = 0, within = 0;
    double worst = 0;
    for (const BowlAnchor &a : anchors) {
        const double t = terrain_at(nav, a.x, a.y);
        const auto f = tf.at(a.x, a.y, t);
        if (!f) continue;
        const double err = std::fabs(t + f->lift - a.z);
        worst = std::max(worst, err);
        close += err <= 200.0 ? 1 : 0;
        within += err <= 1300.0 ? 1 : 0;
    }
    char msg[300];
    std::snprintf(msg, sizeof(msg),
                  "Joeva: against retail's 9 anchors the floor (terrain + lift) "
                  "is within 2 m of %zu and within 13 m "
                  "of %zu (the most %.0f cm: a tower, a terrace)",
                  close, within, worst);
    CHECK(close >= 7 && within == 9, msg);
    std::printf("    %s\n", msg);
}

static void build_bowl_floor(const nav::AreaNav &nav, const char *name, const world::CityNodeInfo &info, int level,
                             world::TownFloor &tf) {
    spawns::NodePopStats st;
    const auto list = select_real(info, level, st);
    std::vector<FloorPerson> persons;
    for (const spawns::NodeSpawn &n : list) {
        FloorPerson p{n.spawn.x, n.spawn.y, 0, n.spawn.z};
        if (nav.terrain_height(p.x, p.y, &p.ground)) persons.push_back(p);
    }
    tf.set(info.guid, persons);
    const std::optional<double> lift = tf.lift_of(info.guid);
    size_t open = 0, open_in = 0;
    for (const FloorPerson &p : persons) {
        if (p.z - p.ground > world::TownFloor::kBowlCm) continue;
        ++open;
        if (tf.at(p.x, p.y, p.ground)) ++open_in;
    }
    char msg[300];
    std::snprintf(msg, sizeof(msg),
                  "%s at level %d: %zu people, %zu on the town's floor, lift "
                  "%.0f cm; of the %zu on open ground (+1 m) "
                  "%zu are taken for the bowl",
                  name, level, persons.size(), tf.floor_persons(info.guid), lift.value_or(0.0), open, open_in);
    CHECK(lift && *lift > 4800.0 && *lift < 5200.0 && tf.floor_persons(info.guid) >= (level == 4 ? 250u : 100u), msg);
    std::printf("    %s\n", msg);
}

static void check_real_bowl_level4(const nav::AreaNav &nav, const char *name, const world::CityNodeInfo &info,
                                   const world::TownFloor &tf) {
    char msg[300];
    const double rt = terrain_at(nav, info.x, info.y);
    const auto at_record = tf.at(info.x, info.y, rt);
    std::snprintf(msg, sizeof(msg),
                  "%s: the node's place (%.0f, %.0f), baked ground %.0f, is in "
                  "the bowl: the floor is %.0f",
                  name, info.x, info.y, rt, at_record ? rt + at_record->lift : 0.0);
    CHECK(at_record.has_value(), msg);
    std::printf("    %s\n", msg);

    for (const world::node_origins::Origin &o : world::node_origins::kOrigins) {
        if (o.node != info.guid) continue;
        const double ot = terrain_at(nav, o.x, o.y);
        const auto of = tf.at(o.x, o.y, ot);
        std::snprintf(msg, sizeof(msg),
                      "%s: retail opened the layout at (%.0f, %.0f, %.0f), baked "
                      "ground %.0f: the floor there is %.0f (%+.0f cm)",
                      name, o.x, o.y, o.z, ot, of ? ot + of->lift : 0.0, of ? o.z - (ot + of->lift) : 0.0);
        CHECK(of.has_value(), msg);
        std::printf("    %s\n", msg);
    }
    if (std::string(name) == "Verra_RVR_Joeva") {
        check_joeva_bowl_join(nav, info, tf);
        check_joeva_bowl_anchors(nav, tf);
    }
}

static void check_real_bowl_node(const nav::AreaNav &nav, const char *name) {
    const world::CityNodeInfo *info = find_city_node_by_name(name);
    CHECK(info != nullptr, name);
    if (!info) return;
    for (const int level : {0, 4}) {
        world::TownFloor tf;
        build_bowl_floor(nav, name, *info, level, tf);
        if (level == 4) check_real_bowl_level4(nav, name, *info, tf);
    }
}

static void test_real_bowls() {
    const char *env = std::getenv("AOC_REAL_GROUND");
    if (!env || !*env) {
        std::printf("the five bowls on the real ground: opt-in (AOC_REAL_GROUND=1 "
                    "reads the heightfield of %s)\n",
                    NODES_NAV_PATH);
        return;
    }
    std::printf("the five bowls on the real ground (%s)\n", NODES_NAV_PATH);
    std::string error;
    auto nav = nav::AreaNav::load(NODES_NAV_PATH, &error);
    CHECK(nav != nullptr, "the chunked ground opens");
    if (!nav) {
        std::printf("    %s\n", error.c_str());
        return;
    }
    const char *names[] = {"Verra_RVR_Winstead", "Verra_RVR_Joeva", "Verra_RVR_NewAela", "Verra_RVR_Halcyon",
                           "Verra_RVR_Miraleth"};
    for (const char *name : names) check_real_bowl_node(*nav, name);
}

static bool step_on_ground(const nav::AreaNav &map, const nav::Vec3 &a, const nav::Vec3 &b) {
    const nav::Vec3 fa{a.x, a.y, a.z - 96.0}, fb{b.x, b.y, b.z - 96.0};
    if (map.raycast(fa, fb)) return true;
    return map.on_navmesh({(fa.x + fb.x) / 2, (fa.y + fb.y) / 2, (fa.z + fb.z) / 2}, 40.0, 150.0);
}

static std::string content_file(const char *name) {
    return (std::filesystem::path(db_path()).parent_path() / name).string();
}

struct RealTown {
    int64_t node;
    int level;
    const char *routes[3];
    size_t snapped_far;
};

static void log_stuck_npcs(const PlayerWorld &world, const npc::NpcWorld &npcs, const std::vector<uint64_t> &ids) {
    std::map<std::string, size_t> stuck_by_kind;
    for (const uint64_t id : ids) {
        const npc::NpcWorld::StandReport one = npcs.stand_report({id});
        if (one.stuck == 0) continue;
        const world::WorldActor *a = world.registry().find(id);
        ++stuck_by_kind[a ? spawns::blueprint_leaf(a->npc.archetype) : std::string("?")];
    }
    std::string list;
    for (const auto &[kind, n] : stuck_by_kind) list += (list.empty() ? "" : ", ") + kind + " x" + std::to_string(n);
    std::printf("    no floor, would walk: %s\n", list.empty() ? "none" : list.c_str());
}

static void check_town_stand_report(PlayerWorld &world, const std::vector<uint64_t> &ids, size_t made) {
    npc::NpcWorld &npcs = *world.npcs();
    const npc::NpcWorld::StandReport rep = npcs.stand_report(ids);
    std::printf("    %zu people: %zu on a floor within %.0f cm of where the data "
                "puts them, %zu on a floor up to %.0f cm from "
                "it, %zu with none (%zu of them with no walkable ground within "
                "%.0f cm; %zu would walk), %zu waiting\n",
                rep.people, rep.on_floor, npc::NpcWorld::kStandNearCm, rep.moved, rep.max_moved_cm, rep.no_floor,
                rep.blocked, npc::NpcWorld::kBlockedCm, rep.stuck, rep.waiting);
    CHECK(rep.people == made && rep.waiting == 0, "all of them have their ground (the area's chunks are resident)");
    log_stuck_npcs(world, npcs, ids);
    CHECK(rep.on_floor * 100 >= rep.people * 60, "at least six in ten stand on a floor within 30 cm of where the data "
                                                 "puts them");
    CHECK(rep.no_floor * 100 <= rep.people * 8, "at most 8 in 100 have no floor in reach");
}

struct SnapStats {
    size_t snapping = 0;
    size_t flying = 0;
    size_t without_floor = 0;
    size_t persons = 0;
    size_t far_moved = 0;
    size_t far_moved_at_street = 0;
};

static void inspect_snapping_actor(PlayerWorld &world, const world::WorldActor &a, SnapStats &st) {
    if (!a.npc.snaps_to_ground) return;
    ++st.snapping;
    st.flying += a.npc.flies ? 1 : 0;
    st.persons += npc::is_beast_archetype(a.npc.archetype) ? 0 : 1;
    const npc::NpcWorld::StandReport one = world.npcs()->stand_report({a.id});
    st.without_floor += one.no_floor;
    if (one.max_moved_cm <= 300.0) return;
    const double foot = g_env.table.get(a.npc.archetype).half_height * npc::actor_scale_z(a);
    const auto street_floor = world.bowl_floor_at(a.npc.home_x, a.npc.home_y);
    const double was = street_floor ? a.npc.home_z - foot - street_floor->z : 0.0;
    const double now = street_floor ? a.z - foot - street_floor->z : 0.0;
    ++st.far_moved;
    if (street_floor && std::fabs(now) <= 250.0) ++st.far_moved_at_street;
    std::printf("      %s (%s): the data puts its feet at %+.0f cm to the "
                "street, it stands at %+.0f cm\n",
                spawns::blueprint_leaf(a.npc.archetype).c_str(), a.npc.flies ? "flies" : "walks", was, now);
}

static void check_town_snapping_creatures(PlayerWorld &world, const world::CityNodeInfo &real, const RealTown &rt,
                                          const std::vector<uint64_t> &ids) {
    SnapStats st;
    for (const uint64_t id : ids) {
        const world::WorldActor *a = world.registry().find(id);
        if (a) inspect_snapping_actor(world, *a, st);
    }
    std::printf("    %zu creatures the data says to snap to the ground (%zu of them "
                "fly): %zu without a floor, %zu "
                "moved more than 3 m, %zu of those now within 2.5 m of the street\n",
                st.snapping, st.flying, st.without_floor, st.far_moved, st.far_moved_at_street);
    CHECK(st.snapping > 0 && st.persons == 0, "creatures of the town snap to the ground, and no person is one");
    CHECK(st.without_floor == 0, "every one of them has a floor of the walking map under it");
    char msg[200];
    std::snprintf(msg, sizeof(msg),
                  "%s: the creatures the data draws more than 3 m from the "
                  "street come to it (%zu, %s %zu), each "
                  "within 2.5 m of it",
                  real.display_name.c_str(), st.far_moved, rt.snapped_far ? "at least" : "exactly", rt.snapped_far);
    CHECK(st.far_moved_at_street == st.far_moved &&
              (rt.snapped_far ? st.far_moved >= rt.snapped_far : st.far_moved == 0),
          msg);
}

static void check_nav_before(const world::CityNodeInfo &info, const std::vector<std::array<double, 3>> &raised_spots,
                             size_t raised_on_floor) {
    const char *before_path = std::getenv("AOC_NAV_BEFORE");
    if (!before_path || !*before_path) return;
    std::string before_error;
    auto before = nav::AreaNav::load(before_path, &before_error);
    if (!before || !before->load_around(info.x, info.y, 2)) {
        std::printf("    (AOC_NAV_BEFORE %s does not load: %s)\n", before_path, before_error.c_str());
        return;
    }
    size_t before_on_floor = 0;
    const npc::NpcWorldConfig cfg{};
    for (const auto &s : raised_spots) {
        double gz = 0;
        if (before->ground_height(s[0], s[1], s[2], &gz, cfg.data_ground_below_cm, cfg.data_ground_above_cm) &&
            std::fabs(gz - s[2]) <= npc::NpcWorld::kStandNearCm) {
            ++before_on_floor;
        }
    }
    std::printf("    the same %zu raised people on the walking map of %s: %zu on "
                "a floor within 30 cm (now %zu)\n",
                raised_spots.size(), before_path, before_on_floor, raised_on_floor);
}

static void collect_raised_and_street_spots(PlayerWorld &world, const world::CityNodeInfo &info,
                                            const std::vector<uint64_t> &ids,
                                            std::vector<std::array<double, 3>> &raised_spots,
                                            std::vector<std::array<double, 3>> &street_spots) {
    size_t raised = 0, raised_on_floor = 0, street = 0, street_on_floor = 0;
    for (const uint64_t id : ids) {
        const world::WorldActor *a = world.registry().find(id);
        if (!a) continue;
        const double feet = a->npc.home_z - 88.0;
        const auto plat = world.bowl_floor_at(a->npc.home_x, a->npc.home_y);
        if (!plat) continue;
        const npc::NpcWorld::StandReport one = world.npcs()->stand_report({id});
        if (feet - plat->z > 300.0) {
            ++raised;
            raised_on_floor += one.on_floor;
            raised_spots.push_back({a->npc.home_x, a->npc.home_y, feet});
        } else if (feet - plat->z > -100.0) {
            ++street;
            street_on_floor += one.on_floor;
            street_spots.push_back({a->npc.home_x, a->npc.home_y, feet});
        }
    }
    std::printf("    people over 3 m above the platform: %zu, of them on a floor "
                "within 30 cm: %zu (the 2a map had "
                "none); at the street: %zu, on a floor within 30 cm: %zu\n",
                raised, raised_on_floor, street, street_on_floor);
    CHECK(raised > 0 && raised_on_floor * 100 >= raised * 55,
          "more than half of the people on terraces and towers stand on a floor "
          "of the map");
    check_nav_before(info, raised_spots, raised_on_floor);
}

static const content::NpcRouteRow *find_route_by_name(const char *name) {
    const content::NpcRouteRow *route = nullptr;
    for (const auto &[guid, r] : g_env.pop->routes) {
        if (r.name == name) route = &r;
    }
    return route;
}

static void sample_route_leg(const nav::AreaNav &map, const nav::Vec3 &p0, const nav::Vec3 &p1, size_t &samples,
                             size_t &covered) {
    const double len = std::hypot(p1.x - p0.x, p1.y - p0.y);
    const int k = std::max(1, static_cast<int>(len / 50.0));
    for (int s = 0; s <= k; ++s) {
        const double t = static_cast<double>(s) / k;
        double gz = 0;
        ++samples;
        if (map.ground_height(p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t, p0.z + (p1.z - p0.z) * t, &gz, 100.0,
                              300.0))
            ++covered;
    }
}

static void check_single_town_route(const nav::AreaNav &map, const world::CityNodeInfo &info, const char *name) {
    const content::NpcRouteRow *route = find_route_by_name(name);
    if (!route || route->waypoints.size() < 2) {
        CHECK(false, name);
        return;
    }
    std::vector<nav::Vec3> pts;
    for (const auto &w : route->waypoints) {
        nav::Vec3 p;
        npc::place_node_route(info, w[0], w[1], w[2], &p.x, &p.y, &p.z);
        pts.push_back(p);
    }
    if (route->looping) pts.push_back(pts.front());
    size_t samples = 0, covered = 0, legs = 0, legs_found = 0;
    double worst_ratio = 0;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const double len = std::hypot(pts[i + 1].x - pts[i].x, pts[i + 1].y - pts[i].y);
        sample_route_leg(map, pts[i], pts[i + 1], samples, covered);
        ++legs;
        const nav::PathResult path = map.find_path(pts[i], pts[i + 1], 150.0, 300.0, 60000);
        if (path.complete) {
            ++legs_found;
            if (len > 300.0) worst_ratio = std::max(worst_ratio, path.length / len);
        }
    }
    std::printf("    route %s: %zu waypoints, ground within -1..+3 m of its "
                "height on %.0f %% of %zu samples; a path "
                "between the waypoints for %zu of %zu legs (the longest is %.2f "
                "times the straight line)\n",
                name, route->waypoints.size(), 100.0 * static_cast<double>(covered) / static_cast<double>(samples),
                samples, legs_found, legs, worst_ratio);
}

static void summon_answers(PlayerWorld &world, const std::vector<std::array<double, 3>> &spots, size_t *allowed,
                           size_t *total, size_t *spots_with_none) {
    *allowed = *total = *spots_with_none = 0;
    for (const auto &s : spots) {
        size_t ok_here = 0;
        for (int k = 0; k < 8; ++k) {
            const double a = k * 3.14159265358979323846 / 4.0;
            const auto g =
                world.floor_under(s[0] + 575.0 * std::cos(a), s[1] + 575.0 * std::sin(a), s[2], 1000.0, 160.0);
            ++*total;
            if (g && std::fabs(*g - s[2]) <= 250.0) {
                ++*allowed;
                ++ok_here;
            }
        }
        *spots_with_none += ok_here == 0 ? 1 : 0;
    }
}

static void check_town_summons(PlayerWorld &world, const std::vector<std::array<double, 3>> &raised_spots,
                               const std::vector<std::array<double, 3>> &street_spots) {
    size_t allowed = 0, total = 0, none = 0;
    summon_answers(world, street_spots, &allowed, &total, &none);
    std::printf("    a summon at %zu street places, 8 headings each: allowed %zu "
                "of %zu, no heading allowed at %zu places\n",
                street_spots.size(), allowed, total, none);
    CHECK(total > 0 && allowed * 100 >= total * 90 && none * 100 <= street_spots.size() * 3,
          "on the street a summon is allowed in nine headings in ten (not facing "
          "a drop)");
    std::vector<std::array<double, 3>> terraces, towers;
    for (const auto &s : raised_spots) {
        const auto plat = world.bowl_floor_at(s[0], s[1]);
        (s[2] - plat->z > 1500.0 ? towers : terraces).push_back(s);
    }
    summon_answers(world, terraces, &allowed, &total, &none);
    std::printf("    at %zu places 3 to 15 m up (terraces, upper floors): "
                "allowed %zu of %zu, no heading allowed at "
                "%zu places\n",
                terraces.size(), allowed, total, none);
    summon_answers(world, towers, &allowed, &total, &none);
    std::printf("    at %zu places over 15 m up (towers, high walls): allowed "
                "%zu of %zu, no heading allowed at %zu places\n",
                towers.size(), allowed, total, none);
}

struct WalkerTrack {
    nav::Vec3 last;
    bool have = false;
};

static bool step_walker_track(const nav::AreaNav &map, const world::WorldActor &a, WalkerTrack &tr, size_t &rays,
                              size_t &off_ground) {
    const nav::Vec3 p{a.x, a.y, a.z};
    bool walked = false;
    if (tr.have && std::hypot(p.x - tr.last.x, p.y - tr.last.y) > 0.5) {
        ++rays;
        walked = true;
        if (!step_on_ground(map, tr.last, p)) ++off_ground;
    }
    tr.last = p;
    tr.have = true;
    return walked;
}

static void check_town_walkers_simulation(PlayerWorld &world, const world::CityNodeInfo &info) {
    npc::NpcWorld &npcs = *world.npcs();
    const nav::AreaNav &map = *world.nav();
    std::map<uint64_t, WalkerTrack> tracks;
    std::vector<uint64_t> walkers;
    world.registry().for_each([&](const world::WorldActor &a) {
        if (a.kind == world::ActorKind::Npc && std::hypot(a.x - info.x, a.y - info.y) < 14000.0 && !npcs.stands(a.id))
            walkers.push_back(a.id);
    });
    size_t rays = 0, off_ground = 0;
    std::set<uint64_t> moved;
    double t = world.nav()->windowed() ? 1.0 : 0.0;
    for (int tick = 0; tick < 15 * 120; ++tick) {
        t += 1.0 / 15.0;
        world.note_player(7, info.x, info.y);
        world.tick_world(t);
        for (const uint64_t id : walkers) {
            const world::WorldActor *a = world.registry().find(id);
            if (a && step_walker_track(map, *a, tracks[id], rays, off_ground)) moved.insert(id);
        }
    }
    const size_t walking_at_all = moved.size();
    std::printf("    %zu walkers (people and animals) near the square; in 2 "
                "minutes %zu of them walked, %zu steps, %zu "
                "of them not along the walkable ground\n",
                walkers.size(), walking_at_all, rays, off_ground);
    CHECK(walking_at_all > 0 && rays > 100, "people and animals walk");
    CHECK(off_ground * 1000 <= rays, "no more than one step in a thousand leaves the walkable ground (a ray "
                                     "along it stays on the map)");
}

static void verify_populated_town(PlayerWorld &world, const world::CityNodeInfo &real, const world::CityNodeInfo &info,
                                  const RealTown &rt, size_t made) {
    const std::vector<uint64_t> ids = world.node_people()->actors_of(rt.node);
    check_town_stand_report(world, ids, made);
    check_town_snapping_creatures(world, real, rt, ids);
    std::vector<std::array<double, 3>> raised_spots, street_spots;
    collect_raised_and_street_spots(world, info, ids, raised_spots, street_spots);
    for (const char *name : rt.routes) check_single_town_route(*world.nav(), info, name);
    check_town_summons(world, raised_spots, street_spots);
    check_town_walkers_simulation(world, info);
}

static void run_real_town_building_case(const RealTown &rt) {
    std::string error;
    const world::CityNodeInfo *real = g_env.nodes->by_guid(rt.node);
    if (!real) {
        CHECK(false, "the town is in the registry");
        return;
    }
    std::printf("%s at level %d on the real walking map (%s)\n", real->display_name.c_str(), rt.level,
                content_file("area_nav.bin").c_str());
    world::CityNodeInfo info = *real;
    std::vector<world::CityZoneInfo> zones{*g_env.nodes->zone_by_guid(info.zoi_guid)};
    world::CityNodes synth({info}, zones);
    set_level(synth, rt.node, rt.level);
    auto wire = world::NodeWire::load(db_path(), synth, error);
    auto nav = nav::AreaNav::load(content_file("area_nav.bin"), &error);
    if (!wire || !nav) {
        CHECK(false, "the node wire and the walking map load");
        std::printf("    %s\n", error.c_str());
        return;
    }
    aoc::protocol::ServerGuidAllocator guids{60, 0x6ee87ee4u};
    PlayerWorld world(&*g_env.db, "", bs::GameStateMode::Tier3, 60, guids);
    world.set_spawn_context(g_env.base);
    if (!world.ready() || !world.load_blueprints(motion_path(), error)) {
        CHECK(false, "the world is ready and the Blueprint table loads");
        return;
    }
    world.set_city_nodes(&synth);
    world.set_node_wire(wire.get());
    world.set_node_population_data(g_env.data.get());
    if (!world.load_town_platforms(content_file("town_platforms.bin"), error)) {
        CHECK(false, "the platforms load");
        std::printf("    %s\n", error.c_str());
        return;
    }
    spawns::AreaConfig circle{info.x, info.y, 90000.0};
    if (!world.load_area(circle, error) || !world.adopt_ground(std::move(nav), error)) {
        CHECK(false, "the area loads and the real ground is adopted");
        std::printf("    %s\n", error.c_str());
        return;
    }
    const size_t made = world.start_node_people({});
    CHECK(made == golden_npcs(real->name, rt.level), "the town's people are made");
    verify_populated_town(world, *real, info, rt, made);
}

static void test_real_town_buildings() {
    const char *env = std::getenv("AOC_REAL_GROUND");
    if (!env || !*env) {
        std::printf("the seed towns with their buildings on the real map: opt-in "
                    "(AOC_REAL_GROUND=1 reads "
                    "dist/content/area_nav.bin)\n");
        return;
    }
    const RealTown towns[] = {{kJoeva,
                               4,
                               {"Patrol_Node_Flat_D_L4_Middle_Guard_02", "Node_Flat_D_L4_Upper_Filler_Scholar_01",
                                "Node_Flat_D_L4_Upper_Filler_Carry_01"},
                               9},
                              {kHalcyon,
                               3,
                               {"Node_River_C_L3_GuardMidPatrol", "Node_River_C_Upper_L3_Filler_Carry_01",
                                "Node_River_C_Upper_L3_Filler_Guard_02"},
                               0}};
    for (const RealTown &rt : towns) run_real_town_building_case(rt);
}

struct PlotFolk {
    size_t total = 0, villager_f = 0, hammer = 0, shovel = 0;
};
static void classify_plot_folk(PlotFolk &f, const std::string &id) {
    ++f.total;
    if (id.find("Villager_M_Hammer") != std::string::npos) {
        ++f.hammer;
        return;
    }
    if (id.find("Villager_M_Shovel") != std::string::npos) {
        ++f.shovel;
        return;
    }
    if (id.find("Villager_F") != std::string::npos || id.find("Villager_M") != std::string::npos) ++f.villager_f;
}
static PlotFolk folk_at(const std::vector<spawns::NodeSpawn> &list, int64_t plot) {
    PlotFolk f;
    for (const spawns::NodeSpawn &s : list) {
        if (plot == 0 || s.plot != plot || !s.spawn.asset) continue;
        classify_plot_folk(f, s.spawn.asset->name + " " + s.spawn.asset->archetype_path);
    }
    return f;
}
struct WorkshopTown {
    const char *name;
    int64_t guid;
    int level;
    size_t factories, farms;
};
static void check_town_workshops(const WorkshopTown &t) {
    world::CityNodes &shipped = *g_env.shipped_nodes;
    const world::NodeWire &swire = *g_env.shipped_wire;
    const world::CityNodeInfo &info = *g_env.nodes->by_guid(t.guid);
    const std::vector<world::PlotRow> rows = swire.plot_rows_of(t.guid, t.level);
    const std::vector<world::PlotRow> base_rows = g_env.wire->plot_rows_of(t.guid, t.level);
    size_t changed = 0;
    for (size_t i = 0; i < rows.size() && i < base_rows.size(); ++i) {
        changed += rows[i].building != base_rows[i].building || rows[i].state != base_rows[i].state;
    }
    char msg[240];
    std::snprintf(msg, sizeof msg,
                  "%s at level %d: %zu plots change (the workshops of the list), "
                  "the others are as the layout has them",
                  t.name, t.level, t.factories + t.farms);
    CHECK(rows.size() == base_rows.size() && changed == t.factories + t.farms, msg);
    spawns::NodePopStats st0, st1;
    const auto base = select(*g_env.nodes, *g_env.wire, info, t.level, st0);
    const auto with = select(shipped, swire, info, t.level, st1);
    size_t villagers = 0, hammers = 0, shovels = 0, at_plots = 0;
    for (size_t i = 0; i < rows.size() && i < base_rows.size(); ++i) {
        if (rows[i].building == base_rows[i].building && rows[i].state == base_rows[i].state) continue;
        const PlotFolk was = folk_at(base, rows[i].plot), now = folk_at(with, rows[i].plot);
        villagers += now.villager_f - was.villager_f;
        hammers += now.hammer - was.hammer;
        shovels += now.shovel - was.shovel;
        at_plots += now.total - was.total;
    }
    std::printf("    %s: %zu NPCs without workshops, %zu with (%zu at the "
                "workshops: %zu villagers, %zu hammer "
                "smiths, %zu farmers)\n",
                t.name, base.size(), with.size(), at_plots, villagers, hammers, shovels);
    std::snprintf(msg, sizeof msg,
                  "%s: a factory brings 2 villagers and 2 hammer smiths, a farm "
                  "2 shovel farmers",
                  t.name);
    CHECK(villagers == 2 * t.factories && hammers == 2 * t.factories && shovels == 2 * t.farms, msg);
    std::snprintf(msg, sizeof msg, "%s has %zu NPCs more than with no workshop", t.name, 4 * t.factories + 2 * t.farms);
    CHECK(with.size() == base.size() + 4 * t.factories + 2 * t.farms, msg);
}
static void test_workshops() {
    std::printf("the workshops the seed starts the towns with\n");
    world::CityNodes &shipped = *g_env.shipped_nodes;
    const world::NodeWire &swire = *g_env.shipped_wire;
    CHECK(shipped.plot_choices(kJoeva).size() == 5 && shipped.plot_choices(kHalcyon).size() == 1 &&
              shipped.plot_choices(kWinstead).empty() && g_env.nodes->plot_choices(kJoeva).empty(),
          "the shipped seed lists 5 workshops for Joeva and 1 for Halcyon, none "
          "for Winstead; the baseline nodes (the "
          "seed without its plots) have none");
    const WorkshopTown towns[] = {{"Joeva", kJoeva, 4, 4, 1}, {"Halcyon", kHalcyon, 3, 1, 0}};
    for (const WorkshopTown &t : towns) check_town_workshops(t);
    spawns::NodePopStats a, b;
    const world::CityNodeInfo &winstead = *g_env.nodes->by_guid(kWinstead);
    CHECK(select(shipped, swire, winstead, 3, a).size() == select(*g_env.nodes, *g_env.wire, winstead, 3, b).size(),
          "Winstead (no list) has the same people with the shipped seed");
}
static std::vector<FloorPerson> floor_grid(double cx, double cy, double lift, int half = 3) {
    std::vector<FloorPerson> v;
    for (int i = -half; i <= half; ++i) {
        for (int j = -half; j <= half; ++j) {
            v.push_back({cx + i * 2000.0, cy + j * 2000.0, 1000.0, 1000.0 + lift + ((i + j) % 3 == 0 ? 20.0 : -10.0)});
        }
    }
    return v;
}
static void test_town_floor_class() {
    std::printf("the town floor: a town stands over the baked ground of its bowl\n");
    world::TownFloor tf;
    CHECK(tf.towns() == 0 && !tf.at(0, 0, 1000), "no town: nowhere is a bowl");
    std::vector<FloorPerson> people = floor_grid(0, 0, 5060);
    for (int i = 0; i < 6; ++i) people.push_back({i * 1000.0, 30000.0, 1000.0, 8000.0});
    people.push_back({500, 0, 1000.0, 3000.0});
    people.push_back({-500, 0, 1000.0, 1100.0});
    tf.set(7, people);
    const std::optional<double> lift = tf.lift_of(7);
    CHECK(tf.towns() == 1 && lift && std::fabs(*lift - 5060.0) < 25.0,
          "the town's lift is the median of its people's heights over the ground "
          "(50.6 m)");
    CHECK(tf.floor_persons(7) == 49, "the floor is the people within 3 m of it: "
                                     "not the upper floors, the wall of the "
                                     "pit or the one on the open ground");
    const auto in = tf.at(100, 100, 1000);
    CHECK(in && in->node == 7 && std::fabs(in->lift - *lift) < 1e-9,
          "a point in the town: its floor is the ground under it plus the lift, "
          "and it names the node");
    CHECK(!tf.at(100, 100, 3000), "...but not where the ground under the point is 20 m off the people's "
                                  "(the wall of the pit)");
    CHECK(tf.at(100, 100, 1000 - 900) && tf.at(100, 100, 1000 + 900), "...a ground 9 m off is inside the relief");
    CHECK(tf.at(6000 + 7000, 0, 1000) && !tf.at(6000 + 8000, 0, 1000),
          "the town reaches 75 m from the last person of its floor");
    CHECK(!tf.at(20000, 20000, 1000) && !tf.at(0, 28000, 1000),
          "...not to a point far from its floor (the upper floors 300 m off do "
          "not make a floor)");
    tf.set(8, floor_grid(100000, 0, 100));
    CHECK(tf.towns() == 1 && !tf.at(100000, 0, 1000) && !tf.lift_of(8),
          "people on the open ground (+1 m) make no town");
    tf.set(9, floor_grid(200000, 0, 4990));
    CHECK(tf.towns() == 2 && tf.at(200000, 0, 1000) && std::fabs(tf.at(200000, 0, 1000)->lift - 4990.0) < 25.0 &&
              std::fabs(tf.at(0, 0, 1000)->lift - *lift) < 1e-9,
          "two towns: each point has its own town's lift");
    tf.set(7, floor_grid(0, 0, 100));
    CHECK(tf.towns() == 1 && !tf.at(0, 0, 1000), "the node's people replaced by ones on the open ground: its town is "
                                                 "gone");
    tf.set(9, {});
    CHECK(tf.towns() == 0, "no people: no town");
}

int main() {
    if (const int rc = init_node_pop_env(); rc != 0) return rc;
    test_workshops();
    test_town_floor_class();
    test_real_bowls();
    test_real_town_buildings();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
