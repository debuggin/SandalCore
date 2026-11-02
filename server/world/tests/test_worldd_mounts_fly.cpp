// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
//
// test_worldd_mounts_fly.cpp: flying and swimming in the move checker (MoveLimits::air, Ride::limits)
#include "test_worldd_mounts_fly_common.h"

#include <set>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace cmf = capture_mount_fly;

static void test_the_law_cruise_and_margin(const Rig &rig) {
    {
        Flight f(rig, 0, 0, 10000);
        f.run(1290.0, 0, 0, 10.0);
        CHECK(f.refused == 0, "10 s of level flight at the cruise (1,290 cm/s) passes");
    }
    {
        Flight f(rig, 0, 0, 10000);
        f.run(1290.0 * 1.09, 0, 0, 10.0);
        CHECK(f.refused == 0, "and at 1.09 x (the margin is 10 %)");
        Flight g(rig, 0, 0, 10000);
        size_t flagged = 0;
        for (double t = 0; t < 10.0; t += 0.07) {
            g.step(1290.0 * 1.25 * 0.07, 0, 0.07, repl::kMoveFlying, 0);
            if (g.last_verdict.borderline) {
                ++flagged;
            }
        }
        CHECK(g.refused == 0 && flagged > 100, "at 1.25 x the cruise nothing is corrected, every move is flagged 'would refuse' (the law's numbers are for tuning)");
        Flight h(rig, 0, 0, 10000);
        h.run(1290.0 * 1.8, 0, 0, 10.0);
        std::printf("    (1.8 x for 10 s: %zu refused, %zu accepted, average %.0f cm/s)\n", h.refused, h.accepted, h.average());
        CHECK(h.refused >= 10 && h.average() < 1290.0 * 1.5 + 60.0, "at 1.8 x it is grossly past the physics: refused, and the client gets no further than 1.5 x the cruise");
    }
}

static void test_the_law_dive_and_pitch(const Rig &rig) {
    {
        Flight f(rig, 0, 0, 100000);
        double speed = 1290.0;
        for (int i = 0; i < 40; ++i) {
            speed = std::min(3671.0, speed + 1600.0 * 0.07);
            f.step(speed * 0.07 * std::cos(0.45), -speed * 0.07 * std::sin(0.45), 0.07, repl::kMoveFlying, -34.5);
        }
        CHECK(f.refused == 0 && speed > 3670.0, "a dive at 34.5 degrees down gaining 1,600 cm/s^2 to the 3,671 ceiling is accepted all the way");
        double v = speed;
        for (int i = 0; i < 12; ++i) {
            v = std::max(1290.0, v - 8500.0 * 0.07);
            f.step(v * 0.07, 0, 0.07, repl::kMoveFlying, -34.5);
        }
        CHECK(f.refused == 0, "and the brake that follows it, at 8,500 cm/s^2, is accepted");
    }
    {
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        for (int i = 0; i < 30; ++i) {
            v = std::min(2188.0, v + 1600.0 * 0.07);
            f.step(v * 0.07, 0, 0.07, repl::kMoveFlying, -13.0);
        }
        CHECK(f.refused == 0, "13 degrees down: the climb to 2,188 cm/s is accepted");
        Flight g(rig, 0, 0, 100000);
        double vg = 1290.0;
        for (int i = 0; i < 40; ++i) {
            vg = std::min(2800.0, vg + 1600.0 * 0.07);
            g.step(vg * 0.07, 0, 0.07, repl::kMoveFlying, -13.0);
        }
        CHECK(g.refused == 0, "at 13 degrees down 2,800 cm/s is past the ceiling (2,188 x 1.1) but inside 1.3 x it: logged, not corrected");
        Flight k(rig, 0, 0, 100000);
        double vk = 1290.0;
        for (int i = 0; i < 140; ++i) {
            vk = std::min(4000.0, vk + 1600.0 * 0.07);
            k.step(vk * 0.07, 0, 0.07, repl::kMoveFlying, -13.0);
        }
        CHECK(k.refused >= 1, "4,000 cm/s level under a 13-degree pitch (1.8 x the law, and past the carried speed's climb) is corrected");
    }
}

