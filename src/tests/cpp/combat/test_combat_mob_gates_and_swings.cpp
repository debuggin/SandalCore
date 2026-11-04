// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#include "combat/bite_area.h"
#include "combat/combat_world.h"
#include "combat/npc_ability_data.h"
#include "npc/ai/npc_ai.h"
#include "npc/ai/threat.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

using namespace aoc::worldd;

void check_true(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "FAIL(%d): %s\n", line, expr);
        std::exit(1);
    }
}

void check_near(double actual, double expected, double tol, const char* expr, int line) {
    if (std::fabs(actual - expected) > tol) {
        std::fprintf(stderr, "FAIL(%d): %s (actual=%.6f expected=%.6f)\n",
                     line, expr, actual, expected);
        std::exit(1);
    }
}

#define ASSERT_TRUE(c) check_true((c), #c, __LINE__)
#define ASSERT_NEAR(a, b, t) check_near((a), (b), (t), #a " ~= " #b, __LINE__)

void verify_notice_and_threat_table() {
    world::NpcInfo aggro_npc{};
    aggro_npc.aggro_off = false;
    world::NpcInfo passive_npc{};
    passive_npc.aggro_off = true;

    ASSERT_TRUE(combat::notices_players(true, aggro_npc));
    ASSERT_TRUE(!combat::notices_players(false, aggro_npc));
    ASSERT_TRUE(!combat::notices_players(true, passive_npc));

    npc::ai::ThreatTable table;
    ASSERT_TRUE(table.empty());
    ASSERT_TRUE(table.add(101, 100.0, 1.0));
    ASSERT_TRUE(table.add(202, 105.0, 1.5));
    ASSERT_TRUE(table.size() == 2u);
    ASSERT_NEAR(table.of(101), 100.0, 1e-6);
    ASSERT_TRUE(table.pick(101) == 101u);

    ASSERT_TRUE(table.add(202, 10.0, 2.0));
    ASSERT_NEAR(table.of(202), 115.0, 1e-6);
    ASSERT_TRUE(table.pick(101) == 202u);

    table.remove(202);
    ASSERT_TRUE(!table.has(202));
    ASSERT_TRUE(table.pick(202) == 101u);
}

void verify_ai_aggro_chase_and_leash_evade() {
    npc::ai::Params params{};
    params.aggro_radius_cm = 800.0;
    params.leash_radius_cm = 2000.0;
    params.sense_interval = 0.0;
    params.reset_arrive_cm = 100.0;
    npc::ai::NpcAi ai(params);

    npc::ai::Sense s{};
    s.now = 1.0;
    s.self = {0.0, 0.0, 0.0};
    s.home = {0.0, 0.0, 0.0};
    s.attack_range_cm = 150.0;
    s.known.push_back({1001u, {500.0, 0.0, 0.0}, 50.0, true, true});

    auto ord1 = ai.think(s);
    ASSERT_TRUE(ai.state() == npc::ai::State::Chase);
    ASSERT_TRUE(ord1.began_fight);
    ASSERT_TRUE(ord1.in_combat);
    ASSERT_TRUE(ord1.target == 1001u);
    ASSERT_TRUE(ord1.move == npc::ai::Orders::Move::ChaseTo);

    s.now = 2.0;
    s.self = {400.0, 0.0, 0.0};
    auto ord2 = ai.think(s);
    ASSERT_TRUE(ai.state() == npc::ai::State::Attack);
    ASSERT_TRUE(ord2.attack);
    ASSERT_TRUE(ord2.move == npc::ai::Orders::Move::Stand);

    s.now = 3.0;
    s.self = {2500.0, 0.0, 0.0};
    s.known[0].pos = {2600.0, 0.0, 0.0};
    auto ord3 = ai.think(s);
    ASSERT_TRUE(ai.state() == npc::ai::State::Reset);
    ASSERT_TRUE(ord3.began_reset);
    ASSERT_TRUE(ord3.move == npc::ai::Orders::Move::Home);
    ASSERT_TRUE(ai.threat().empty());

    s.now = 4.0;
    s.self = {50.0, 0.0, 0.0};
    s.known.clear();
    auto ord4 = ai.think(s);
    ASSERT_TRUE(ai.state() == npc::ai::State::Idle);
    ASSERT_TRUE(ord4.reset_done);
    ASSERT_TRUE(!ord4.in_combat);
}

