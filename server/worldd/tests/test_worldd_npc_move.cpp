// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_npc_move_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace emit = aoc::protocol::emit;
using nav::Vec3;

static void test_standing() {
    std::printf("standing\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1200));
    reg.add(make_npc(1002, 6000, 0, 800));
    reg.add(make_npc(1004, 9000, -5000, 1700));
    reg.add(make_npc(1006, 1000, 1000, 9000));
    reg.add(make_npc(1008, 500000, 0, 1000));
    reg.add(make_npc(1010, 0, 0, 1000, world::ActorKind::Gatherable));
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup s = wander_setup();
    s.ambient = npc::Ambient::Stationary;
    bool found = false;
    CHECK(w.add(1000, s, &found) && found, "an NPC on the plain is added and found its ground");
    CHECK(std::fabs(reg.find(1000)->z - 1096.0) < 0.5, "it stands on the plain: ground 1,000 + the half height 96");
    const Rm r = read_rm(*reg.find(1000));
    CHECK(std::fabs(r.loc.z - 1096.0) < 0.02 && std::fabs(r.loc.x - 1000) < 0.02 && r.vel.x == 0 && r.input == 0, "ReplicatedMovement says so too (0.01 cm), standing");
    CHECK(reg.find(1000)->version > 1, "the change counted (the view sends it)");
    CHECK(w.add(1002, s, &found) && found && std::fabs(reg.find(1002)->z - (1400.0 + 96.0)) < 0.5, "on the hill: the terrain's height");
    CHECK(w.add(1004, s, &found) && found && std::fabs(reg.find(1004)->z - (1540.0 + 96.0)) < 12.0, "on the platform: its top (the navmesh's, within 10 cm)");
    CHECK(w.add(1006, s, &found) && found && std::fabs(reg.find(1006)->z - 1096.0) < 0.5, "a height no surface is near (a placement drawn at the wrong height): the terrain's is the ground, the NPC stands on the plain");
    check_standing_invalid(w, reg, s);
}

static void test_ground_by_data() {
    std::printf("an NPC that keeps its data height (a city node's, in a bowl)\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    world::WorldActor bowl = make_npc(1100, 1000, 1000, 9000);
    bowl.npc.ground_by_data = true;
    world::WorldActor reach = make_npc(1102, 1000, 1000, 1200);
    reach.npc.ground_by_data = true;
    reg.add(std::move(bowl));
    reg.add(std::move(reach));
    reg.add(make_npc(1104, 1000, 1000, 9000));
    world::WorldActor high = make_npc(1106, 1000, 1000, 1700);
    high.npc.ground_by_data = true;
    world::WorldActor low = make_npc(1108, 1000, 1000, 800);
    low.npc.ground_by_data = true;
    reg.add(std::move(high));
    reg.add(std::move(low));
    reg.add(make_npc(1110, 1000, 1000, 1700));
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup s = wander_setup();
    bool found = true;
    CHECK(w.add(1100, s, &found) && !found, "out of reach of every surface: no ground was found for it");
    CHECK(std::fabs(reg.find(1100)->z - 9000.0) < 0.01 && std::fabs(read_rm(*reg.find(1100)).loc.z - 9000.0) < 0.02 && w.stands(1100), "...it keeps the height the data gave it (not the terrain's 1,096), here and in the replicated movement, and it stands (a wanderer has no navmesh to wander on)");
    CHECK(w.add(1102, s, &found) && found && std::fabs(reg.find(1102)->z - 1096.0) < 0.5, "a surface within reach of its height: it stands on it as any NPC does");
    CHECK(w.add(1104, s, &found) && found && std::fabs(reg.find(1104)->z - 1096.0) < 0.5, "the open world's NPC (no flag) at the first one's height: still moved to the terrain");
    check_ground_by_data_window(w, reg, s);
}

static void test_snap_to_ground() {
    std::printf("a city node's creature whose data says SnapToGround\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    put_snap_npc(reg, 1300, {1000, 1000, 2200}, true, false);
    put_snap_npc(reg, 1302, {1000, 1000, 500}, true, false);
    put_snap_npc(reg, 1304, {1000, 1000, 2200}, false, false);
    put_snap_npc(reg, 1306, {1000, 1000, 2200}, true, true);
    put_snap_npc(reg, 1308, {1000, 1000, 500}, true, true);
    put_snap_npc(reg, 1310, {1000, 1000, 5000}, true, false);
    put_snap_npc(reg, 1312, {9000, -5000, 1500}, true, false);
    put_snap_npc(reg, 1314, {1000, 1000, 1100}, true, false);
    npc::NpcWorld w(reg, &n);
    const npc::NpcMotionSetup s = wander_setup();
    bool found = false;
    CHECK(w.add(1300, s, &found) && found && std::fabs(reg.find(1300)->z - 1096.0) < 0.5, "a wolf the data draws 12 m over the street stands on the street");
    CHECK(std::fabs(read_rm(*reg.find(1300)).loc.z - 1096.0) < 0.02 && !w.stands(1300), "...in the replicated movement too, and it is no longer stuck: its life (a wander) runs");
    CHECK(w.add(1302, s, &found) && found && std::fabs(reg.find(1302)->z - 1096.0) < 0.5, "a wolf drawn 5 m under the street comes up onto it");
    CHECK(w.add(1304, s, &found) && !found && std::fabs(reg.find(1304)->z - 2296.0) < 0.01 && w.stands(1304), "a guard on a tower, 12 m up, stays where the data put it");
    CHECK(w.add(1306, s, &found) && !found && std::fabs(reg.find(1306)->z - 2296.0) < 0.01, "a bird 12 m over the street keeps its height");
    check_snap_to_ground_more(w, reg, s);
}

static void test_dormant() {
    std::printf("dormant and awake\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld w(reg, &n);
    CHECK(w.add(1000, wander_setup()), "a wanderer is added");
    const double x0 = reg.find(1000)->x;
    const double y0 = reg.find(1000)->y;
    const uint32_t v0 = reg.find(1000)->version;
    double t = run_idle(w, 0, 120);
    CHECK(reg.find(1000)->x == x0 && reg.find(1000)->y == y0 && reg.find(1000)->version == v0, "no player anywhere: 120 s later it has not moved and nothing was written");
    t = run(w, t, 60, {100000, 100000, 0}, [](double) {
    });
    CHECK(reg.find(1000)->x == x0 && reg.find(1000)->y == y0, "a player 1.4 km away: still still");
    size_t moved = 0;
    t = run(w, t, 60, {1000, 9000, 0}, [&](double) {
        moved += (reg.find(1000)->x != x0 || reg.find(1000)->y != y0) ? 1 : 0;
    });
    CHECK(moved > 100, "a player 80 m away wakes it: it walks");
    t = run_idle(w, t, 6.0);
    const Rm r = read_rm(*reg.find(1000));
    CHECK(r.vel.x == 0 && r.vel.y == 0 && r.input == 0, "when the last player is gone it is still, and its last write says zero velocity and no input");
    const double px = reg.find(1000)->x;
    const double py = reg.find(1000)->y;
    t = run_idle(w, t, 30);
    CHECK(reg.find(1000)->x == px && reg.find(1000)->y == py, "and stays where it stopped");
    size_t moved_again = 0;
    run(w, t, 60, {1000, 9000, 0}, [&](double) {
        moved_again += (reg.find(1000)->x != px || reg.find(1000)->y != py) ? 1 : 0;
    });
    CHECK(moved_again > 50, "a player back near wakes it again");
}

static void test_wander() {
    std::printf("wander\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld w(reg, &n);
    w.add(1000, wander_setup(1500.0));
    WanderStat st;
    run(w, 0, 300, {1000, 7000, 0}, [&](double) {
        record_wander_tick(st, n, *reg.find(1000));
    });
    CHECK(st.legs >= 8, "it walked at least eight legs in five minutes");
    CHECK(st.max_dist <= 1500.0 + 120.0 && st.max_dist > 600.0, "it kept within its radius of home (plus the navmesh's snap) and used much of it");
    CHECK(st.worst_ground < 1.5, "its feet were on the ground all the way: the origin was always the terrain's height + 96 (within 1.5 cm)");
    CHECK(st.max_speed > 140.0 && st.max_speed < 160.0, "it walked at its speed (150 cm/s, whole cm/s on the wire)");
    CHECK(st.max_step < 150.0 / 15.0 * 1.2, "and a step of one tick was at most its speed's (10 cm)");
    CHECK(st.idle_time > 20.0 && st.walk_time > 60.0, "and idled between legs");
    CHECK(st.input_mismatch == 0, "CurrentMovementInput was 1 while it walked");
    CHECK(!st.stamp_back && st.last_stamp > 290.0, "the timestamp only rose, up to the clock's");
    const Rm r = read_rm(*reg.find(1000));
    CHECK(std::fabs(r.loc.x - reg.find(1000)->x) < 0.01 && std::fabs(r.loc.y - reg.find(1000)->y) < 0.01 && std::fabs(r.loc.z - reg.find(1000)->z) < 0.01, "ReplicatedMovement's location is the registry's to 0.01 cm");
    CHECK(st.steps >= 4499 && st.steps <= 4501, "300 s of 15 Hz steps");
}

static void test_wander_determinism() {
    std::printf("determinism\n");
    auto &n = navfix::shared_nav();
    std::vector<std::vector<double>> traces;
    for (int run_no = 0; run_no < 2; ++run_no) {
        world::ActorRegistry reg;
        reg.add(make_npc(1000, 1000, 1000, 1096));
        reg.add(make_npc(1002, -1000, 3000, 1096));
        npc::NpcWorld w(reg, &n);
        w.add(1000, wander_setup());
        w.add(1002, wander_setup(800));
        std::vector<double> trace;
        run(w, 0, 90, {0, 0, 0}, [&](double) {
            trace.push_back(reg.find(1000)->x);
            trace.push_back(reg.find(1002)->y);
        });
        traces.push_back(trace);
    }
    CHECK(traces[0] == traces[1] && traces[0].size() == 2700, "the same world gives the same walks (a per-NPC random stream)");
}

static void test_budget_and_legs() {
    std::printf("the path-finding budget and the length of a leg\n");
    auto &n = navfix::shared_nav();
    test_pathfind_budget(n);
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup s = wander_setup(2000.0);
    s.wander_min = 100.0;
    s.wander_delay = 0.2;
    s.wander_delay_deviation = 0.05;
    w.add(1000, s);
    std::vector<double> legs;
    double max_home = 0;
    double sx = 0;
    double sy = 0;
    bool was_moving = false;
    run(w, 0, 1500, {1000, 1000, 0}, [&](double) {
        const world::WorldActor &a = *reg.find(1000);
        const Rm r = read_rm(a);
        const bool moving = std::hypot(r.vel.x, r.vel.y) > 1.0;
        if (moving && !was_moving) {
            sx = a.x;
            sy = a.y;
        }
        if (!moving && was_moving) {
            legs.push_back(std::hypot(a.x - sx, a.y - sy));
        }
        was_moving = moving;
        max_home = std::max(max_home, dist2(a, {1000, 1000, 1000}));
    });
    std::sort(legs.begin(), legs.end());
    const double med = legs.empty() ? 0 : legs[legs.size() / 2];
    const double p90 = legs.empty() ? 0 : legs[legs.size() * 9 / 10];
    CHECK(legs.size() > 80, "it walks leg after leg");
    CHECK(med > 150 && med < 450 && p90 > 450 && p90 < 1300, "a leg is a few metres, as retail's: a median of about 250 cm, the 90th percentile of about 740");
    CHECK(max_home <= 2000.0 + 150.0, "and it never leaves its radius: a leg that would turns back towards home");
}

static void test_patrol_loop(const nav::AreaNav &n, npc::NpcMotionSetup s) {
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 0, 5000, 1096));
    npc::NpcWorld w(reg, &n);
    s.loop = true;
    w.add(1000, s);
    std::vector<int> order;
    int last_wp = -1;
    double pause_t = 0;
    double pause_max = 0;
    run(w, 0, 140, {0, 5000, 0}, [&](double) {
        const world::WorldActor &a = *reg.find(1000);
        record_patrol_wp(a, s.route, order, last_wp, &pause_t);
        const Rm r = read_rm(a);
        if (last_wp == 1 && std::hypot(r.vel.x, r.vel.y) < 1.0 && dist2(a, s.route[1]) < 8.0) {
            pause_t += 1.0 / 15.0;
            pause_max = std::max(pause_max, pause_t);
        }
    });
    std::string seq;
    for (int i : order) {
        seq += std::to_string(i);
    }
    CHECK(order.size() >= 7 && seq.substr(0, 7) == "0120120", "a looping route goes round: 0 1 2 0 1 2 0 ...");
    CHECK(pause_max > 2.7 && pause_max < 3.6, "it waited about the 3 s the route says at the second waypoint");
}

static void test_patrol() {
    std::printf("patrol\n");
    auto &n = navfix::shared_nav();
    npc::NpcMotionSetup s;
    s.ambient = npc::Ambient::Patrol;
    s.foot_offset = 96;
    s.patrol_speed = 200;
    s.run_speed = 400;
    s.route = {{0, 5000, 1000}, {3000, 5000, 1000}, {3000, 8000, 1000}};
    s.pauses = {0.0, 3.0, 1.0};
    test_patrol_loop(n, s);
    {
        world::ActorRegistry reg;
        reg.add(make_npc(1000, 0, 5000, 1096));
        npc::NpcWorld w(reg, &n);
        s.loop = false;
        w.add(1000, s);
        std::vector<int> order;
        int last_wp = -1;
        run(w, 0, 200, {0, 5000, 0}, [&](double) {
            record_patrol_wp(*reg.find(1000), s.route, order, last_wp, nullptr);
        });
        std::string seq;
        for (int i : order) {
            seq += std::to_string(i);
        }
        CHECK(seq.size() >= 6 && seq.substr(0, 6) == "012101", "a route that does not loop goes there and back: 0 1 2 1 0 1 ...");
    }
    {
        world::ActorRegistry reg;
        reg.add(make_npc(1000, 0, 5000, 1096));
        npc::NpcWorld w(reg, &n);
        npc::NpcMotionSetup bad = s;
        bad.loop = true;
        bad.route = {{0, 5000, 1000}, {-8400, 6600, 1000}, {3000, 5000, 1000}};
        w.add(1000, bad);
        std::vector<int> order;
        int last_wp = -1;
        run(w, 0, 120, {0, 5000, 0}, [&](double) {
            record_patrol_wp(*reg.find(1000), bad.route, order, last_wp, nullptr);
        });
        const bool visited_pen = std::find(order.begin(), order.end(), 1) != order.end();
        CHECK(!visited_pen && order.size() >= 4, "a waypoint in the closed pen is never reached: the patrol goes on between the others");
        CHECK(w.stats().no_path > 0, "and the failures are counted");
    }
}

static void check_service_refusals(npc::NpcWorld &w, world::ActorRegistry &reg, double &t) {
    const double x1 = reg.find(1000)->x;
    CHECK(!w.order_move(1000, {-8400, 6600, 1000}, 300), "a point in the closed pen: refused");
    CHECK(!w.order_move(1000, {900000, 0, 0}, 300), "a point far outside the navmesh: refused");
    CHECK(!w.order_move(1000, {std::nan(""), 0, 0}, 300), "a NaN destination: refused");
    CHECK(!w.order_move(1000, {0, 0, 1000}, 0) && !w.order_move(1000, {0, 0, 1000}, -5) && !w.order_move(1000, {0, 0, 1000}, std::nan("")), "a speed of zero, negative or NaN: refused");
    run(w, t, 5, {1000, 1000, 0}, [](double) {
    });
    CHECK(reg.find(1000)->x == x1, "nothing moved");
    CHECK(!w.order_move(7777, {0, 0, 1000}, 300) && !w.release(7777) && !w.order_return_home(7777), "orders to an unknown NPC are refused");
    CHECK(!w.order_move(1002, {0, 0, 1000}, 300), "an NPC inside the closed pen cannot be sent out: no path");
    CHECK(w.order_move(1002, {-8000, 6800, 1000}, 300), "but it can walk inside the pen");
    t = run(w, t, 10, {-8400, 6600, 0}, [](double) {
    });
    CHECK(dist2(*reg.find(1002), {-8000, 6800, 0}) < 150.0 || !w.mover(1002)->moving(), "and did");
    CHECK(w.release(1002) && !w.release(1002), "released once");
}

static void check_service_wanderer_home(npc::NpcWorld &w, world::ActorRegistry &reg, double &t) {
    reg.add(make_npc(1004, 1000, -3000, 1096));
    npc::NpcMotionSetup ws = wander_setup(500);
    ws.run_speed = 450;
    w.add(1004, ws);
    const auto returns0 = w.stats().returns_home;
    CHECK(w.order_move(1004, {-1000, 3000, 1000}, 600), "a wanderer is ordered 66 m from home");
    t = run(w, t, 30, {1000, -3000, 0}, [](double) {
    });
    CHECK(dist2(*reg.find(1004), {-1000, 3000, 0}) < 5.0, "it got there");
    CHECK(w.release(1004), "released");
    double max_speed = 0;
    bool home_again = false;
    t = run(w, t, 60, {1000, -3000, 0}, [&](double) {
        const Rm r = read_rm(*reg.find(1004));
        max_speed = std::max(max_speed, std::hypot(r.vel.x, r.vel.y));
        if (dist2(*reg.find(1004), {1000, -3000, 0}) < 700.0) {
            home_again = true;
        }
    });
    CHECK(w.stats().returns_home == returns0 + 1, "it began walking home on its own (one return)");
    CHECK(max_speed > 440 && max_speed < 460 && home_again, "at its run speed, and it is within its radius of home again");
    const double hx = reg.find(1004)->x;
    const double hy = reg.find(1004)->y;
    CHECK(w.set_home(1004, {-8400, 6600, 1000}) && w.home_of(1004).x == -8400, "a home is re-set");
    CHECK(!w.set_home(1004, {std::nan(""), 0, 0}) && !w.set_home(7777, {0, 0, 0}), "a home that is not a number, or of an unknown NPC, is refused");
    CHECK(!w.order_return_home(1004), "an unreachable home: the order is refused");
    run(w, t, 5, {1000, -3000, 0}, [](double) {
    });
    CHECK(std::hypot(reg.find(1004)->x - hx, reg.find(1004)->y - hy) < 1500.0, "and the NPC did not go anywhere in particular (its wander is about the old place only until it is far: it walks the radius it has)");
    w.set_home(1004, {hx, hy, 1000});
}

static void test_service() {
    std::printf("the service (return home, orders)\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    reg.add(make_npc(1002, -8400, 6600, 1096));
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup s = wander_setup(1500);
    s.ambient = npc::Ambient::Stationary;
    s.run_speed = 400;
    w.add(1000, s);
    w.add(1002, s);
    CHECK(w.order_move(1000, {-5000, 0, 1000}, 500), "an order to walk to (-5000, 0) round the wall is accepted");
    CHECK(w.is_ordered(1000) && w.mover(1000)->moving(), "it is ordered and on its way");
    double max_speed = 0;
    double t = run(w, 0, 40, {1000, 1000, 0}, [&](double) {
        const Rm r = read_rm(*reg.find(1000));
        max_speed = std::max(max_speed, std::hypot(r.vel.x, r.vel.y));
    });
    CHECK(max_speed > 480 && max_speed < 520, "at the speed ordered (500 cm/s)");
    CHECK(dist2(*reg.find(1000), {-5000, 0, 0}) < 5.0 && !w.mover(1000)->moving(), "and it stood there");
    CHECK(w.is_ordered(1000), "it stays ordered (the AI's) until released");
    CHECK(w.order_return_home(1000), "an order home is accepted (it is across the wall: a path round it)");
    double max_home_speed = 0;
    t = run(w, t, 60, {1000, 1000, 0}, [&](double) {
        const Rm r = read_rm(*reg.find(1000));
        max_home_speed = std::max(max_home_speed, std::hypot(r.vel.x, r.vel.y));
    });
    CHECK(max_home_speed > 390 && max_home_speed < 410, "it ran at its run speed (400 cm/s)");
    CHECK(dist2(*reg.find(1000), {1000, 1000, 0}) < 5.0 && !w.is_ordered(1000), "it is home, and its own behaviour (standing) has it again");
    check_service_refusals(w, reg, t);
    check_service_wanderer_home(w, reg, t);
    CHECK(w.order_move(1000, {-1000, 3000, 1000}, 400, 300.0), "a walk that stops 300 cm short of its point");
    t = run(w, t, 30, {1000, 1000, 0}, [](double) {
    });
    const double d = dist2(*reg.find(1000), {-1000, 3000, 0});
    CHECK(d > 250 && d < 340, "ends about 300 cm short");
}

static void check_mover_bounds(npc::Mover &m) {
    CHECK(!m.move_to({-8400, 6600, 1000}, 200.0) && m.status() == npc::Mover::Status::Blocked, "a point in the pen: Blocked");
    CHECK(m.move_to({-8400, 6600, 1000}, 200.0, 0, 0, true) && m.moving(), "or, partial allowed, a walk to the closest point it can reach");
    npc::Mover none;
    none.init(nullptr, 96.0, 360.0);
    none.place(0, 0, 1000, 0);
    CHECK(!none.move_to({100, 0, 1000}, 100.0) && none.status() == npc::Mover::Status::Blocked, "without a navmesh every order is Blocked");
    CHECK(!m.follow({}, 100.0) && !m.follow({{1, 2, 3}}, 0.0) && !m.follow({{std::nan(""), 0, 0}}, 100.0), "an empty path, a zero speed and a NaN point are refused");
    m.place(0, 0, 1000, 0);
    CHECK(m.follow({{0, 0, 1000}, {300, 0, 1000}}, 100.0) && m.advance(0.0) == npc::Mover::Status::Moving && m.advance(std::nan("")) == npc::Mover::Status::Moving, "a zero or NaN step does nothing");
    CHECK(m.advance(1.0) == npc::Mover::Status::Moving && std::fabs(m.ground().x - 100.0) < 1e-6, "a step of a second at 100 cm/s: 100 cm");
    CHECK(m.advance(10.0) == npc::Mover::Status::Arrived && std::fabs(m.ground().x - 300.0) < 1e-6, "a long step ends at the end of the path");
}

static void test_mover() {
    std::printf("the mover\n");
    auto &n = navfix::shared_nav();
    npc::Mover m;
    m.init(&n, 96.0, 360.0);
    m.place(-5000, 0, 1000, 0);
    CHECK(m.status() == npc::Mover::Status::Idle && !m.moving(), "idle after place");
    CHECK(m.move_to({-1000, 0, 1000}, 200.0) && m.moving() && m.remaining() > 5000, "a walk round the wall is longer than the straight 4,000 cm");
    size_t n_steps = 0;
    double total = 0;
    Vec3 prev = m.ground();
    while (m.moving() && n_steps < 100000) {
        m.advance(1.0 / 15.0);
        total += std::hypot(m.ground().x - prev.x, m.ground().y - prev.y);
        prev = m.ground();
        ++n_steps;
    }
    CHECK(m.status() == npc::Mover::Status::Arrived && std::hypot(m.ground().x + 1000, m.ground().y) < 1.0, "it arrives at the point");
    CHECK(std::fabs(total / (n_steps / 15.0) - 200.0) < 6.0, "at the speed asked, on average (the last step is short)");
    CHECK(m.velocity().x == 0 && m.velocity().y == 0 && m.speed() == 0, "and has no velocity once there");
    check_mover_bounds(m);
}

static void check_motion_table_blueprint(const std::string &head) {
    npc::MotionTable c;
    std::string err;
    const std::string row = "/Game/W.Default__W_C\tBeast_C,NPCPawn_C\t1.09\t6064632310208593979,0,42\t88\t34\t600\t600\t2048\t360\tEMovementMode::MOVE_NavWalking\t0\tAnimal,Hunted\n";
    CHECK(c.parse(head + row, err), "a line with parents, a scale, alignments and tags parses");
    const auto m = c.find("/Game/W.Default__W_C");
    const bool ok = m && m->parents == "Beast_C,NPCPawn_C" && m->default_scale == 1.09 && m->alignments.size() == 2 && m->alignments[0] == 6064632310208593979LL && m->alignments[1] == 42 && m->tags == "Animal,Hunted" && m->move_mode == "EMovementMode::MOVE_NavWalking";
    CHECK(ok, "its facts are read (an unset alignment slot, 0, is dropped)");
    CHECK(!c.parse(head + "/A\tNPCPawn_C\t0\t\t88\t34\t600\t600\t2048\t360\tx\t0\t\n", err) && !c.parse(head + "/A\tNPCPawn_C\t1\tnot-a-guid\t88\t34\t600\t600\t2048\t360\tx\t0\t\n", err), "a zero scale and an alignment that is no guid are refused");
}

static void check_hostile_world() {
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld w(reg, &n);
    w.add(1000, wander_setup());
    w.note_player(1, std::nan(""), 0, 1.0);
    w.note_player(2, 1e300, -1e300, 1.0);
    w.tick(1.0);
    w.tick(0.5);
    w.tick(std::nan(""));
    CHECK(w.now() == 1.0, "a NaN or earlier clock is ignored");
    const auto steps0 = w.stats().steps;
    w.note_player(1, 1000, 1000, 2000.0);
    w.tick(2000.0);
    CHECK(w.stats().steps - steps0 <= static_cast<uint64_t>(w.config().max_catch_up_steps), "a clock that jumped ahead is not made up for beyond the catch-up cap");
    reg.add(make_npc(1006, 1500, 1500, 1096));
    w.add(1006, wander_setup());
    double tt = 3000.0;
    for (int i = 0; i < 300 && w.has(1006); ++i) {
        tt += 1.0 / 15.0;
        w.note_player(1, 1500, 1500, tt);
        w.tick(tt);
        if (i == 150) {
            reg.remove(1006);
        }
    }
    CHECK(!w.has(1006) && w.mover(1006) == nullptr && !w.order_move(1006, {0, 0, 1000}, 100), "an NPC the registry removes is dropped (no dangling state) and refuses orders");
    CHECK(!w.remove(1006), "and removing it again says so");
    world::ActorRegistry reg2;
    reg2.add(make_npc(1000, 1000, 1000, 1096));
    npc::NpcWorld none(reg2, nullptr);
    CHECK(!none.add(1000, wander_setup()) && none.size() == 0, "no navmesh: add refuses");
    none.note_player(1, 1000, 1000, 1.0);
    none.tick(1.0);
    CHECK(reg2.find(1000)->x == 1000 && reg2.find(1000)->z == 1096 && reg2.find(1000)->version == 1, "and the NPC stands where it was, unchanged");
}

static void test_table_and_hostile() {
    std::printf("the motion table and hostile inputs\n");
    npc::MotionTable t;
    std::string err;
    const std::string head = "archetype\tparents\tdefault_scale\tstarting_alignments\thalf_height\tradius\tmax_walk_speed\tmax_run_speed\tmax_acceleration\trotation_rate_yaw\tmove_mode\tfly_speed\ttags\n";
    const std::string row_a = "/Game/A.Default__A_C\tNPCPawn_C\t1\t\t88\t34\t600\t800\t2048\t360\tEMovementMode::MOVE_NavWalking\t0\t\n";
    const std::string row_b = "/Game/B.Default__B_C\tNPCPawn_C\t1\t\t66.5\t55\t300\t300\t1024\t720\tEMovementMode::MOVE_NavWalking\t0\t\n";
    CHECK(t.parse(head + row_a + row_b, err), "a table parses");
    CHECK(t.size() == 2 && t.find("/Game/A.Default__A_C")->max_run_speed == 800 && t.get("/Game/B.Default__B_C").half_height == 66.5, "its lines are found");
    CHECK(!t.find("/Game/Z.Default__Z_C") && t.get("/Game/Z.Default__Z_C").half_height == 88.0, "an archetype with no line: the NPCPawn defaults");
    CHECK(!t.parse("", err) && !t.parse("a\tb\n", err) && !t.parse(head + "/A\t1\t2\n", err), "an empty table, a wrong header and a short line are refused");
    const std::string neg_row = "/A\tNPCPawn_C\t1\t\t-1\t1\t1\t1\t1\t1\tEMovementMode::MOVE_NavWalking\t0\t\n";
    const std::string nan_row = "/A\tNPCPawn_C\t1\t\tnan\t1\t1\t1\t1\t1\tEMovementMode::MOVE_NavWalking\t0\t\n";
    CHECK(!t.parse(head + neg_row, err) && !t.parse(head + nan_row, err), "a negative or NaN number is refused");
    CHECK(t.size() == 2, "a refused parse leaves the table as it was");
    check_motion_table_blueprint(head);
    CHECK(!t.load("no/such/npc_motion.tsv", err) && err.find("cannot open") != std::string::npos, "a missing file: 'cannot open'");
    check_hostile_world();
}

static void test_cost() {
    std::printf("the cost of 900 NPCs\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    std::vector<uint64_t> ids;
    for (int i = 0; i < 900; ++i) {
        const uint64_t id = 2000 + static_cast<uint64_t>(i) * 2;
        reg.add(make_npc(id, -12000 + (i % 30) * 800.0, -12000 + (i / 30) * 800.0, 1096));
        ids.push_back(id);
    }
    npc::NpcWorld w(reg, &n);
    size_t added = 0;
    for (const uint64_t id : ids) {
        npc::NpcMotionSetup s = wander_setup(1200);
        s.wander_max = 2000;
        if (w.add(id, s)) {
            ++added;
        }
    }
    CHECK(added > 800, "most of 900 NPCs found their ground on the fixture's plain (the rest stand on the wall, in the pen or off the area)");
    double t = 0;
    double total_us = 0;
    double max_us = 0;
    size_t ticks = 0;
    for (int i = 0; i < 900; ++i) {
        t += 1.0 / 15.0;
        w.note_player(1, 0, 0, t);
        const auto t0 = std::chrono::steady_clock::now();
        w.tick(t);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        total_us += us;
        max_us = std::max(max_us, us);
        ++ticks;
    }
    const auto st = w.stats();
    CHECK(total_us / static_cast<double>(ticks) < 2000.0, "a step takes under 2 ms on average with the area's crowd awake");
    CHECK(st.active_now > 100 && st.moves_started > 100, "the NPCs around the player were awake and walking");
}

static void check_town_report_and_walker(npc::NpcWorld &w, world::ActorRegistry &reg, const nav::AreaNav &n, const std::vector<uint64_t> &ids) {
    const npc::NpcWorld::StandReport r = w.stand_report(ids);
    CHECK(r.people == 8 && r.waiting == 0, "the report holds the eight people, none waiting for its ground");
    CHECK(r.on_floor == 5 && r.moved == 1 && r.no_floor == 2 && r.blocked == 1 && r.stuck == 0, "five on a floor within 30 cm, one 80 cm from its floor, two without a floor: the one in the air has no ground near, the one in the wall has the wall's top within 3 m over its feet; none of them would walk");
    CHECK(r.max_moved_cm > 60.0 && r.max_moved_cm < 100.0, "the farthest move to a floor is the 80 cm one");
    CHECK(w.stand_report({9999, 3000}).people == 1, "a person the world does not hold is not counted");
    reg.add(town_person(3100, -2880, 0, 1000));
    npc::NpcWorld w2(reg, &n);
    npc::NpcMotionSetup walker = wander_setup(800);
    bool grounded = false;
    CHECK(w2.add(3100, walker, &grounded) && grounded && !w2.stands(3100), "a wanderer posted 20 cm from the wall has a ground and a life");
    const Vec3 start{reg.find(3100)->x, reg.find(3100)->y, 0};
    double far = 0;
    run(w2, 0, 60, {-2880, 0, 0}, [&](double) {
        far = std::max(far, dist2(*reg.find(3100), start));
    });
    CHECK(far > 150.0, "...and it walks away from the wall");
}

static void test_near_walls_and_report() {
    std::printf("a city node's person near a wall: the nearest polygon's floor; the report of how a town's people stand\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(town_person(3000, -2880, 0, 1000));
    reg.add(town_person(3002, -2700, 0, 1000));
    reg.add(town_person(3004, 9000, -5000, 1540));
    reg.add(town_person(3006, 9000, -5000, 1000));
    reg.add(town_person(3008, -2700, 200, 1080));
    reg.add(town_person(3010, -3000, 0, 1320));
    reg.add(town_person(3012, 1000, 1000, 5000));
    reg.add(town_person(3014, -2780, 0, 1000));
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup stand = wander_setup();
    stand.ambient = npc::Ambient::Stationary;
    std::vector<uint64_t> ids;
    bool found = false;
    for (const uint64_t id : {3000, 3002, 3004, 3006, 3008, 3010, 3012, 3014}) {
        CHECK(w.add(id, stand, &found), "(added)");
        ids.push_back(id);
    }
    const world::WorldActor *near_wall = reg.find(3000);
    CHECK(near_wall && std::fabs(near_wall->z - (1000.0 + 96.0)) < 15.0 && std::fabs(near_wall->x - (-2880.0)) < 1e-6, "20 cm from the wall: it stays at its place and takes the floor of the nearest polygon (the plain), not none");
    CHECK(std::fabs(reg.find(3004)->z - (1540.0 + 96.0)) < 15.0 && std::fabs(reg.find(3006)->z - (1000.0 + 96.0)) < 15.0, "on the platform and under it: each on its own floor");
    CHECK(std::fabs(reg.find(3008)->z - (1000.0 + 96.0)) < 15.0, "80 cm over the plain: put on it");
    CHECK(std::fabs(reg.find(3010)->z - (1320.0 + 96.0)) < 1e-6 && std::fabs(reg.find(3012)->z - (5000.0 + 96.0)) < 1e-6, "in the wall and 40 m up: no floor, they keep the data's height");
    check_town_report_and_walker(w, reg, n, ids);
}

static void test_raised_people_before_and_after() {
    std::printf("raised standing people: on a floor with the buildings in the map, in the air without them\n");
    const size_t before = count_raised_on_floor(map_without_buildings());
    const size_t after = count_raised_on_floor(navfix::shared_nav());
    CHECK(before == 0 && after == 5, "five people on a platform 5 m over the plain: none has a floor in the map without it, all five have one with it");
}

static void test_walkers_do_not_cross_walls() {
    std::printf("walkers and a wall: a ray along every step stays on the walkable ground, nobody is inside the wall\n");
    auto &n = navfix::shared_nav();
    world::ActorRegistry reg;
    reg.add(make_npc(5000, -3600, 0, 1096));
    reg.add(make_npc(5002, -2500, 100, 1096));
    reg.add(make_npc(5004, -3500, -600, 1096));
    for (const uint64_t id : {5000, 5002, 5004}) {
        reg.find(id)->npc.ground_by_data = true;
    }
    npc::NpcWorld w(reg, &n);
    npc::NpcMotionSetup patrol;
    patrol.ambient = npc::Ambient::Patrol;
    patrol.foot_offset = 96;
    patrol.patrol_speed = 240;
    patrol.run_speed = 400;
    patrol.route = {{-3600, 0, 1000}, {-2500, 0, 1000}};
    patrol.loop = false;
    patrol.start_closest = true;
    CHECK(w.add(5000, patrol), "(the guard is added)");
    npc::NpcMotionSetup wander = wander_setup(1200);
    CHECK(w.add(5002, wander) && w.add(5004, wander), "(two wanderers beside the wall)");
    std::map<uint64_t, Track> tracks;
    run(w, 0, 300, {-3000, 0, 0}, [&](double) {
        for (const uint64_t id : {5000, 5002, 5004}) {
            record_walker_step(n, *reg.find(id), tracks[id]);
        }
    });
    for (const uint64_t id : {5000, 5002, 5004}) {
        const Track &t = tracks[id];
        CHECK(t.inside == 0 && t.blocked == 0, "no step leaves the walkable ground and nobody stands in the wall");
    }
    CHECK(tracks[5000].rays > 1000 && tracks[5000].min_x < -3400.0 && tracks[5000].max_x > -2600.0, "the guard walked from one side of the wall to the other (round it)");
    CHECK(tracks[5002].rays > 100 && tracks[5004].rays > 100, "and the wanderers walked");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto t0 = std::chrono::steady_clock::now();
    test_standing();
    test_ground_by_data();
    test_snap_to_ground();
    test_dormant();
    test_wander();
    test_wander_determinism();
    test_budget_and_legs();
    test_patrol();
    test_service();
    test_mover();
    test_table_and_hostile();
    test_cost();
    test_near_walls_and_report();
    test_raised_people_before_and_after();
    test_walkers_do_not_cross_walls();
    std::printf("test_worldd_npc_move: %d passed, %d failed (%.1f s)\n", g_pass, g_fail, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return g_fail == 0 ? 0 : 1;
}