static void test_the_law(const Mounts &m) {
    std::printf("the flight law: cruise, the dive's ceiling, its climb and its brake\n");
    const content::MountRow *dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    if (!dawn) {
        return;
    }
    Rig rig(m, *dawn);
    test_the_law_cruise_and_margin(rig);
    test_the_law_dive_and_pitch(rig);
    {
        Flight f(rig, 0, 0, 100000);
        f.c.set_speed_mult(1.35);
        f.run(1741.5, 0, 0, 5.0);
        CHECK(f.refused == 0, "with MoveSpeedMult 1.35 the cruise is 1,741.5 cm/s");
        Flight g(rig, 0, 0, 100000);
        g.run(1741.5, 0, 0, 12.0);
        std::printf("    (1,741 cm/s for 12 s, no dash: %zu refused, %zu accepted, last fault %s)\n", g.refused, g.accepted, fault_name(g.last_fault));
        CHECK(g.refused >= 1 && g.last_fault == MoveFault::TooFastInAirForLong, "the same speed without the effect (a dash that was never started) is logged, then corrected once it has stayed over the law for 2.5 s");
    }
    {
        Flight f(rig, 0, 0, 100000);
        f.c.set_speed_mult(1.35);
        f.run(1741.5, 0, 0, 2.0);
        f.c.set_speed_mult(1.0);
        const size_t before = f.refused;
        f.run(1741.5, 0, 0, 1.0);
        CHECK(f.refused == before, "(a multiplier that fell is not a correction at once: 1,741 cm/s is inside 1.5 x the cruise)");
        f.run(1741.5, 0, 0, 5.0);
        CHECK(f.refused > before, "(but a dash that has ended is not kept: after the sustain window the speed is corrected; the session's grace is Ride::effective_mult)");
    }
    {
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        for (int i = 0; i < 40; ++i) {
            v = std::min(3671.0, v + 1600.0 * 0.07);
            f.step(v * 0.07, -v * 0.07 * 0.3, 0.07, repl::kMoveFlying, -34.5);
        }
        const size_t before = f.refused;
        for (int i = 0; i < 3; ++i) {
            f.step(3000.0 * 0.07, -200.0 * 0.07, 0.07, repl::kMoveFalling, -10);
        }
        CHECK(f.refused == before, "the first moves of a fall after a dive may still carry 3,000 cm/s along (the allowance brakes, it does not vanish)");
        for (int i = 0; i < 40; ++i) {
            f.step(0, -50.0 * 0.07, 0.07, repl::kMoveFalling, -10);
        }
        const size_t mid = f.refused;
        for (int i = 0; i < 20; ++i) {
            f.step(3000.0 * 0.07, 0, 0.07, repl::kMoveWalking, 0);
        }
        CHECK(f.refused > mid, "and 3 s later 3,000 cm/s on foot is refused again (the ground limit is 990 x 1.5)");
    }
}

