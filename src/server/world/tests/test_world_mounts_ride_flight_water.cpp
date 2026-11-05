// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_mounts_ride_common.h"

#include "nav_fixture.h"
#include "town_platform_fixture.h"

using namespace aoc::worldd;
using namespace aoc::protocol;
using std::chrono::milliseconds;

namespace {

uint16_t pitch16_of(double degrees) { return static_cast<uint16_t>(static_cast<int>(std::lround(degrees * 65536.0 / 360.0)) & 0xFFFF); }
svc::Character cyb_on_the_ground() {
    svc::Character c = cyb_record();
    c.has_position = true;
    c.position = {0.0, 0.0, 1098.0, 0.0f};
    return c;
}
void adopt_the_ground(PlayerWorld& w) {
    std::string err;
    auto nav = aoc::worldd::nav::AreaNav::from_file(aoc::worldd::navfix::build_file(), &err);
    if (!nav || !w.adopt_ground(std::move(nav), err)) {
        std::printf("  [FAIL] the ground does not load: %s\n", err.c_str());
        std::exit(2);
    }
}
std::optional<int64_t> registry_view_pitch(const Harness& h) {
    const world::WorldActor* m = h.mount();
    return m ? int_of(m->actor.obj, "RemoteViewPitch16") : std::nullopt;
}
std::optional<int64_t> client_view_pitch(const Harness& h, size_t from) {
    const world::WorldActor* m = h.mount();
    std::optional<int64_t> out;
    if (!m) return out;
    for (const fake::Delivered* d : chains_on(h, h.mount_ch(), from)) {
        const States st = states_of(*d, m->actor);
        const auto it = st.find(0);
        if (it != st.end()) {
            if (const auto v = int_of(it->second, "RemoteViewPitch16")) out = *v;
        }
    }
    return out;
}
void take_off(Harness& h, Rider& r, double altitude) {
    for (int i = 0; i < 4; ++i) ride_step_mode(h, r, {0, 90, 0}, repl::kMoveWalking);
    for (int i = 0; i < 3; ++i) ride_step_mode(h, r, {0, 60, 55}, repl::kMoveFalling);
    const double z0 = r.z;
    for (int i = 0; i < 4; ++i) ride_step_mode(h, r, {0, 90, 80}, repl::kMoveFlying);
    while (r.z - z0 < altitude) ride_step_mode(h, r, {0, 90, 80}, repl::kMoveFlying);
}
void verify_flight_dash_and_landing(Harness& h, Rider& r) {
    const int64_t dash = h.fx.world->mounts()->dash_ability();
    const emit::FIntrepidNetworkGUID comp = ability_component_of(*h.mount());
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, comp, dash, 31), true)});
    h.run(milliseconds(300));
    CHECK(h.s->ride()->dash_active() && std::fabs(mult_of(*h.mount()).value_or(0) - 1.35) < 1e-6, "the dash starts in the air: MoveSpeedMult 1.35");
    const size_t refused2 = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step_mode(h, r, {174.15, 0, 0}, repl::kMoveFlying, 0);
    CHECK(h.s->moves_refused() == refused2, "20 moves at 1,741.5 cm/s (1,290 x 1.35) are accepted while the dash lasts");
    double v = 1290.0;
    int guard = 0;
    while (r.z > 1057.0 + 800.0 && guard++ < 200) {
        v = std::min(4500.0, v + 160.0);
        ride_step_mode(h, r, {v * 0.1 * 0.5, 0, -v * 0.1 * 0.866}, repl::kMoveFlying, pitch16_of(-60.0));
    }
    const size_t refused3 = h.s->moves_refused();
    for (int i = 0; i < 4; ++i) ride_step_mode(h, r, {150, 0, -std::min(r.z - 1057.0, 200.0)}, repl::kMoveFalling);
    const double ground_z = 1057.0;
    r.z = ground_z;
    for (int i = 0; i < 6; ++i) ride_step_mode(h, r, {99, 0, 0}, repl::kMoveWalking);
    CHECK(h.s->moves_refused() == refused3 && h.s->movement().good().movement_mode == repl::kMoveWalking && std::fabs(h.s->movement().good().z - ground_z) < 2.0,
          "landing: mode 5 to falling to walking on the ground, the carried speed of the dive and the 990 x 1.5 walking limit all accepted");
    const size_t refused4 = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step_mode(h, r, {300, 0, 0}, repl::kMoveWalking);
    CHECK(h.s->moves_refused() > refused4, "3,000 cm/s on the ground is refused again once the flight's allowance has braked away");
}
void test_flight_session() {
    std::printf("stage 4: flying: takeoff by ordinary moves, the dive, the dash in the air, RemoteViewPitch16, landing\n");
    Harness h(cyb_on_the_ground(), adopt_the_ground);
    h.play();
    CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    const size_t from = h.got.size();
    take_off(h, r, 4000.0);
    CHECK(h.s->moves_refused() == 0 && h.s->movement().good().movement_mode == repl::kMoveFlying, "takeoff by ordinary moves (mode 1, the jump in mode 3, the second jump in mode 5) and a climb at 1,200 cm/s are all accepted");
    CHECK(h.s->mount_moves_acknowledged() >= 40, "(each acknowledged on the mount's channel)");
    ride_step_mode(h, r, {90, 0, 0}, repl::kMoveFlying, pitch16_of(-20.0));
    h.run(milliseconds(200));
    CHECK(registry_view_pitch(h) == static_cast<int64_t>(pitch16_of(-20.0)) && client_view_pitch(h, from) == static_cast<int64_t>(pitch16_of(-20.0)),
          "RemoteViewPitch16 of the mount is the move's pitch, in the registry and on the wire (the only thing retail sends besides the move responses)");
    double v = 1290.0;
    for (int i = 0; i < 28; ++i) {
        v = std::min(3671.0, v + 160.0);
        ride_step_mode(h, r, {v * 0.1 * std::cos(0.45), 0, -v * 0.1 * std::sin(0.45)}, repl::kMoveFlying, pitch16_of(-34.5));
    }
    CHECK(h.s->moves_refused() == 0, "a dive at 34.5 degrees down climbing to 3,671 cm/s is accepted");
    for (int i = 0; i < 12; ++i) ride_step_mode(h, r, {std::max(1290.0, 3671.0 - 850.0 * (i + 1)) * 0.1, 0, 0}, repl::kMoveFlying, 0);
    CHECK(h.s->moves_refused() == 0, "the brake that follows is accepted");
    const size_t refused = h.s->moves_refused();
    for (int i = 0; i < 12; ++i) ride_step_mode(h, r, {300, 0, 0}, repl::kMoveFlying, 0);
    CHECK(h.s->moves_refused() > refused, "3,000 cm/s level is refused in flight");
    const GoodPosition good = h.s->movement().good();
    r.x = good.x;
    r.y = good.y;
    r.z = good.z;
    verify_flight_dash_and_landing(h, r);
}
void test_no_road_bonus_in_the_air() {
    std::printf("review A M2: a flyer in the air over a road gets no road bonus\n");
    Harness h(cyb_on_the_ground(), adopt_the_ground);
    h.play();
    summon_with(h, kFlyingSlot);
    h.run(milliseconds(1900));
    const world::WorldActor* m0 = h.mount();
    CHECK(m0 && h.s->mounts()->phase() == MountController::Phase::Out, "(the flyer is out)");
    if (!m0) return;
    world::RoadMap::Polyline road;
    road.tier = 3;
    road.half_width_cm = 500;
    road.xy = {m0->x - 5000, m0->y, m0->x + 5000, m0->y};
    CHECK(h.fx.world->roads().add(road), "(a road is added under the flyer)");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(the player rides it)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    CHECK(h.s->ride()->road_tier() == 3, "on foot (mode 1) it is on the road");
    ride_step_mode(h, r, {0, 60, 0}, repl::kMoveWalking);
    CHECK(h.s->ride()->road_tier() == 3, "(and stays on it)");
    ride_step_mode(h, r, {0, 90, 80}, repl::kMoveFlying);
    CHECK(h.s->ride()->road_tier() == -1 && mult_of(*h.mount()).value_or(0) == 1.0, "in flight (mode 5) the road effect goes: MoveSpeedMult is 1");
}
void test_flight_on_a_ground_mount() {
    std::printf("stage 4: movement mode 5 on a mount that cannot fly, mode 4 at an aquatic mount's speed on one that is not aquatic\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h, kMountSlot), "(the player rides the terror bird)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 5; ++i) ride_step_mode(h, r, {90, 0, 0}, repl::kMoveWalking);
    const size_t refused = h.s->moves_refused();
    const size_t corrections = h.s->corrections_sent();
    ride_step_mode(h, r, {90, 0, 80}, repl::kMoveFlying);
    CHECK(h.s->moves_refused() == refused + 1 && h.s->corrections_sent() == corrections + 1, "a ground mount's move in mode 5 is refused and corrected");
    r.x = h.s->movement().good().x;
    r.z = h.s->movement().good().z;
    for (int i = 0; i < 5; ++i) ride_step_mode(h, r, {90, 0, 0}, repl::kMoveWalking);
    CHECK(h.s->moves_refused() == refused + 1, "(it walks on)");
    const size_t refused2 = h.s->moves_refused();
    for (int i = 0; i < 12; ++i) ride_step_mode(h, r, {92, 0, 0}, repl::kMoveSwimming);
    CHECK(h.s->moves_refused() > refused2, "swimming at 920 cm/s on a ground mount (its own water speed is 400) is refused");
}
void test_swimming_session() {
    std::printf("stage 4: an aquatic mount rides into the water: 600 on land, 920 in mode 4, no dismount\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h, kAquaticSlot), "(the player rides an aquatic mount)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 20; ++i) ride_step_mode(h, r, {59.9, 0, 0}, repl::kMoveWalking);
    CHECK(h.s->moves_refused() == 0, "20 moves on land at 599 cm/s (its Blueprint's run speed) are accepted");
    for (int i = 0; i < 20; ++i) ride_step_mode(h, r, {92, 0, i < 3 ? -30.0 : 0.0}, repl::kMoveSwimming);
    CHECK(h.s->moves_refused() == 0 && h.s->mounted() && h.s->dismounts() == 0 && h.s->movement().good().movement_mode == repl::kMoveSwimming,
          "20 moves in the water at 920 cm/s (mode 4) are accepted and the rider stays on: entering water does not dismount");
    for (int i = 0; i < 20; ++i) ride_step_mode(h, r, {40, 0, -60}, repl::kMoveSwimming);
    CHECK(h.s->moves_refused() == 0, "a dive and a climb inside the 920 are accepted (the water speed is a 3D speed)");
    const size_t refused = h.s->moves_refused();
    for (int i = 0; i < 12; ++i) ride_step_mode(h, r, {140, 0, 0}, repl::kMoveSwimming);
    CHECK(h.s->moves_refused() > refused, "1,400 cm/s in the water is refused");
    const GoodPosition good = h.s->movement().good();
    r.x = good.x;
    r.y = good.y;
    r.z = good.z;
    const world::WorldActor* m = h.mount();
    const emit::FIntrepidNetworkGUID comp = ability_component_of(*m);
    const int64_t dash = h.s->ride()->dash_ability();
    h.client_sends({h.client.actor(h.mount_ch(), trigger_on(h, comp, dash, 41), true)});
    h.run(milliseconds(300));
    const size_t refused2 = h.s->moves_refused();
    for (int i = 0; i < 20; ++i) ride_step_mode(h, r, {124.2, 0, 0}, repl::kMoveSwimming);
    CHECK(h.s->ride()->dash_active() && h.s->moves_refused() == refused2, "the dash in the water: 920 x 1.35 = 1,242 cm/s is accepted");
}
int mount_mode(const world::WorldActor& m) {
    const auto v = int_of(m.actor.obj, "ReplicatedMovementMode");
    return v ? static_cast<int>(*v) : -1;
}
void test_water_dismount() {
    std::printf("stage 4 fix 2: an aquatic mount dismounted in the water stays where it is, swimming; getting on again works\n");
    Harness h(cyb_record());
    h.play();
    CHECK(ride_on(h, kAquaticSlot), "(the player rides an aquatic mount)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    for (int i = 0; i < 20; ++i) ride_step_mode(h, r, {92, 0, i < 3 ? -30.0 : 0.0}, repl::kMoveSwimming);
    const GoodPosition at = h.s->movement().good();
    CHECK(at.movement_mode == repl::kMoveSwimming, "(the rider swims)");
    get_off(h);
    h.run(milliseconds(300));
    const world::WorldActor* m = h.mount();
    CHECK(!h.s->mounted() && m != nullptr, "(the rider is off, the mount is still there)");
    if (!m) return;
    CHECK(std::fabs(m->x - at.x) < 2.0 && std::fabs(m->y - at.y) < 2.0 && std::fabs(m->z - at.z) < 2.0, "the aquatic mount stays at its position in the water");
    CHECK(mount_mode(*m) == repl::kMoveSwimming, "and keeps its swimming movement mode (4)");
    h.run(milliseconds(2000));
    m = h.mount();
    CHECK(m && std::fabs(m->z - at.z) < 2.0, "it does not sink afterwards");
    get_on(h);
    h.run(milliseconds(300));
    CHECK(h.s->mounted(), "getting on again works from the swimmer's place beside it");
}
bool pawn_detached_at_z(const Harness& h, size_t from, double want_z) {
    for (const fake::Delivered* d : chains_on(h, h.pawn_ch, from)) {
        const States st = states_of(*d, h.s->bootstrap().pawn());
        const auto it = st.find(0);
        if (it == st.end()) continue;
        const repl::Value* loc = it->second.state.get(*it->second.schema, "RepDetachmentLocation");
        const auto* v = loc ? loc->get_if<repl::Vec3d>() : nullptr;
        if (v && std::fabs(v->z - want_z) < 1.0) return true;
    }
    return false;
}
void verify_air_dismount_fall() {
    Harness h(cyb_on_the_ground(), adopt_the_ground);
    h.play();
    CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    take_off(h, r, 5000.0);
    const GoodPosition up = h.s->movement().good();
    CHECK(up.z > 1057.0 + 4800.0, "(the rider is 50 m up)");
    const GoodPosition saved = h.s->last_position();
    CHECK(std::fabs(saved.z - (1000.0 + 98.0)) < 2.0 && std::hypot(saved.x - up.x, saved.y - up.y) < 100.0, "relog while flying: the character is saved on the ground below its last good position");
    const size_t from = h.got.size();
    get_off(h);
    h.run(milliseconds(300));
    const world::WorldActor* m = h.mount();
    CHECK(!h.s->mounted() && h.s->air_dismounts() == 1, "dismounting in the air is counted and puts the rider off");
    CHECK(m && std::fabs(m->z - up.z) < 2.0 && std::hypot(m->x - up.x, m->y - up.y) < 2.0, "the flyer stays where its rider left it, at its altitude (it does not settle on the ground)");
    CHECK(m && mount_mode(*m) == repl::kMoveFlying, "and keeps its flying movement mode (5) in its replicated movement mode");
    CHECK(pawn_detached_at_z(h, from, up.z + 197.0), "the rider is detached 197 cm above where the mount was in the air: it falls from there");
    CHECK(h.s->movement().good().movement_mode == repl::kMoveFalling && std::fabs(h.s->movement().good().z - (up.z + 197.0)) < 1.0, "the pawn's moves start in falling mode from the detachment point");
    g_pawn_ts = 0.05f;
    GoodPosition at = h.s->movement().good();
    const size_t refused = h.s->moves_refused();
    double vz = 0;
    for (int i = 0; i < 12 && at.z > 1100.0; ++i) {
        vz = std::min(4000.0, vz + 980.0 * 0.1 * 1.3);
        at.z -= vz * 0.1;
        g_pawn_ts += 0.1f;
        move_pawn(h, falling_move_at(g_pawn_ts, at.x, at.y, at.z, yaw16_of(at.yaw_degrees)));
        h.step(milliseconds(100));
    }
    CHECK(h.s->moves_refused() == refused, "the fall from the detachment point (up to 4,000 cm/s) is accepted by the pawn's own checker");
    at.z -= 900.0;
    g_pawn_ts += 0.1f;
    move_pawn(h, falling_move_at(g_pawn_ts, at.x, at.y, at.z, yaw16_of(at.yaw_degrees)));
    CHECK(h.s->moves_refused() == refused + 1, "(and a fall of 9,000 cm/s is not)");
}
void test_air_dismount_and_relog() {
    std::printf("stage 4 (modelled): dismount in the air, relog while flying, landing\n");
    verify_air_dismount_fall();
    {
        Harness h(cyb_on_the_ground(), adopt_the_ground);
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 3000.0);
        h.s->finish("the test");
        CHECK(!h.sink.positions.empty() && std::fabs(h.sink.positions.back().z - 1098.0) < 2.0, "the session's end saves the character on the ground, not in the air");
    }
    {
        Harness h(cyb_record());
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(a flyer, no ground in the world)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 3000.0);
        const GoodPosition saved = h.s->last_position();
        CHECK(saved.z > r.z - 300.0, "(without a ground the saved position stays beside the mount)");
    }
}
void adopt_a_bowl(PlayerWorld& w) {
    adopt_the_ground(w);
    std::vector<world::TownFloor::Person> people;
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) people.push_back({i * 2000.0, 15000.0 + j * 2000.0, 1000.0, 1000.0 + 3000.0});
    }
    w.town_floor().set(4242, people);
}
void test_relog_flying_over_a_bowl() {
    std::printf("test-2 fix: relog while flying over a town's bowl: saved on the town's floor, not on the baked ground 30 m under it\n");
    {
        Harness h(cyb_on_the_ground(), adopt_a_bowl);
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 5000.0);
        const GoodPosition up = h.s->movement().good();
        const GoodPosition saved = h.s->last_position();
        const double want = 1000.0 + 3000.0 + PlayerWorld::kBowlSlackCm + 98.0;
        CHECK(up.z > want + 1000.0 && std::fabs(saved.z - want) < 2.0 && std::hypot(saved.x - up.x, saved.y - up.y) < 100.0,
              "the rider 50 m up over the town is saved on the town's floor (the plain + 30 m + the slack + the capsule), not at 1,098 under it");
        h.s->finish("the test");
        CHECK(!h.sink.positions.empty() && std::fabs(h.sink.positions.back().z - want) < 2.0, "the session's end hands that position to the saver");
    }
    {
        Harness h(cyb_on_the_ground(), adopt_the_ground);
        h.play();
        CHECK(ride_on(h, kFlyingSlot), "(a flyer, a plain with no town)");
        if (!h.s->mounted()) return;
        Rider r = start_riding(h);
        take_off(h, r, 5000.0);
        CHECK(std::fabs(h.s->last_position().z - 1098.0) < 2.0, "over the open plain the rider is saved on the baked ground, 1,098");
    }
}
void adopt_a_platform_bowl(PlayerWorld& w) {
    adopt_the_ground(w);
    w.set_town_platforms(tpfix::platforms_at(4242, 0, 0, 6060.0, 0.0, tpfix::grid(30, 0.0, 0.0, 2700.0)));
}
svc::Character cyb_in_a_platform_bowl(double above) {
    svc::Character c = cyb_record();
    c.has_position = true;
    c.position = {0.0, 0.0, 6060.0 + 98.0 + above, 0.0f};
    return c;
}
void test_summon_in_a_platform_bowl() {
    std::printf("the towns' ground: a mount is summoned on a street of a bowl (its floor is the platform's), refused on a tower\n");
    {
        Harness h(cyb_in_a_platform_bowl(0.0), adopt_a_platform_bowl);
        h.play();
        CHECK(std::fabs(h.s->movement().good().z - (6060.0 + 98.0)) < 0.01, "(the character stands on the floor: joined where it was saved)");
        summon_with(h, kMountSlot, 91);
        h.run(milliseconds(1900));
        const world::WorldActor* m = h.mount();
        CHECK(h.s->mounts()->stats().summons == 1 && h.s->mounts()->stats().refused == 0 && m != nullptr && h.s->mounts()->phase() == MountController::Phase::Out,
              "a summon on a street of the town works (it was refused in every town: the baked ground is the pit, 50 m down)");
        CHECK(m && std::fabs(m->z - (6060.0 + MountController::kMountFootOffsetCm)) < 2.0, "the mount stands on the platform's floor, 575 cm ahead of the player");
        desummon_with(h, kMountSlot);
        h.run(milliseconds(1000));
        m = h.mount();
        CHECK(m && m->x > 575.0 + 200.0 && m->x < 2500.0 && std::fabs(m->z - (6060.0 + MountController::kMountFootOffsetCm)) < 2.0, "the desummoned mount runs off over the floor and does not sink into the pit");
    }
    {
        Harness h(cyb_in_a_platform_bowl(3000.0), adopt_a_platform_bowl);
        h.play();
        summon_with(h, kMountSlot, 92);
        h.run(milliseconds(1900));
        CHECK(h.s->mounts()->stats().refused == 1 && h.s->mounts()->stats().summons == 0 && h.mount() == nullptr, "on a tower top 30 m over the floor the refusal stays (the modelled 2.5 m rule)");
    }
    {
        Harness h(cyb_in_a_platform_bowl(300.0), adopt_a_platform_bowl);
        h.play();
        summon_with(h, kMountSlot, 93);
        h.run(milliseconds(1900));
        CHECK(h.s->mounts()->stats().refused == 1, "...and 3 m over the floor, past the 2.5 m a ground mount tolerates, it is refused too");
    }
    {
        Harness h(cyb_in_a_platform_bowl(100.0), adopt_a_platform_bowl);
        h.play();
        summon_with(h, kMountSlot, 94);
        h.run(milliseconds(1900));
        CHECK(h.s->mounts()->stats().summons == 1, "1 m over the floor (a step): allowed");
    }
    {
        Harness h(cyb_in_a_platform_bowl(0.0), adopt_the_ground);
        h.play();
        summon_with(h, kMountSlot, 95);
        h.run(milliseconds(1900));
        CHECK(h.s->mounts()->stats().refused == 1, "(without the platform file: refused, as before)");
    }
}
void adopt_a_platform_bowl_for_the_flight(PlayerWorld& w) {
    adopt_the_ground(w);
    w.set_town_platforms(tpfix::platforms_at(4242, 0, 7000, 4000.0, 0.0, tpfix::grid(60, 0.0, 0.0, 5500.0)));
}
void test_relog_flying_over_a_platform_bowl() {
    std::printf("the towns' ground: relog while flying over a town whose floor is the platform: saved on the floor + the slack\n");
    Harness h(cyb_on_the_ground(), adopt_a_platform_bowl_for_the_flight);
    h.play();
    CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    take_off(h, r, 5000.0);
    const GoodPosition up = h.s->movement().good();
    const GoodPosition saved = h.s->last_position();
    const double want = 4000.0 + PlayerWorld::kBowlSlackCm + 98.0;
    CHECK(up.z > want + 1000.0 && std::fabs(saved.z - want) < 2.0 && std::hypot(saved.x - up.x, saved.y - up.y) < 100.0,
          "the rider 50 m up over the town is saved on the platform's floor (4,000 + the slack + the capsule), 3 m over the street, not at 1,098 under it");
    h.s->finish("the test");
    CHECK(!h.sink.positions.empty() && std::fabs(h.sink.positions.back().z - want) < 2.0, "the session's end hands that position to the saver");
}
aoc::worldd::nav::Geometry town_with_buildings_geometry() {
    aoc::worldd::nav::Geometry g = aoc::worldd::navfix::test_geometry();
    namespace nf = aoc::worldd::navfix;
    nf::place_box(g, 0, -2700, 2700, -2700, 2700, 5900, 6060);
    nf::place_box(g, 0, 1000, 1800, -400, 400, 6060, 6460);
    nf::place_box(g, 0, -1600, -1200, -200, 200, 6060, 8060);
    nf::place_box(g, 0, -800, 1200, 2000, 4000, 6060, 7260);
    return g;
}
aoc::worldd::nav::NavFile build_walking_map(const aoc::worldd::nav::Geometry& g) {
    aoc::worldd::nav::BuildOptions opt;
    opt.threads = 2;
    opt.nav_margin_cm = 3000;
    aoc::worldd::nav::NavFile f;
    aoc::worldd::nav::BuildStats st;
    std::string err;
    if (!aoc::worldd::nav::build_nav(g, 7, opt, &f, &st, &err)) {
        std::printf("  [FAIL] the town's walking map does not build: %s\n", err.c_str());
        std::exit(2);
    }
    return f;
}
void adopt_a_walking_map(PlayerWorld& w, const aoc::worldd::nav::NavFile& file) {
    std::string err;
    auto nav = aoc::worldd::nav::AreaNav::from_file(file, &err);
    if (!nav || !w.adopt_ground(std::move(nav), err)) {
        std::printf("  [FAIL] the ground does not load: %s\n", err.c_str());
        std::exit(2);
    }
}
void adopt_a_town_with_buildings(PlayerWorld& w) {
    static const aoc::worldd::nav::NavFile file = build_walking_map(town_with_buildings_geometry());
    adopt_a_walking_map(w, file);
    w.set_town_platforms(tpfix::platforms_at(4242, 0, 0, 6060.0, 0.0, tpfix::grid(30, 0.0, 0.0, 2700.0)));
}
svc::Character cyb_standing(double x, double y, double feet, float yaw = 0.0f) {
    svc::Character c = cyb_record();
    c.has_position = true;
    c.position = {x, y, feet + 98.0, yaw};
    return c;
}
bool summon_allowed(const svc::Character& c, double* mount_z = nullptr) {
    Harness h(c, adopt_a_town_with_buildings);
    h.play();
    summon_with(h, kMountSlot, 91);
    h.run(milliseconds(1900));
    const bool allowed = h.s->mounts()->stats().summons == 1 && h.s->mounts()->stats().refused == 0;
    if (mount_z) {
        const world::WorldActor* m = h.mount();
        *mount_z = m ? m->z : 0.0;
    }
    return allowed;
}
void test_floors_of_a_town() {
    std::printf("the towns' buildings: the floors of a place (PlayerWorld on a street, a terrace, a tower, a roof)\n");
    Harness h(cyb_standing(0, 1500, 6060), adopt_a_town_with_buildings);
    PlayerWorld& w = *h.fx.world;
    const auto stack = w.floors_at(1400, 0, 5000, 9000);
    CHECK(stack.size() == 2 && std::fabs(stack[0] - 6460.0) < 15.0 && std::fabs(stack[1] - 6060.0) < 15.0, "over the terrace: two floors, its top (6,460) and the street under it (the platform's 6,060, once, not twice)");
    const auto tower = w.floors_at(-1400, 0, 5000, 9000);
    CHECK(tower.size() == 2 && std::fabs(tower[0] - 8060.0) < 15.0 && std::fabs(tower[1] - 6060.0) < 15.0, "on the tower: the top (8,060) and the street");
    const auto street = w.floors_at(0, 1500, 5000, 9000);
    CHECK(street.size() == 1 && std::fabs(street[0] - 6060.0) < 15.0, "on the street: one floor");
    CHECK(w.floors_at(0, 1500, 7000, 9000).empty(), "a range over every floor: none");
    CHECK(w.floors_at(0, 100000, 0, 9000).empty(), "outside the bowl and the map: none");
    const auto under_terrace = w.floor_under(1400, 0, 6460.0, 1000.0, 160.0);
    CHECK(under_terrace && std::fabs(*under_terrace - 6460.0) < 15.0, "standing on the terrace: the terrace");
    const auto under_flyer = w.floor_under(1400, 0, 6460.0 + 5000.0, 60000.0, 150.0);
    CHECK(under_flyer && std::fabs(*under_flyer - 6460.0) < 15.0, "50 m over the terrace: the terrace, not the street under it");
    const auto off_terrace = w.floor_under(2300, 0, 6460.0, 1000.0, 160.0);
    CHECK(off_terrace && std::fabs(*off_terrace - 6060.0) < 15.0, "at the terrace's height beside it: the street, 4 m down");
    CHECK(!w.floor_under(-1400, 1000, 8060.0, 1000.0, 160.0), "at the tower's height beside it: the street is 20 m down, out of reach");
    const auto town_flyer = w.town_ground_at(1400, 0, 6460.0 + 5000.0);
    CHECK(town_flyer && town_flyer->raised && town_flyer->platform && std::fabs(town_flyer->z - 6460.0) < 15.0, "town_ground_at with a height: the terrace under a flyer is a raised floor");
    const auto town_street = w.town_ground_at(0, 1500, 6060.0 + 5000.0);
    CHECK(town_street && !town_street->raised && std::fabs(town_street->z - 6060.0) < 5.0, "...and over the street it is the street");
    const auto town_under = w.town_ground_at(1400, 0, 6060.0 - 5000.0);
    CHECK(town_under && !town_under->raised && std::fabs(town_under->z - 6060.0) < 5.0, "...and a body under the platform is never put on a floor over it: the street");
}
void test_summon_in_a_town_with_buildings() {
    std::printf("the towns' buildings: a mount is summoned on a street and on a terrace, refused at a terrace's edge and on a tower top\n");
    double z = 0;
    CHECK(summon_allowed(cyb_standing(0, 1500, 6060), &z) && std::fabs(z - (6060.0 + MountController::kMountFootOffsetCm)) < 15.0, "on the street, facing the open street: allowed, the mount stands on the street");
    CHECK(summon_allowed(cyb_standing(1125, 0, 6460), &z) && std::fabs(z - (6460.0 + MountController::kMountFootOffsetCm)) < 15.0,
          "on the terrace with the point ahead on the terrace: allowed, the mount stands on the terrace's floor, not 4 m under it on the street");
    CHECK(!summon_allowed(cyb_standing(1700, 0, 6460)), "at the terrace's edge, facing out over the street 4 m down: refused (past the 2.5 m a ground mount tolerates)");
    CHECK(!summon_allowed(cyb_standing(-1400, 0, 8060)), "on the tower top facing along the street: refused, the street is 20 m down");
    CHECK(!summon_allowed(cyb_standing(-1400, 0, 8060, 180.0f)) && !summon_allowed(cyb_standing(-1400, 0, 8060, 90.0f)) && !summon_allowed(cyb_standing(-1400, 0, 8060, -90.0f)),
          "...in every direction (a tower top is smaller than the 5.75 m the mount appears ahead)");
    CHECK(summon_allowed(cyb_standing(100, 3000, 7260), &z) && std::fabs(z - (7260.0 + MountController::kMountFootOffsetCm)) < 15.0,
          "on a roof wide enough to hold the point ahead: allowed like any floor, the mount stands on the roof");
    CHECK(!summon_allowed(cyb_standing(1000, 3000, 7260)), "...and refused at the roof's edge facing out (12 m up)");
    CHECK(!summon_allowed(cyb_standing(1125, 0, 6460 + 1100.0)), "11 m over the terrace in the air: refused, nothing under the point within reach");
}
void test_relog_in_a_town_with_buildings() {
    std::printf("the towns' buildings: a relog on a terrace or a tower comes back there, not in the street below\n");
    {
        Harness h(cyb_standing(1125, 0, 6460), adopt_a_town_with_buildings);
        h.play();
        const GoodPosition g = h.s->movement().good();
        CHECK(std::fabs(g.z - (6460.0 + 98.0)) < 0.01 && std::fabs(g.x - 1125.0) < 0.01, "a character saved on the terrace joins on the terrace (its z is the saved one, not the street's)");
        CHECK(std::fabs(h.s->last_position().z - (6460.0 + 98.0)) < 0.01, "...and the position the session would save is that one");
    }
    {
        Harness h(cyb_standing(-1400, 0, 8060), adopt_a_town_with_buildings);
        h.play();
        CHECK(std::fabs(h.s->movement().good().z - (8060.0 + 98.0)) < 0.01, "a character saved on the tower top joins on the tower top");
    }
    {
        Harness h(cyb_standing(1125, 0, 6060 - 3000.0), adopt_a_town_with_buildings);
        h.play();
        CHECK(std::fabs(h.s->movement().good().z - (6060.0 + 98.0 + PlayerWorld::kBowlSlackCm)) < 0.01, "a character saved 30 m under the street joins on the street (floor + capsule + slack), not on the terrace over it");
    }
}
void adopt_terrace_for_flight(PlayerWorld& w) {
    static const aoc::worldd::nav::NavFile file = [] {
        aoc::worldd::nav::Geometry g = aoc::worldd::navfix::test_geometry();
        aoc::worldd::navfix::place_box(g, 0, -3000, 3000, 500, 13500, 3900, 4000);
        aoc::worldd::navfix::place_box(g, 0, -2000, 2000, 5000, 8000, 4000, 4400);
        return build_walking_map(g);
    }();
    adopt_a_walking_map(w, file);
    w.set_town_platforms(tpfix::platforms_at(4242, 0, 7000, 4000.0, 0.0, tpfix::grid(60, 0.0, 0.0, 5500.0)));
}
void test_relog_flying_over_a_terrace() {
    std::printf("the towns' buildings: a rider flying over a terrace is saved on the terrace\n");
    Harness h(cyb_on_the_ground(), adopt_terrace_for_flight);
    h.play();
    CHECK(ride_on(h, kFlyingSlot), "(the player rides a flyer)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    take_off(h, r, 5000.0);
    const GoodPosition up = h.s->movement().good();
    const GoodPosition saved = h.s->last_position();
    CHECK(up.y > 5000.0 && up.y < 8000.0 && up.z > 4400.0 + 1000.0, "(the rider is 10 m or more over the terrace)");
    CHECK(std::fabs(saved.z - (4400.0 + 98.0)) < 20.0 && std::hypot(saved.x - up.x, saved.y - up.y) < 100.0,
          "it is saved on the terrace's floor (4,400 + the capsule), not on the street 4 m under it (4,000 + the slack + the capsule)");
}
void test_mounts_remain() {
    std::printf("stage 4: a mount that has appeared stays through a fight (a hit never desummons it)\n");
    Harness h(cyb_record());
    h.play();
    summon_with(h, kMountSlot, 71);
    h.run(milliseconds(300));
    CHECK(h.mount() != nullptr, "(the mount has opened, 104 ms in; its cast has 1.2 s to go)");
    const size_t cancelled = h.s->mounts()->stats().cancelled, desummons = h.s->mounts()->stats().desummons;
    for (int i = 0; i < 16; ++i) {
        h.s->take_damage(h.conn, 1.0, h.t);
        h.run(milliseconds(500));
    }
    CHECK(mount_count(h) == 1 && h.s->mounts()->phase() == MountController::Phase::Out && h.s->mounts()->stats().cancelled == cancelled && h.s->mounts()->stats().desummons == desummons,
          "8 s of hits, through the end of the cast: the mount is still out, nothing cancelled or desummoned");
    walk_up_to_mount(h);
    get_on(h);
    h.run(milliseconds(200));
    CHECK(h.s->mounted(), "(and it can be ridden)");
    for (int i = 0; i < 10; ++i) {
        h.s->take_damage(h.conn, 1.0, h.t);
        h.run(milliseconds(300));
    }
    CHECK(h.s->mounted() && mount_count(h) == 1 && h.s->dismounts() == 0, "hits while mounted land on the mount and do not put the rider off");
}
void test_distance_desummon() {
    std::printf("stage 4 (modelled): a mount left far behind is desummoned\n");
    Harness h(cyb_record());
    h.play();
    h.s->set_desummon_distance(3000.0);
    summon_with(h, kMountSlot, 81);
    h.run(milliseconds(1900));
    CHECK(h.mount() && h.s->mounts()->phase() == MountController::Phase::Out, "(a mount is out)");
    const GoodPosition g = h.s->movement().good();
    walk_pawn_to(h, g.x - 1800.0, g.y);
    h.run(milliseconds(300));
    CHECK(h.s->distance_desummons() == 0 && h.s->mounts()->phase() == MountController::Phase::Out, "18 m away (under the 30 m limit) the mount stays");
    walk_pawn_to(h, g.x - 4200.0, g.y);
    h.run(milliseconds(300));
    CHECK(h.s->distance_desummons() == 1 && h.s->mounts()->phase() == MountController::Phase::Desummoning, "past the limit the mount is desummoned");
    h.run(milliseconds(3400));
    CHECK(mount_count(h) == 0, "(it dissolves and is gone)");
}
struct RealRoad {
    int tier = 0;
    double half = 0;
    std::vector<std::pair<double, double>> xy;
};
void parse_real_road_line(const std::string& line, std::vector<RealRoad>& out) {
    if (line.empty() || line[0] == '#') return;
    std::vector<std::string> cells;
    size_t a = 0;
    while (a <= line.size()) {
        size_t b = line.find('\t', a);
        if (b == std::string::npos) b = line.size();
        cells.push_back(line.substr(a, b - a));
        a = b + 1;
    }
    if (cells.size() < 5 || cells[0] != "road") return;
    RealRoad r;
    r.tier = std::atoi(cells[1].c_str());
    r.half = std::atof(cells[2].c_str());
    for (size_t i = 3; i < cells.size(); ++i) {
        const size_t comma = cells[i].find(',');
        if (comma == std::string::npos) continue;
        r.xy.emplace_back(std::atof(cells[i].substr(0, comma).c_str()), std::atof(cells[i].substr(comma + 1).c_str()));
    }
    out.push_back(std::move(r));
}
std::vector<RealRoad> read_real_roads(const std::string& path) {
    std::vector<RealRoad> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::string text;
    char buf[65536];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        parse_real_road_line(text.substr(pos, eol - pos), out);
        pos = eol + 1;
    }
    return out;
}
void verify_real_roads_map(const std::vector<RealRoad>& roads, const world::RoadMap& map) {
    size_t t1 = 0, t2 = 0, other = 0;
    double metres = 0, narrow = 1e9;
    bool self = true;
    for (const RealRoad& r : roads) {
        (r.tier == 1 ? t1 : r.tier == 2 ? t2 : other)++;
        narrow = std::min(narrow, r.half);
        for (size_t i = 1; i < r.xy.size(); ++i) metres += std::hypot(r.xy[i].first - r.xy[i - 1].first, r.xy[i].second - r.xy[i - 1].second) / 100.0;
        for (const auto& [x, y] : r.xy) self = self && map.tier_at(x, y) >= r.tier;
    }
    CHECK(t1 >= 5 && t2 >= 20 && other == 0, "the area's roads are tier 1 (the Riverlands Artery at level 0) and tier 2 (Lions Hold's)");
    CHECK(metres > 25000.0 && metres < 45000.0, "about 34 km of road in all (the region's 91 roads at Wilderness: 24.7 km of tier 1, 9.1 km of tier 2)");
    CHECK(narrow >= 500.0, "no road is narrower than half of max(RoadWidth, 1000): the overlap's half width is at least 500 cm");
    CHECK(self, "every sample of every road lies on a road of at least its tier");
    CHECK(map.tier_at(map.bounds().max_x + 300000.0, map.bounds().max_y + 300000.0) == -1, "a point 3 km outside the roads' extent is on none");
}
void ride_to_point(Harness& h, Rider& r, double tx, double ty) {
    int guard = 0;
    while (std::hypot(tx - r.x, ty - r.y) > 1.0 && guard++ < 400) {
        const double dist = std::hypot(tx - r.x, ty - r.y);
        const double step = std::min(dist, 99.0);
        ride_step(h, r, (tx - r.x) / dist * step, (ty - r.y) / dist * step);
    }
}
const RealRoad* find_nearest_tier2_road(const std::vector<RealRoad>& roads, size_t& best_i) {
    const double cx = -777664.5, cy = 616578.69;
    const RealRoad* best = nullptr;
    double best_d = 1e18;
    for (const RealRoad& r : roads) {
        if (r.tier != 2 || r.xy.size() < 40) continue;
        for (size_t i = 0; i + 30 < r.xy.size(); ++i) {
            const double d = std::hypot(r.xy[i].first - cx, r.xy[i].second - cy);
            if (d < best_d) {
                best_d = d;
                best = &r;
                best_i = i;
            }
        }
    }
    return best;
}
bool find_off_road_point(const world::RoadMap& map, const Rider& r, double nx, double ny, std::pair<double, double>& off) {
    for (const double away : {2500.0, 4000.0, 6000.0, 9000.0}) {
        for (const double side : {1.0, -1.0}) {
            if (map.tier_at(r.x + nx * away * side, r.y + ny * away * side) < 0) {
                off = {r.x + nx * away * side, r.y + ny * away * side};
                return true;
            }
        }
    }
    return false;
}
void verify_real_road_on_and_off(Harness& h, Rider& r, const world::RoadMap& map, const RealRoad& best, size_t from_vertex) {
    const uint32_t mch = h.mount_ch();
    const content::MountData& data = h.fx.world->mounts()->data();
    ride_to_point(h, r, best.xy[from_vertex].first, best.xy[from_vertex].second);
    CHECK(h.s->ride()->road_tier() == 2 && std::fabs(mult_of(*h.mount()).value_or(0) - 1.2) < 1e-6, "on the real road the mount has Road_T2_Speed_Mount: MoveSpeedMult 1.2");
    const auto fx = items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects");
    CHECK(fx.size() == 1 && data.road(2) && fx[0].values.at(2).get_if<int64_t>() && *fx[0].values.at(2).get_if<int64_t>() == data.road(2)->effect, "the effect is the road's tier-2 effect, one item");
    const size_t refused = h.s->moves_refused();
    for (size_t i = from_vertex + 1; i < std::min(best.xy.size(), from_vertex + 25); ++i) ride_to_point(h, r, best.xy[i].first, best.xy[i].second);
    CHECK(h.s->moves_refused() == refused && h.s->ride()->road_tier() >= 1, "riding along 25 samples of the road (5 km... 50 m each 200 cm... 48 m): no move refused, the effect stays on");
    size_t effect_updates = 0;
    for (const fake::Delivered* d : chains_on(h, mch, 0)) {
        for (const auto& [id, o] : states_of(*d, h.mount()->actor)) {
            const schema::NetField* fe = o.schema ? o.schema->field("CurrentEffects") : nullptr;
            if (fe && o.arrays.find(fe->index)) ++effect_updates;
        }
    }
    CHECK(effect_updates >= 1 && effect_updates <= 3, "the effect was added once (at the mount-up, the window starts there) and not re-sent as the mount rode along the road (no flapping at the samples)");
    const auto& q0 = best.xy[std::min(best.xy.size() - 2, from_vertex + 24)];
    const auto& q1 = best.xy[std::min(best.xy.size() - 1, from_vertex + 25)];
    const double qlen = std::hypot(q1.first - q0.first, q1.second - q0.second);
    const double nx = -(q1.second - q0.second) / qlen, ny = (q1.first - q0.first) / qlen;
    std::pair<double, double> off{0.0, 0.0};
    const bool found_off = find_off_road_point(map, r, nx, ny, off);
    CHECK(found_off, "(a point within 90 m of the road that is on no road)");
    if (!found_off) return;
    ride_to_point(h, r, off.first, off.second);
    CHECK(map.tier_at(off.first, off.second) < 0 && h.s->ride()->road_tier() == -1 && mult_of(*h.mount()).value_or(0) == 1.0 && items_of(*h.mount(), "UAoCStatsComponent", "CurrentEffects").empty(),
          "off every road the effect is removed, MoveSpeedMult is back to 1");
    ride_to_point(h, r, q1.first, q1.second);
    CHECK(h.s->ride()->road_tier() >= 1, "back on the road it is added again");
}
void test_real_roads() {
    std::printf("stage 4: the roads of the area (mapc roads, contentc roads): the real file, and a mount riding along one\n");
    const std::string path = MOUNT_ROADS_PATH;
    const std::vector<RealRoad> roads = read_real_roads(path);
    CHECK(roads.size() >= 30, "dist/content/roads.tsv holds the area's roads (91 at Wilderness over the whole region: 54 tier 1, 37 tier 2)");
    if (roads.empty()) return;
    world::RoadMap map;
    std::string error;
    CHECK(map.load(path, error) && map.roads() == roads.size() && map.node_level() == 0, "world's loader reads it, and the header says the tiers were resolved at node level 0");
    verify_real_roads_map(roads, map);
    size_t best_i = 0;
    const RealRoad* best = find_nearest_tier2_road(roads, best_i);
    CHECK(best != nullptr, "(a tier-2 road with 30 samples ahead of its nearest point)");
    if (!best) return;
    const auto& p0 = best->xy[best_i];
    const auto& p1 = best->xy[best_i + 1];
    const double yaw = std::atan2(p1.second - p0.second, p1.first - p0.first) * 180.0 / 3.14159265358979;
    svc::Character rec = cyb_record();
    rec.has_position = true;
    rec.position = {p0.first, p0.second, 3000.0, static_cast<float>(yaw)};
    Harness h(rec, [&](PlayerWorld& w) {
        std::string e;
        if (!w.load_roads(path, e)) std::printf("  [FAIL] the roads do not load into the world: %s\n", e.c_str());
    });
    h.play();
    CHECK(h.fx.world->roads().roads() == roads.size(), "(the world holds the real roads)");
    CHECK(ride_on(h, kMountSlot), "(the player rides the terror bird, summoned in front of it on the road)");
    if (!h.s->mounted()) return;
    Rider r = start_riding(h);
    size_t from_vertex = best_i;
    double nearest = 1e18;
    for (size_t i = best_i; i < best->xy.size(); ++i) {
        const double d = std::hypot(best->xy[i].first - r.x, best->xy[i].second - r.y);
        if (d < nearest) {
            nearest = d;
            from_vertex = i;
        }
    }
    verify_real_road_on_and_off(h, r, map, *best, from_vertex);
}
} // namespace
int main() {
    spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
    std::string error;
    {
        auto real = content::ContentDb::open(real_db_path(), error);
        const auto data = real ? real->mount_data(error) : std::nullopt;
        if (!data) {
            std::printf("  [FAIL] the real content.db has no mount tables (%s): %s\n", real_db_path().c_str(), error.c_str());
            return 2;
        }
        for (const content::MountRow& r : data->mounts) {
            if (r.item_name == "Mount_Grounded_TerrorBird_0") g_mount_record = r.item;
            if (r.item_name == "Mount_Grounded_Horse_0") g_other_mount_record = r.item;
            if (r.kind == "aquatic" && !g_aquatic_record) g_aquatic_record = r.item;
            if (r.kind == "flying" && !g_flying_record) g_flying_record = r.item;
        }
    }
    test_flight_session();
    test_no_road_bonus_in_the_air();
    test_flight_on_a_ground_mount();
    test_swimming_session();
    test_water_dismount();
    test_air_dismount_and_relog();
    test_relog_flying_over_a_bowl();
    test_summon_in_a_platform_bowl();
    test_relog_flying_over_a_platform_bowl();
    test_floors_of_a_town();
    test_summon_in_a_town_with_buildings();
    test_relog_in_a_town_with_buildings();
    test_relog_flying_over_a_terrace();
    test_mounts_remain();
    test_distance_desummon();
    test_real_roads();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
