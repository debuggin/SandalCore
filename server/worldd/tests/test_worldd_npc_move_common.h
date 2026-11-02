// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "nav_fixture.h"
#include "npc/area_npcs.h"
#include "protocol/schema/generated/index.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg)                       \
    do {                                       \
        if (cond) {                            \
            std::printf("  [ok ] %s\n", msg);  \
            g_pass++;                          \
        } else {                               \
            std::printf("  [FAIL] %s\n", msg); \
            g_fail++;                          \
        }                                      \
    } while (0)

inline const aoc::protocol::schema::ClassSchema &schema_named(const char *name) {
    const aoc::protocol::schema::ClassSchema *s = aoc::protocol::schema::gen::find_class_schema(name);
    if (!s) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *s;
}

inline aoc::protocol::emit::FIntrepidNetworkGUID guid(uint64_t id) {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}

inline aoc::worldd::world::WorldActor make_npc(uint64_t id, double x, double y, double z, aoc::worldd::world::ActorKind kind = aoc::worldd::world::ActorKind::Npc) {
    aoc::worldd::world::WorldActor w;
    w.id = id;
    w.kind = kind;
    w.x = x;
    w.y = y;
    w.z = z;
    aoc::protocol::repl::Actor &a = w.actor;
    a.guid = guid(id);
    a.obj.schema = &schema_named("ANPCCharacter");
    aoc::protocol::repl::RepMovement m;
    m.location = aoc::protocol::repl::quantize_location(a.obj.opts, x, y, z);
    m.rotation = aoc::protocol::repl::Rot{0, 64, 0};
    m.linear_velocity = aoc::protocol::repl::QuantVec{aoc::protocol::repl::QuantVec::Mode::Scaled, 0, 0, 0};
    a.obj.state.set(*a.obj.schema, "ReplicatedMovement", aoc::protocol::repl::Value(m));
    a.obj.state.set(*a.obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp", aoc::protocol::repl::Value::f32(0.f));
    a.obj.state.set(*a.obj.schema, "CurrentMovementInput", aoc::protocol::repl::Value(aoc::protocol::repl::PlayerMovementInput{0}));
    a.open.spawn.location = aoc::protocol::repl::quantize_vector(10, x, y, z);
    a.open.spawn.rotation = aoc::protocol::repl::Rot{0, 11000, 0};
    w.npc.name = "test npc";
    return w;
}

struct Rm {
    aoc::worldd::nav::Vec3 loc;
    aoc::worldd::nav::Vec3 vel;
    int yaw8 = 0;
    float stamp = 0;
    int input = -1;
};

inline void read_rm_loc(const aoc::protocol::repl::ObjectState &o, Rm &r) {
    const aoc::protocol::repl::Value *v = o.state.get(*o.schema, "ReplicatedMovement");
    const auto *m = v ? v->get_if<aoc::protocol::repl::RepMovement>() : nullptr;
    if (!m) {
        return;
    }
    const auto l = aoc::protocol::repl::dequantize_location(o.opts, m->location);
    const auto vel = aoc::protocol::repl::dequantize_velocity(o.opts, m->linear_velocity);
    r.loc = {l.x, l.y, l.z};
    r.vel = {vel.x, vel.y, vel.z};
    r.yaw8 = m->rotation.yaw;
}

inline Rm read_rm(const aoc::worldd::world::WorldActor &w) {
    Rm r;
    const aoc::protocol::repl::ObjectState &o = w.actor.obj;
    read_rm_loc(o, r);
    if (const aoc::protocol::repl::Value *v = o.state.get(*o.schema, "ReplicatedServerLastTransformUpdateTimeStamp")) {
        if (const auto *f = v->get_if<float>()) {
            r.stamp = *f;
        }
    }
    if (const aoc::protocol::repl::Value *v = o.state.get(*o.schema, "CurrentMovementInput")) {
        if (const auto *p = v->get_if<aoc::protocol::repl::PlayerMovementInput>()) {
            r.input = p->bits;
        }
    }
    return r;
}

inline aoc::worldd::npc::NpcMotionSetup wander_setup(double radius = 1500.0) {
    aoc::worldd::npc::NpcMotionSetup s;
    s.ambient = aoc::worldd::npc::Ambient::Wander;
    s.foot_offset = 96.0;
    s.wander_min = 200.0;
    s.wander_max = radius;
    s.walk_speed = 150.0;
    s.run_speed = 300.0;
    s.wander_delay = 0.4;
    s.wander_delay_deviation = 0.1;
    return s;
}

template <class F>
inline double run(aoc::worldd::npc::NpcWorld &w, double t0, double seconds, aoc::worldd::nav::Vec3 player_pos, F &&each) {
    double t = t0;
    const double end = t0 + seconds;
    while (t < end) {
        t += 1.0 / 15.0;
        w.note_player(1, player_pos.x, player_pos.y, t);
        w.tick(t);
        each(t);
    }
    return t;
}

inline double run_idle(aoc::worldd::npc::NpcWorld &w, double t0, double seconds) {
    double t = t0;
    const double end = t0 + seconds;
    while (t < end) {
        t += 1.0 / 15.0;
        w.tick(t);
    }
    return t;
}

inline double dist2(const aoc::worldd::world::WorldActor &a, const aoc::worldd::nav::Vec3 &b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

inline void put_snap_npc(aoc::worldd::world::ActorRegistry &reg, uint64_t id, aoc::worldd::nav::Vec3 pos_feet, bool snaps, bool flies) {
    aoc::worldd::world::WorldActor a = make_npc(id, pos_feet.x, pos_feet.y, pos_feet.z + 96.0);
    a.npc.ground_by_data = true;
    a.npc.snaps_to_ground = snaps;
    a.npc.flies = flies;
    reg.add(std::move(a));
}

struct WanderStat {
    double max_dist = 0;
    double worst_ground = 0;
    double max_speed = 0;
    double walk_time = 0;
    double idle_time = 0;
    double last_stamp = -1;
    double max_step = 0;
    double px = 1000;
    double py = 1000;
    size_t input_mismatch = 0;
    size_t legs = 0;
    size_t steps = 0;
    bool was_moving = false;
    bool stamp_back = false;
};

inline void record_wander_tick(WanderStat &st, const aoc::worldd::nav::AreaNav &n, const aoc::worldd::world::WorldActor &a) {
    st.max_dist = std::max(st.max_dist, dist2(a, {1000, 1000, 1000}));
    double tz = 0;
    n.terrain_height(a.x, a.y, &tz);
    st.worst_ground = std::max(st.worst_ground, std::fabs(a.z - 96.0 - tz));
    const double step = std::hypot(a.x - st.px, a.y - st.py);
    st.max_step = std::max(st.max_step, step);
    st.px = a.x;
    st.py = a.y;
    const Rm r = read_rm(a);
    const double sp = std::hypot(r.vel.x, r.vel.y);
    st.max_speed = std::max(st.max_speed, sp);
    const bool moving = sp > 1.0;
    if (moving) {
        st.walk_time += 1.0 / 15.0;
    } else {
        st.idle_time += 1.0 / 15.0;
    }
    if (moving && !st.was_moving) {
        ++st.legs;
    }
    st.was_moving = moving;
    const bool bad_walk = moving && r.input != 1;
    const bool bad_idle = !moving && r.input == 1 && step == 0;
    if (bad_walk || bad_idle) {
        ++st.input_mismatch;
    }
    if (r.stamp < st.last_stamp - 1e-6f) {
        st.stamp_back = true;
    }
    st.last_stamp = r.stamp;
    ++st.steps;
}

inline void record_patrol_wp(const aoc::worldd::world::WorldActor &a, const std::vector<aoc::worldd::nav::Vec3> &route, std::vector<int> &order, int &last_wp, double *pause_t) {
    for (size_t i = 0; i < route.size(); ++i) {
        if (dist2(a, route[i]) < 8.0 && static_cast<int>(i) != last_wp) {
            order.push_back(static_cast<int>(i));
            last_wp = static_cast<int>(i);
            if (pause_t) {
                *pause_t = 0;
            }
        }
    }
}

inline aoc::worldd::world::WorldActor town_person(uint64_t id, double x, double y, double feet) {
    aoc::worldd::world::WorldActor p = make_npc(id, x, y, feet + 96.0);
    p.npc.ground_by_data = true;
    return p;
}

inline aoc::worldd::nav::AreaNav &map_without_buildings() {
    static std::unique_ptr<aoc::worldd::nav::AreaNav> n = [] {
        aoc::worldd::nav::Geometry g = aoc::worldd::navfix::test_geometry();
        g.placements.erase(g.placements.begin() + 1);
        aoc::worldd::nav::BuildOptions opt;
        opt.threads = 2;
        opt.nav_margin_cm = 3000;
        aoc::worldd::nav::NavFile f;
        aoc::worldd::nav::BuildStats st;
        std::string err;
        if (!aoc::worldd::nav::build_nav(g, 7, opt, &f, &st, &err)) {
            std::printf("  [FAIL] the map does not build: %s\n", err.c_str());
            std::exit(2);
        }
        auto p = aoc::worldd::nav::AreaNav::from_file(f, &err);
        if (!p) {
            std::printf("  [FAIL] the map does not load: %s\n", err.c_str());
            std::exit(2);
        }
        return p;
    }();
    return *n;
}

inline size_t count_raised_on_floor(aoc::worldd::nav::AreaNav &map) {
    const double xs[] = {8400, 8700, 9000, 9300, 9600};
    const double ys[] = {-5600, -5300, -5000, -4700, -4400};
    aoc::worldd::world::ActorRegistry reg;
    std::vector<uint64_t> ids;
    for (int i = 0; i < 5; ++i) {
        reg.add(town_person(4000 + 2 * i, xs[i], ys[i], 1540.0));
        ids.push_back(4000 + 2 * i);
    }
    aoc::worldd::npc::NpcWorld w(reg, &map);
    aoc::worldd::npc::NpcMotionSetup stand = wander_setup();
    stand.ambient = aoc::worldd::npc::Ambient::Stationary;
    for (const uint64_t id : ids) {
        w.add(id, stand);
    }
    return w.stand_report(ids).on_floor;
}

inline bool step_on_ground(const aoc::worldd::nav::AreaNav &map, const aoc::worldd::nav::Vec3 &a, const aoc::worldd::nav::Vec3 &b) {
    const aoc::worldd::nav::Vec3 fa{a.x, a.y, a.z - 96.0};
    const aoc::worldd::nav::Vec3 fb{b.x, b.y, b.z - 96.0};
    if (map.raycast(fa, fb)) {
        return true;
    }
    return map.on_navmesh({(fa.x + fb.x) / 2, (fa.y + fb.y) / 2, (fa.z + fb.z) / 2}, 40.0, 80.0);
}

struct Track {
    aoc::worldd::nav::Vec3 last;
    bool have = false;
    size_t rays = 0;
    size_t blocked = 0;
    size_t inside = 0;
    double min_x = 1e18;
    double max_x = -1e18;
};

inline void record_walker_step(const aoc::worldd::nav::AreaNav &n, const aoc::worldd::world::WorldActor &a, Track &t) {
    const aoc::worldd::nav::Vec3 p{a.x, a.y, a.z};
    if (std::fabs(a.x + 3000.0) < 100.0 - 1.0 && std::fabs(a.y) < 2000.0) {
        ++t.inside;
    }
    t.min_x = std::min(t.min_x, a.x);
    t.max_x = std::max(t.max_x, a.x);
    if (t.have && std::hypot(p.x - t.last.x, p.y - t.last.y) > 0.5) {
        ++t.rays;
        if (!step_on_ground(n, t.last, p)) {
            ++t.blocked;
        }
    }
    t.last = p;
    t.have = true;
}

inline void check_standing_invalid(aoc::worldd::npc::NpcWorld &w, aoc::worldd::world::ActorRegistry &reg, const aoc::worldd::npc::NpcMotionSetup &s) {
    bool found = false;
    CHECK(w.add(1008, s, &found) && !found, "far outside the terrain: no ground, stands");
    CHECK(!w.add(1010, s), "a gatherable is not an NPC");
    CHECK(!w.add(9999, s), "an unknown actor is refused");
    CHECK(!w.add(1000, s), "a second add is refused");
    aoc::worldd::npc::NpcMotionSetup nan = s;
    nan.walk_speed = std::nan("");
    reg.add(make_npc(1012, 0, 0, 1000));
    CHECK(!w.add(1012, nan), "a setup with a NaN is refused");
    nan = s;
    nan.foot_offset = 1e9;
    CHECK(!w.add(1012, nan), "a half height of 1e9 is refused");
    CHECK(w.size() == 5, "five NPCs under its care");
    const uint32_t version_before = reg.find(1000)->version;
    w.tick(5000.0);
    CHECK(read_rm(*reg.find(1000)).stamp == 5000.0f && reg.find(1000)->version == version_before, "the first tick sets every NPC's timestamp to the clock without a change to send");
}

inline void check_ground_by_data_window(aoc::worldd::npc::NpcWorld &w, aoc::worldd::world::ActorRegistry &reg, const aoc::worldd::npc::NpcMotionSetup &s) {
    bool found = true;
    CHECK(w.add(1106, s, &found) && !found && std::fabs(reg.find(1106)->z - 1700.0) < 0.01, "a node's person 5 m over the only surface: the tight window does not reach it, it keeps its height (no snap down to the pit's ground)");
    CHECK(w.add(1108, s, &found) && !found && std::fabs(reg.find(1108)->z - 800.0) < 0.01, "a surface more than 150 cm over its feet is not its floor either");
    CHECK(w.add(1110, s, &found) && found && std::fabs(reg.find(1110)->z - 1096.0) < 0.5, "the open world's NPC at the roof's height: the 15 m window still stands it on the plain");
    CHECK(aoc::worldd::npc::NpcWorldConfig{}.data_ground_below_cm == 300.0 && aoc::worldd::npc::NpcWorldConfig{}.data_ground_above_cm == 150.0, "the window is 300 cm under and 150 cm over");
}

inline void check_snap_to_ground_more(aoc::worldd::npc::NpcWorld &w, aoc::worldd::world::ActorRegistry &reg, const aoc::worldd::npc::NpcMotionSetup &s) {
    bool found = false;
    CHECK(w.add(1308, s, &found) && found && std::fabs(reg.find(1308)->z - 1096.0) < 0.5, "a bird drawn 5 m under the street is lifted out of the ground onto it");
    CHECK(w.add(1310, s, &found) && !found && std::fabs(reg.find(1310)->z - 5096.0) < 0.01, "a wolf 40 m over the street, out of the open world's reach, keeps its height");
    CHECK(w.add(1312, s, &found) && found && std::fabs(reg.find(1312)->z - (1540.0 + 96.0)) < 12.0, "a wolf with a floor in the tight reach stands on that floor (the platform), not on the plain");
    CHECK(w.add(1314, s, &found) && found && std::fabs(reg.find(1314)->z - 1096.0) < 0.5, "a wolf 1 m over the street stands on it, as it did before");
    const aoc::worldd::npc::NpcWorld::StandReport r = w.stand_report({1300, 1302, 1304, 1306, 1308, 1310});
    CHECK(r.people == 6 && r.moved == 3 && r.no_floor == 3 && r.stuck == 3 && r.max_moved_cm > 1100.0, "the report counts the three that came to the street as moved (12 m at the most) and the three that kept their height as without a floor");
}

inline void test_pathfind_budget(const aoc::worldd::nav::AreaNav &n) {
    aoc::worldd::world::ActorRegistry reg;
    aoc::worldd::npc::NpcWorldConfig cfg;
    cfg.max_pathfinds_per_step = 3;
    aoc::worldd::npc::NpcWorld w(reg, &n, cfg);
    std::vector<uint64_t> wanderers;
    for (int i = 0; i < 30; ++i) {
        const uint64_t id = 2000 + 2 * static_cast<uint64_t>(i);
        reg.add(make_npc(id, 500 + 40.0 * i, 800, 1096));
        aoc::worldd::npc::NpcMotionSetup s;
        if (i < 12) {
            s.ambient = aoc::worldd::npc::Ambient::Patrol;
            s.foot_offset = 96.0;
            s.route = {{-8400, 6600, 1000}, {-8500, 6600, 1000}, {-8600, 6600, 1000}};
            s.pauses = {0, 0, 0};
            s.patrol_speed = 200.0;
        } else {
            s = wander_setup(900);
            wanderers.push_back(id);
        }
        w.add(id, s);
    }
    std::set<uint64_t> walked;
    run(w, 0, 60, {1000, 1000, 0}, [&](double) {
        for (const uint64_t id : wanderers) {
            if (std::hypot(reg.find(id)->x - (500 + 40.0 * ((id - 2000) / 2)), reg.find(id)->y - 800) > 100.0) {
                walked.insert(id);
            }
        }
    });
    const auto st = w.stats();
    CHECK(st.patrol_no_path > 0, "(control) the patrols found no path");
    CHECK(walked.size() == wanderers.size(), "every wanderer was asked and walked, whatever the patrols spent: the budget goes round");
}