static void test_the_gryphon_mode3_and_ground(const Mounts &m, const Rig &rig) {
    {
        Flight t(rig, 0, 0, 20000);
        t.step_dt = 0.069;
        t.run(1000.0, 0, 0, 3.0, repl::kMoveFalling);
        Flight u(rig, 0, 0, 20000);
        u.step_dt = 0.078;
        u.run(1230.0, 0, 0, 3.0, repl::kMoveFalling);
        std::printf("    (mode 3 level at 1,000 cm/s: %zu refused of %zu; at 1,230 cm/s: %zu of %zu)\n", t.refused, t.refused + t.accepted, u.refused, u.refused + u.accepted);
        CHECK(t.refused == 0 && u.refused == 0, "a flyer in movement mode 3 at 1,000 and at 1,230 cm/s level is not refused: it travels at flight speed in every mode of the air");
        Flight hack(rig, 0, 0, 20000);
        hack.run(4000.0, 0, 0, 3.0, repl::kMoveFalling);
        CHECK(hack.refused >= 5 && hack.average() < 1290.0 * 1.1 + 60.0, "but 4,000 cm/s in mode 3 is a hack: held to the cruise times the margin, as the flying mode is");
        Flight climb(rig, 0, 0, 20000);
        for (int i = 0; i < 20; ++i) {
            climb.step(0, 3000.0 * 0.07, 0.07, repl::kMoveFalling, 0);
        }
        CHECK(climb.refused >= 5, "and mode 3 does not climb: the rising rules stay (3,000 cm/s straight up is refused once the 500 cm burst is spent)");
    }
    const content::MountRow *ground = nullptr;
    for (const content::MountRow &row : m.book->data().mounts) {
        if (row.kind != "flying" && row.kind != "aquatic") {
            ground = &row;
            break;
        }
    }
    if (ground) {
        Rig horse(m, *ground);
        Flight f2(horse, 0, 0, 20000);
        f2.run(5000.0, 0, 0, 2.0, repl::kMoveFalling);
        CHECK(f2.refused >= 5, "a mount that cannot fly is still held to its ground limit in mode 3");
    } else {
        CHECK(false, "(a ground mount is in the book)");
    }
}

static void test_the_gryphon(const Mounts &m) {
    std::printf("the GM Eagle Gryphon (Blueprint 800, text 215 %%): the user's run flew it at about 1,570 cm/s\n");
    const content::MountRow *g = m.by_item("Mount_Flying_PTR_ONLY");
    if (!g) {
        CHECK(false, "(the Eagle Gryphon is in the book)");
        return;
    }
    const spawns::MountBook::Speeds *sp = m.book->speeds_of(g->blueprint);
    CHECK(sp && std::fabs(sp->fly - 800.0) < 1e-6 && std::fabs(sp->run - 600.0) < 1e-6, "its Blueprint (a plain child of Mount_Flying_Base) says MaxFlySpeed 800 and MaxRunSpeed 600");
    Rig rig(m, *g);
    CHECK(rig.ride.can_fly(), "it flies");
    Flight f(rig, 0, 0, 20000);
    f.run(1290.0, 0, 0, 4.0);
    CHECK(f.refused == 0, "its cruise is at least the item text's 1,290 cm/s");
    double v = 1290.0;
    for (int i = 0; i < 30; ++i) {
        v = std::min(1570.0, v + 1600.0 * 0.052);
        f.step(v * 0.052, -v * 0.052 * 0.1, 0.052, repl::kMoveFlying, -10.4);
    }
    CHECK(f.refused == 0, "a gentle dive (10 degrees down) at 1,570 cm/s, the speed the refused moves of the run had, is accepted");
    Flight h(rig, 0, 0, 20000);
    h.run(2200.0, 0, 0, 3.0);
    std::printf("    (2,200 for 3 s: %zu refused, %zu accepted, average %.0f cm/s)\n", h.refused, h.accepted, h.average());
    CHECK(h.refused >= 5 && h.average() < 1290.0 * 1.5 + 60.0, "2,200 cm/s level is still a hack on it: refused, held to 1.5 x the cruise");
    test_the_gryphon_mode3_and_ground(m, rig);
}