void verify_bite_area_and_swing_cadence() {
    combat::MontageData montage{};
    montage.length = 2.4;
    montage.rate_scale = 1.2;
    montage.anim_end = 1.8;
    ASSERT_NEAR(montage.seconds(1.0), 2.0, 1e-5);
    ASSERT_NEAR(montage.task_done_after(1.0), 1.5, 1e-5);
    ASSERT_NEAR(montage.task_done_after(2.0), 0.75, 1e-5);

    combat::BiteArea area{};
    area.discovery = combat::BiteArea::Discovery::PrefersActive;
    area.check_validity = true;
    area.cone_check = true;
    area.los_check = true;
    area.distance = 250.0;
    area.angle = 90.0;
    ASSERT_TRUE(area.usable());
    ASSERT_TRUE(area.gate() == combat::BiteArea::Gate::Validity);

    combat::HitSubject front{0.0, 0.0, 0.0, 150.0, 0.0, 40.0};
    combat::HitSubject behind{0.0, 0.0, 0.0, -150.0, 0.0, 40.0};
    combat::HitSubject far_out{0.0, 0.0, 0.0, 400.0, 0.0, 40.0};

    ASSERT_TRUE(combat::hit_lands(area, front, true, [] { return true; }));
    ASSERT_TRUE(!combat::hit_lands(area, front, true, [] { return false; }));
    ASSERT_TRUE(!combat::hit_lands(area, behind, true, [] { return true; }));
    ASSERT_TRUE(!combat::hit_lands(area, far_out, true, [] { return true; }));
}

void verify_npc_swing_condition_gates() {
    combat::CondContext ctx{};
    ctx.has_target = true;
    ctx.surface_distance = 120.0;
    ctx.centre_distance = 160.0;
    ctx.angle = 25.0;
    ctx.health_fraction = 0.50;
    ctx.fight_seconds = 8.0;

    combat::CondAtom dist_gate{};
    dist_gate.kind = combat::CondAtom::Kind::In;
    dist_gate.value = 200.0;
    ASSERT_TRUE(combat::eval_atom(dist_gate, ctx));

    combat::CondAtom hp_under{};
    hp_under.kind = combat::CondAtom::Kind::HealthUnder;
    hp_under.value = 0.40;
    ASSERT_TRUE(!combat::eval_atom(hp_under, ctx));

    combat::CondAtom hp_not_under{};
    hp_not_under.kind = combat::CondAtom::Kind::HealthNotUnder;
    hp_not_under.value = 0.40;
    ASSERT_TRUE(combat::eval_atom(hp_not_under, ctx));

    combat::CondGroup group = {
        {'c', 0, dist_gate},
        {'a', 0, hp_not_under},
    };
    ASSERT_TRUE(combat::eval_group(group, ctx));
}

void test_mob_aggro_and_leash_evade() {
    verify_notice_and_threat_table();
    verify_ai_aggro_chase_and_leash_evade();
}

void test_mob_swing_timer_cadence() {
    verify_bite_area_and_swing_cadence();
    verify_npc_swing_condition_gates();
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view selected_case;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            selected_case = argv[++i];
        }
    }

    if (selected_case.empty() || selected_case == "mob_aggro_and_leash_evade") {
        test_mob_aggro_and_leash_evade();
    }
    if (selected_case.empty() || selected_case == "mob_swing_timer_cadence") {
        test_mob_swing_timer_cadence();
    }
    return 0;
}