static void test_exploits_air_speed_and_teleport(const Rig &fly) {
    {
        Flight f(fly, 0, 0, 10000);
        f.run(3000.0, 0, 0, 2.0);
        CHECK(f.refused >= 10 && f.last_fault == MoveFault::TooFastInAir && f.average() < 1290.0 * 1.5 + 60.0, "a flying speed hack (3,000 cm/s level, no dive before) is refused as too fast in the air, and gets nowhere faster than 1.5 x the cruise");
    }
    {
        Flight f(fly, 0, 0, 10000);
        f.run(1290.0, 0, 0, 1.0);
        const size_t before = f.refused;
        f.step(0, 5000.0, 0.07, repl::kMoveFlying, 0);
        CHECK(f.refused == before + 1 && f.last_fault == MoveFault::TooFastInAir, "a vertical teleport (+5,000 cm in 0.07 s) is refused");
        f.step(0, -5000.0, 0.07, repl::kMoveFlying, -90);
        CHECK(f.refused == before + 2, "and 5,000 cm straight down with the view pitched 90 degrees down: the dive is a ramp (2,400 cm/s^2), not a switch");
    }
    {
        Flight f(fly, 0, 0, 10000);
        const size_t before = f.refused;
        f.step(5000.0 * 0.07, 0, 0.07, repl::kMoveFlying, -90.0);
        f.step(5000.0 * 0.07, 0, 0.07, repl::kMoveFlying, -90.0);
        CHECK(f.refused > before, "5,000 cm/s level under a faked 90-degree pitch is refused at once");
        Flight g(fly, 0, 0, 10000);
        for (int i = 0; i < 60; ++i) {
            g.step(5000.0 * 0.07, 0, 0.07, repl::kMoveFlying, -90.0);
        }
        const double bound = 1.1 * (1290.0 * g.elapsed + 0.5 * 4096.0 * g.elapsed * g.elapsed) + 100.0;
        std::printf("    (4.2 s of 5,000 cm/s under a faked pitch: %zu refused, the client got %.0f cm, the ramp's bound is %.0f)\n", g.refused, g.travelled(), bound);
        CHECK(g.refused >= 1 && g.travelled() <= bound, "and held to the climb the client can make: it gets no further than 1.1 x (cruise x t + 2,048 x t^2)");
    }
}

static void test_exploits_air(const Rig &fly, const Rig &ground, const Rig &water) {
    test_exploits_air_speed_and_teleport(fly);
    Flight f(ground, 0, 0, 10000);
    f.step(60, 0, 0.07, repl::kMoveFlying, 0);
    CHECK(f.refused == 1 && f.last_fault == MoveFault::ModeNotAllowed, "movement mode 5 on a ground mount is refused (ModeNotAllowed)");
    Flight a(water, 0, 0, 10000);
    a.step(60, 0, 0.07, repl::kMoveFlying, 0);
    CHECK(a.refused == 1 && a.last_fault == MoveFault::ModeNotAllowed, "and on an aquatic mount");
    MoveChecker pawn(MoveLimits{}, GoodPosition{});
    CHECK(pawn.check(move_at(1.0f, {0, 0, 0}, repl::kMoveFlying), t0).accepted, "(the pawn's checker, without `air`, does not refuse mode 5: the rule is the mount's)");
}

static void test_exploits_water_and_ground(const Rig &ground, const Rig &water) {
    {
        Flight w(water, 0, 0, 1000);
        w.run(920.0, 0, 0, 5.0, repl::kMoveSwimming);
        CHECK(w.refused == 0, "an aquatic mount swims at its 920 cm/s (mode 4)");
        Flight w2(water, 0, 0, 1000);
        w2.run(1400.0, 0, 0, 3.0, repl::kMoveSwimming);
        CHECK(w2.refused >= 10 && w2.average() < 920.0 * 1.1 + 60.0, "and not at 1,400");
        Flight g(ground, 0, 0, 1000);
        g.run(920.0, 0, 0, 3.0, repl::kMoveSwimming);
        CHECK(g.refused >= 10 && g.average() < 400.0 * 1.1 + 60.0, "a ground mount claiming mode 4 at 920 cm/s is refused: its water speed is its Blueprint's 400");
        Flight g2(ground, 0, 0, 1000);
        g2.run(400.0, 0, 0, 3.0, repl::kMoveSwimming);
        CHECK(g2.refused == 0, "(a ground mount that really swims, at its 400, is not refused)");
        Flight v(water, 0, 0, 1000);
        v.run(1400.0, 52.0, 0, 3.0, repl::kMoveSwimming);
        CHECK(v.refused >= 10 && v.average() < 920.0 * 1.1 + 60.0, "the water speed is a 3D speed: 1,100 cm/s up with 860 along (1,400) is refused, with the climb counted in");
        Flight v2(water, 0, 0, 1000);
        v2.run(920.0, 45.0, 0, 3.0, repl::kMoveSwimming);
        CHECK(v2.refused == 0, "while 920 cm/s on a slant is fine");
    }
    {
        Flight w(ground, 0, 0, 1000);
        w.run(1400.0, 0, 0, 2.0, repl::kMoveWalking);
        CHECK(w.refused == 0, "(the ground mount's walking limit is unchanged: 1,400 cm/s passes)");
        Flight w2(ground, 0, 0, 1000);
        w2.run(1800.0, 0, 0, 3.0, repl::kMoveWalking);
        CHECK(w2.refused >= 5 && w2.average() < 990.0 * 1.5 + 60.0, "(and 1,800 cm/s does not: held to 990 x 1.5)");
    }
}

static void test_exploits(const Mounts &m) {
    std::printf("exploits: a flying speed hack, a vertical teleport, flight on a ground mount, swimming on a mount that is not aquatic\n");
    const content::MountRow *dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    const content::MountRow *bird = m.by_item("Mount_Grounded_TerrorBird_0");
    if (!bird) {
        bird = m.by_blueprint_prefix("Mount_Grounded_");
    }
    const content::MountRow *shoal = m.by_blueprint_prefix("Mount_Aquatic_Shoalskipper");
    if (!dawn || !bird || !shoal) {
        CHECK(false, "(the mounts are in the book)");
        return;
    }
    Rig fly(m, *dawn), ground(m, *bird), water(m, *shoal);
    test_exploits_air(fly, ground, water);
    test_exploits_water_and_ground(ground, water);
}

static constexpr double kFakePitchSustainedMax = 2.8;

static void test_review_a_faked_and_real_dive(const Rig &fly) {
    {
        Flight f(fly, 0, 0, 10000);
        f.run(1290.0, 0, 0, 1.0);
        const double x_at = f.x, t_at = f.elapsed;
        for (int i = 0; i < 300; ++i) {
            f.step(8000.0 * 0.07, 0, 0.07, repl::kMoveFlying, -90.0);
        }
        const double sustained = (f.x - x_at) / (f.elapsed - t_at);
        const double cruise = 1290.0;
        std::printf("    (20 s of a faked pitch on a level path: %zu refused, sustained %.0f cm/s = %.2f x the cruise)\n", f.refused, sustained, sustained / cruise);
        CHECK(sustained < cruise * kFakePitchSustainedMax, "a faked pitch on a level path buys no sustained speed beyond the trusted share of the dive's ceiling");
    }
    {
        Flight f(fly, 0, 0, 350000);
        f.run(1290.0, 0, 0, 1.0);
        double v = 1290.0, top = 0;
        float last_ts = f.ts;
        double last_z = f.z;
        for (int i = 0; i < 100; ++i) {
            v = std::min(7000.0, v + 3500.0 * 0.07);
            const size_t before = f.refused;
            f.step(0, -v * 0.07, 0.07, repl::kMoveFlying, -90.0);
            if (f.refused == before) {
                top = std::max(top, (last_z - f.z) / (f.ts - last_ts));
                last_ts = f.ts;
                last_z = f.z;
            }
        }
        CHECK(top > 6500.0, "(control: a real dive straight down under -90 degrees still reaches MaxFlyFallSpeed)");
    }
}

static void test_review_a(const Mounts &m) {
    std::printf("review A: alternating modes (C1), a faked pitch on a level path (I2)\n");
    const content::MountRow *dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    const content::MountRow *shoal = m.by_blueprint_prefix("Mount_Aquatic_Shoalskipper");
    if (!dawn || !shoal) {
        CHECK(false, "(the mounts are in the book)");
        return;
    }
    Rig fly(m, *dawn), water(m, *shoal);
    for (const bool flying : {true, false}) {
        Flight f(flying ? fly : water, 0, 0, 10000);
        f.step(0, 0, 0.07, repl::kMoveWalking, 0);
        const uint8_t mode = flying ? repl::kMoveFlying : repl::kMoveSwimming;
        size_t refused = 0;
        for (int i = 0; i < 300; ++i) {
            f.step(0, 0, 0.001, mode, 0);
            if (!f.step(0, 500.0, 0.001, repl::kMoveWalking, 0)) {
                ++refused;
            }
        }
        const double climbed = f.z - f.z0;
        const double bound = 1350.0 * f.elapsed + 500.0 + 100.0;
        char msg[200];
        std::snprintf(msg, sizeof(msg), "%s: 300 pairs of (mode %d standing still, 500 cm up on the ground) in %.2f s climb %.0f cm, at most %.0f (%zu refused)", flying ? "flying" : "swimming",
                      static_cast<int>(mode), f.elapsed, climbed, bound, refused);
        CHECK(climbed <= bound && refused > 250, msg);
    }
    test_review_a_faked_and_real_dive(fly);
}

static void test_dive_gravity_and_coast(const Rig &rig) {
    {
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        const double slope = 60.0 * 3.14159265358979323846 / 180.0;
        for (int i = 0; i < 28; ++i) {
            v = std::min(3500.0, v + 1100.0 * 0.07);
            f.step(v * 0.07 * std::cos(slope), -v * 0.07 * std::sin(slope), 0.07, repl::kMoveFlying, 0.0);
        }
        CHECK(f.refused == 0 && v > 3400.0, "a steep fall with the view level (gravity helps the climb of the speed) is accepted up to 3,500 cm/s");
        Flight h(rig, 0, 0, 100000);
        double w = 1290.0;
        for (int i = 0; i < 40; ++i) {
            w = std::min(3500.0, w + 1100.0 * 0.07);
            h.step(w * 0.07, 0, 0.07, repl::kMoveFlying, 0.0);
        }
        std::printf("    (level climb of the speed: %zu refused, %zu accepted)\n", h.refused, h.accepted);
        CHECK(h.refused >= 1, "while the same climb of the speed in level flight (no descent, no dive) is corrected: it is not the client's physics");
    }
    {
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        for (int i = 0; i < 20; ++i) {
            v = std::min(3671.0, v + 3000.0 * 0.07);
            f.step(v * 0.07 * std::cos(0.6), -v * 0.07 * std::sin(0.6), 0.07, repl::kMoveFlying, -34.5);
        }
        CHECK(f.refused == 0, "a dive that climbs at 3,000 cm/s^2 (twice the retail 1,600, inside the client's physics) is accepted");
    }
    {
        Flight f(rig, 0, 0, 100000);
        double v = 1290.0;
        for (int i = 0; i < 40; ++i) {
            v = std::min(3671.0, v + 1600.0 * 0.07);
            f.step(v * 0.07 * std::cos(0.45), -v * 0.07 * std::sin(0.45), 0.07, repl::kMoveFlying, -34.5);
        }
        for (int i = 0; i < 20; ++i) {
            v = std::max(1290.0, v - 2500.0 * 0.07);
            f.step(v * 0.07, 0, 0.07, repl::kMoveFlying, 0.0);
        }
        CHECK(f.refused == 0, "after a dive to 3,671 the carried speed coasts down at 2,500 cm/s^2 with the view level: never corrected");
    }
}

static void test_the_users_dive(const Mounts &m) {
    std::printf("stage 4 fix 1: the user's dive and level-out (the logged moves), momentum, gravity\n");
    const content::MountRow *g = m.by_item("Mount_Flying_PTR_ONLY");
    if (!g) {
        CHECK(false, "(the Eagle Gryphon is in the book)");
        return;
    }
    Rig rig(m, *g);
    {
        Flight f(rig, -804000, 621000, 16000);
        f.step_dt = 0.074;
        f.run(1290.0, 0, 0, 3.0, repl::kMoveFlying);
        double v = 1290.0;
        const double down = 10.4 * 3.14159265358979323846 / 180.0;
        for (int i = 0; i < 10; ++i) {
            v = std::min(1905.0, v + 1600.0 * 0.074);
            f.step(v * 0.074 * std::cos(down), -v * 0.074 * std::sin(down), 0.074, repl::kMoveFlying, -10.4);
        }
        CHECK(f.refused == 0 && v > 1900.0, "the dive to 1,905 cm/s (141 cm in 0.074 s, the first refused move of the log) passes");
        f.step(134.0, -134.0 * std::tan(down), 0.070, repl::kMoveFlying, -10.4);
        CHECK(f.refused == 0, "134 cm in 0.070 s (1,914 cm/s, the second) passes");
        size_t level_refused = 0;
        for (int i = 0; i < 10; ++i) {
            v = std::max(1290.0, v - 2500.0 * 0.081);
            const size_t before = f.refused;
            f.step(v * 0.081, 0, 0.081, repl::kMoveFlying, 0.0);
            level_refused += f.refused - before;
        }
        CHECK(level_refused == 0, "levelling out at high speed (the carried 1,905 cm/s coasting down, the third refused move of the log among it) is never corrected");
    }
    {
        MoveChecker plain(MoveLimits{}, GoodPosition{});
        plain.set_max_speed(1743.0);
        plain.check(move_at(1.0f, {0, 0, 0}, repl::kMoveFlying), t0);
        const MoveVerdict w = plain.check(move_at(1.074f, {5000.0, 0, 0}, repl::kMoveFlying), t0 + std::chrono::milliseconds(74));
        CHECK(!w.accepted && w.fault == MoveFault::TooFastInAir, "mode 5 is never judged 'along the ground': a mode-5 teleport is refused as too fast in the air");
    }
    test_dive_gravity_and_coast(rig);
}

static void test_gross_only_straight_dive(const Rig &rig) {
    Flight f(rig, 0, 0, 350000);
    double v = 1290.0, top = 0, top_refused_at = 0, last_z = f.z;
    float last_ts = f.ts;
    for (int i = 0; i < 160; ++i) {
        v = std::min(12000.0, v + 3500.0 * 0.07);
        const size_t before = f.refused;
        f.step(0, -v * 0.07, 0.07, repl::kMoveFlying, -90.0);
        if (f.refused == before) {
            top = std::max(top, (last_z - f.z) / (f.ts - last_ts));
            last_ts = f.ts;
            last_z = f.z;
        } else {
            top_refused_at = std::max(top_refused_at, v);
        }
    }
    std::printf("    (straight dive: fastest accepted step %.0f cm/s, %zu refused, refusals up to a claimed %.0f)\n", top, f.refused, top_refused_at);
    CHECK(top > 6500.0 && top <= 7500.0 * 1.1 + 50.0 / 0.07, "a straight dive may reach MaxFlyFallSpeed (7,500) and no more than 1.1 x it (and the 50 cm slack)");
    CHECK(f.refused >= 1 && top_refused_at > 8250.0, "... and a speed above that physical maximum is refused");
    Flight h(rig, 0, 0, 350000);
    h.run(1290.0, 0, 0, 1.0);
    h.step(0, -9000.0 * 0.07, 0.07, repl::kMoveFlying, -90.0);
    CHECK(h.last_verdict.accepted == false && h.last_fault == MoveFault::TooFastInAir, "9,000 cm/s straight down out of the cruise is refused at once");
}

static void test_gross_only_sustain(const Rig &rig) {
    Flight f(rig, 0, 0, 10000);
    size_t flagged = 0;
    double first_refusal_at = -1;
    for (int i = 0; i < 200; ++i) {
        f.step(1290.0 * 1.45 * 0.07, 0, 0.07, repl::kMoveFlying, 0.0);
        if (f.last_verdict.accepted && f.last_verdict.borderline) {
            ++flagged;
        }
        if (!f.last_verdict.accepted && first_refusal_at < 0) {
            first_refusal_at = f.elapsed;
        }
    }
    std::printf("    (1.45 x level for 14 s: %zu refused, %zu flagged, first refusal at %.2f s, average %.0f cm/s)\n", f.refused, flagged, first_refusal_at, f.average());
    CHECK(f.refused >= 1 && f.last_fault == MoveFault::TooFastInAirForLong && flagged >= 10 && first_refusal_at > 2.3 && first_refusal_at < 3.0,
          "1.45 x the cruise level is flagged for 2.5 s and then corrected (the sustain window)");
    CHECK(f.average() < 1290.0 * 1.3, "and after it the client is held to the law: its average pace over 14 s is below 1.3 x the cruise");
}

static void test_gross_only(const Mounts &m) {
    std::printf("stage 4 fix 1: only gross violations are corrected\n");
    const content::MountRow *dawn = m.by_blueprint_prefix("Mount_Flying_Dawnbreaker");
    if (!dawn) {
        return;
    }
    Rig rig(m, *dawn);
    {
        Flight f(rig, 0, 0, 10000);
        f.run(1290.0 * 1.2, 0, 0, 1.0);
        CHECK(f.refused == 0 && f.last_verdict.accepted && f.last_verdict.borderline && f.last_verdict.soft_allowed > 0.0 && f.last_verdict.step > f.last_verdict.soft_allowed,
              "a move over the plain law (1.2 x the cruise) is accepted and flagged with the law's number (`borderline`, `soft_allowed`)");
        Flight g(rig, 0, 0, 10000);
        g.run(1290.0, 0, 0, 1.0);
        CHECK(!g.last_verdict.borderline, "a move inside the law is not flagged");
    }
    test_gross_only_straight_dive(rig);
    test_gross_only_sustain(rig);
    {
        Flight f(rig, 0, 0, 10000);
        double v = 1290.0;
        for (int i = 0; i < 200; ++i) {
            v = std::min(5000.0, v + 2000.0 * 0.07);
            f.step(v * 0.07, 0, 0.07, repl::kMoveFlying, 0.0);
        }
        std::printf("    (a level ramp to 5,000: %zu refused, average %.0f cm/s)\n", f.refused, f.average());
        CHECK(f.refused >= 20 && f.average() < 1290.0 * 1.5, "a level ramp to 5,000 cm/s is held below 1.5 x the cruise (nothing earned it)");
    }
}

static void test_health(const Mounts &m) {
    std::printf("flyers' Health: x5 of Mount_Base_HP, x10 for the Drake of the Canopy and the Thundering Bolt Beast (the item texts)\n");
    size_t flyers = 0, x5 = 0, x10 = 0, wrong = 0;
    std::set<std::string> tens;
    for (const content::MountRow &r : m.book->data().mounts) {
        if (r.kind != "flying") {
            continue;
        }
        ++flyers;
        const double base = m.book->max_health(content::MountRow{}, 30);
        const double hp = m.book->max_health(r, 30);
        if (std::fabs(hp - base * 5.0) < 0.5) {
            ++x5;
        } else if (std::fabs(hp - base * 10.0) < 0.5) {
            ++x10;
            tens.insert(r.item_name);
        } else {
            ++wrong;
        }
    }
    CHECK(flyers >= 46 && wrong == 0, "every flying mount has x5 or x10 Health");
    CHECK(x10 == 3 && tens.count("Mount_Flying_DrakeOfTheCanopy_0") == 1 && tens.count("Mount_Flying_ThunderingBoltBeast_0") == 1 && tens.count("Mount_Flying_ThunderingBoltBeast_1") == 1,
          "x10 is the Drake of the Canopy and the Thundering Bolt Beast (3 items), x5 the other 43");
    CHECK(x5 == flyers - 3, "(x5 for all the rest)");
}

int main() {
    Mounts m;
    test_goldens(m);
    test_the_law(m);
    test_the_gryphon(m);
    test_exploits(m);
    test_review_a(m);
    test_the_users_dive(m);
    test_gross_only(m);
    test_health(m);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
