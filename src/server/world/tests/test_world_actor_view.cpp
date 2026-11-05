// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_actor_view_common.h"

#include "net/connection.h"
#include "protocol/repl/replicator.h"
#include "world/actor_registry.h"
#include "world/relevancy.h"

#include <set>
#include <vector>

static void test_registry() {
    std::printf("the registry\n");
    world::ActorRegistry reg;
    CHECK(reg.add(make_npc(1000, 0, 0)) && reg.size() == 1, "an actor is added");
    CHECK(!reg.add(make_npc(1000, 5, 5)), "the same id twice is refused");
    CHECK(!reg.add(make_npc(1001, 0, 0)), "an odd id (a static object's) is refused");
    world::WorldActor bad = make_npc(1002, 0, 0);
    bad.id = 1004;
    CHECK(!reg.add(std::move(bad)), "an id that is not the actor's NetGUID is refused");
    world::WorldActor nan = make_npc(1006, 0, 0);
    nan.x = std::nan("");
    CHECK(!reg.add(std::move(nan)), "a position that is not a number is refused");
    CHECK(reg.add(make_npc(2000, 9999, 0)) && reg.add(make_npc(2002, 10001, 0)) && reg.add(make_npc(2004, -1, -1)) && reg.add(make_npc(2006, -10001, -10001)),
          "actors on either side of a cell edge and at negative coordinates");

    std::vector<uint64_t> ids;
    reg.query(0, 0, 10000, ids);
    std::set<uint64_t> got(ids.begin(), ids.end());
    CHECK(got == std::set<uint64_t>({1000, 2000, 2004}), "a 100 m query around the origin finds the three within it (9,999 m is in, 10,001 is out, -1 is in)");
    ids.clear();
    reg.query(10000, 0, 100, ids);
    CHECK(ids.size() == 2, "a small query across a cell edge finds both neighbours");
    ids.clear();
    reg.query(-10000, -10000, 2, ids);
    CHECK(ids.size() == 1 && ids[0] == 2006, "a query at negative coordinates");
    ids.clear();
    reg.query(0, 0, 1e12, ids);
    CHECK(ids.size() == reg.size(), "a radius larger than the world walks the grid whole");
    ids.clear();
    reg.query(std::nan(""), 0, 100, ids);
    reg.query(0, 0, -5, ids);
    CHECK(ids.empty(), "a query with no number in it finds nothing");

    const uint32_t v = reg.find(1000)->version, rv = reg.find(1000)->reliable_version;
    CHECK(reg.move(1000, 25000, 0, 12000, 90) && reg.find(1000)->version == v + 1 && reg.find(1000)->reliable_version == rv, "a move counts as a change that need not be reliable");
    ids.clear();
    reg.query(0, 0, 10000, ids);
    CHECK(std::set<uint64_t>(ids.begin(), ids.end()) == std::set<uint64_t>({2000, 2004}), "the grid follows a move to another cell");
    ids.clear();
    reg.query(25000, 0, 100, ids);
    CHECK(ids.size() == 1 && ids[0] == 1000, "...and finds the actor at its new place");
    CHECK(reg.modify(1000, world::Change::State, [](world::WorldActor& a) { a.npc.level = 15; }) && reg.find(1000)->npc.level == 15 &&
              reg.find(1000)->version == v + 2 && reg.find(1000)->reliable_version == rv + 1,
          "a state change moves both counters");
    CHECK(!reg.modify(77, world::Change::State, [](world::WorldActor&) {}) && !reg.move(77, 0, 0, 0, 0) && !reg.move(1000, std::nan(""), 0, 0, 0), "an unknown actor or a bad position changes nothing");

    std::vector<uint64_t> told;
    const uint32_t h1 = reg.on_remove([&](const world::WorldActor& a) { told.push_back(a.id); });
    const uint32_t h2 = reg.on_remove([&](const world::WorldActor& a) { told.push_back(a.id + 1); });
    CHECK(reg.remove(2000) && told == std::vector<uint64_t>({2000, 2001}) && reg.find(2000) == nullptr && reg.size() == 4, "removing tells the listeners first, in order");
    reg.off_remove(h1);
    told.clear();
    CHECK(reg.remove(2002) && told == std::vector<uint64_t>({2003}) && !reg.remove(2002), "a listener taken off hears nothing; removing twice is refused");
    reg.off_remove(h2);
    CHECK(reg.count(world::ActorKind::Npc) == 3, "the count by kind");
}

// ---- relevancy ------------------------------------------------------------------------------------------

static void test_classify() {
    std::printf("relevancy rings\n");
    using world::ClientState;
    using world::Ring;
    const world::RelevancyConfig cfg;   // 150 m, 350 m, 10 m
    const auto c = [&](double d, ClientState prior, double awake = 0, bool never = false) { return world::classify(cfg, d, awake, never, prior); };
    CHECK(c(14999, ClientState::None) == Ring::Awake && c(15001, ClientState::None) == Ring::Snapshot && c(35001, ClientState::None) == Ring::Out, "the edges: 150 m and 350 m");
    CHECK(c(15500, ClientState::Awake) == Ring::Awake && c(16001, ClientState::Awake) == Ring::Snapshot && c(15500, ClientState::Dormant) == Ring::Snapshot,
          "an open actor keeps its ring 10 m past the edge; one that is not open does not get it");
    CHECK(c(35500, ClientState::Dormant) == Ring::Snapshot && c(36001, ClientState::Dormant) == Ring::Out && c(35500, ClientState::None) == Ring::Out,
          "a dormant one stays in the snapshot ring 10 m past its edge, a new one does not enter past it");
    CHECK(c(11901, ClientState::None, 11900) == Ring::Snapshot && c(11899, ClientState::None, 11900) == Ring::Awake, "a class with its own radius (119 m) uses it");
    CHECK(c(15001, ClientState::None, 0, true) == Ring::Out && c(15500, ClientState::Awake, 0, true) == Ring::Awake && c(16001, ClientState::Awake, 0, true) == Ring::Out,
          "an actor that is never dormant is awake or out, with the same hysteresis");
    CHECK(world::query_radius(cfg) == 36000 && world::query_radius(cfg, 50000) == 51000, "the registry query reaches the outermost ring and the largest class radius");
}

static void check_view_rings_wake_and_leave(world::ActorRegistry& reg, ActorView& v, Pair& p) {
    run(v, p, 12000, 0, 1);
    auto s = take(p);
    const uint32_t reopened = v.channel_of(1002);
    size_t reopen_bits = 0;
    for (const Seen& x : s) if (x.open && x.channel == reopened) reopen_bits = x.bits;
    CHECK(reopened != 0 && v.state_of(1002) == world::ClientState::Awake && v.stats().wakes == 1 && reopen_bits > 0, "walking up to a dormant actor opens it again");
    CHECK(v.state_of(1004) == world::ClientState::Snapshot, "the actor that came into the snapshot ring got its snapshot");
    CHECK(reopened == 8, "the index of the Dormancy close was acknowledged, so the lowest free channel is that one");
    world::WorldActor* a = reg.find(1002);
    repl::ActorShadow fresh;
    repl::ExportAcks all;
    auto em = repl::open_actor(a->actor, fresh, all);
    size_t expect_bits = 0;
    if (em) {
        expect_bits = em->payload.bit_pos();
        for (const auto& x : em->exports) expect_bits += x.bit_pos();
    }
    auto em_acked = repl::open_actor(a->actor, fresh, v.export_acks());
    size_t expect_acked = 0;
    if (em_acked) {
        expect_acked = em_acked->payload.bit_pos();
        for (const auto& x : em_acked->exports) expect_acked += x.bit_pos();
    }
    CHECK(em && em_acked && (reopen_bits == expect_acked || reopen_bits == expect_bits) && expect_acked <= expect_bits,
          "the re-open is the whole state again: the bits of an open from an empty shadow (with the exports the client acknowledged left out)");

    run(v, p, -20000, 0, 1);
    s = take(p);
    uint32_t reason = 99;
    for (const Seen& x : s) if (x.close && x.channel == 7) reason = x.reason;
    CHECK(v.state_of(1000) == world::ClientState::Dormant && reason == kCloseDormancy, "an awake actor that leaves the awake ring is closed with Dormancy: the client keeps it");
    run(v, p, -20000, 0, 3);
    take(p);
    run(v, p, -50000, 0, 1);
    s = take(p);
    CHECK(v.state_of(1000) == world::ClientState::None && s.empty() && v.stats().forgotten >= 1, "a dormant one that leaves the snapshot ring is forgotten with no message (the client keeps the actor)");
    run(v, p, 10000, 0, 1);
    CHECK(v.state_of(1000) == world::ClientState::Awake, "coming back opens it again");
}

static void test_view_rings() {
    std::printf("the view: awake, snapshot and out\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    reg.add(make_npc(1000, 10000, 0));
    reg.add(make_npc(1002, 20000, 0));
    reg.add(make_npc(1004, 40000, 0));
    run(v, p, 0, 0, 1);
    auto s = take(p);
    CHECK(v.channel_of(1000) == 7 && v.state_of(1000) == world::ClientState::Awake, "the first free channel is 7: 3-6 are the session's own");
    CHECK(v.state_of(1002) == world::ClientState::Snapshot && v.state_of(1004) == world::ClientState::None && v.tracked() == 2, "the snapshot is opened, the actor at 400 m is not even tracked");
    size_t opens = 0, closes = 0;
    for (const Seen& x : s) { opens += x.open ? 1 : 0; closes += x.close ? 1 : 0; }
    CHECK(opens == 2 && closes == 0, "two opens went out and no close yet (the Dormancy close is 134 ms after the open)");
    CHECK(v.stats().opens == 1 && v.stats().snapshots == 1, "counted as one open and one snapshot");

    p.t += milliseconds(10);
    run(v, p, 0, 0, 1);
    s = take(p);
    closes = 0;
    uint32_t reason = 99, closed_channel = 0;
    for (const Seen& x : s) if (x.close) { ++closes; reason = x.reason; closed_channel = x.channel; }
    CHECK(closes == 1 && reason == kCloseDormancy && closed_channel == 8, "the snapshot is closed with Dormancy on its channel");
    CHECK(v.state_of(1002) == world::ClientState::Dormant && v.channel_of(1002) == 0 && v.state_of(1000) == world::ClientState::Awake, "it is dormant now; the awake one is unchanged");

    run(v, p, 0, 0, 20);
    s = take(p);
    CHECK(s.empty(), "nothing is sent while nothing changes");
    reg.move(1002, 20100, 0, reg.find(1002)->z, 45);
    reg.move(1002, 20200, 0, reg.find(1002)->z, 45);
    run(v, p, 0, 0, 5);
    s = take(p);
    CHECK(s.empty() && v.stats().snapshots == 1 && v.state_of(1002) == world::ClientState::Dormant, "a dormant actor that walked within the snapshot ring is not sent again");
    reg.modify(1002, world::Change::State, [](world::WorldActor& a) { a.npc.level = 9; });
    run(v, p, 0, 0, 1);
    s = take(p);
    opens = 0;
    for (const Seen& x : s) opens += x.open ? 1 : 0;
    CHECK(opens == 1 && v.stats().snapshots == 2 && v.stats().wakes == 0 && v.state_of(1002) == world::ClientState::Snapshot, "a dormant actor whose state changed is sent as a new snapshot (not a wake)");
    p.t += milliseconds(10);
    run(v, p, 0, 0, 3);
    s = take(p);
    closes = 0;
    for (const Seen& x : s) closes += x.close ? 1 : 0;
    CHECK(closes == 1 && v.state_of(1002) == world::ClientState::Dormant && v.channel_of(1002) == 0, "...and closed with Dormancy 134 ms later");
    check_view_rings_wake_and_leave(reg, v, p);
}

static void test_wide_actor_leaves_the_query_alone() {
    std::printf("the view: a wide actor (a town's replicator) does not widen the registry query\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    const world::RelevancyConfig cfg = view_config().relevancy;
    CHECK(v.query_radius_cm() == world::query_radius(cfg), "(a new view asks the registry for the snapshot ring and its hysteresis)");
    world::WorldActor wide = make_npc(1000, 100000, 0);
    wide.awake_radius_cm = 145000;
    reg.add(std::move(wide));
    reg.add(make_npc(1010, 10000, 0));
    run(v, p, 0, 0, 3);
    CHECK(reg.wide_count() == 1 && v.state_of(1000) == world::ClientState::Awake, "the wide actor is found at 1 km (query_wide) and awake");
    CHECK(v.query_radius_cm() == world::query_radius(cfg), "having seen it leaves the grid query at 360 m (a 1.46 km query would visit a quarter of the world's cells for good)");
    world::WorldActor big = make_npc(1020, 30000, 0);
    big.awake_radius_cm = 38000;
    reg.add(std::move(big));
    run(v, p, 0, 0, 3);
    CHECK(v.query_radius_cm() == world::query_radius(cfg, 38000), "an actor of 380 m still raises it (only the grid finds that one)");
}

static void test_view_updates() {
    std::printf("the view: updates, reliability, hysteresis\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    reg.add(make_npc(1000, 5000, 0));
    run(v, p, 0, 0, 2);
    take(p);
    const uint32_t ch = v.channel_of(1000);
    CHECK(ch == 7, "opened on 7");
    reg.modify(1000, world::Change::State, [](world::WorldActor& a) {
        for (repl::Subobject& s : a.actor.subobjects) {
            if (std::string(s.name) == "CombatInfo") s.obj.state.set(*s.obj.schema, "bIsDead", repl::Value::boolean(true));
        }
    });
    run(v, p, 0, 0, 2);
    auto s = take(p);
    CHECK(s.size() == 1 && s[0].channel == ch && !s[0].open && s[0].reliable && s[0].bits < 200, "a state change is one small reliable update on the actor's channel");
    CHECK(v.stats().updates == 1, "counted");
    reg.move(1000, 5100, 0, 12000, 0);
    reg.modify(1000, world::Change::Movement, [](world::WorldActor& a) {
        repl::RepMovement m;
        m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, 5100 * 100, 0, 1200000};
        m.rotation = repl::Rot{0, 64, 0};
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 0, 0};
        a.actor.obj.state.set(*a.actor.obj.schema, "ReplicatedMovement", repl::Value(m));
    });
    run(v, p, 0, 0, 2);
    s = take(p);
    CHECK(s.size() == 1 && !s[0].reliable && !s[0].open && s[0].bits < 400 && v.stats().updates_unreliable == 1, "a movement alone is an unreliable update");
    run(v, p, 0, 0, 5);
    CHECK(take(p).empty(), "an unchanged actor sends nothing");
    reg.touch(*reg.find(1000), world::Change::State);
    run(v, p, 0, 0, 2);
    CHECK(take(p).empty() && v.stats().updates == 1, "a change that left every value as it was sends nothing");

    reg.add(make_npc(1002, 15000, 0));
    size_t flaps = 0;
    run(v, p, 0, 0, 2);
    take(p);
    for (int i = 0; i < 60; ++i) {
        const double x = (i % 2) ? -400.0 : 400.0;
        run(v, p, x, 0, 1);
        for (const Seen& e : take(p)) flaps += (e.open || e.close) ? 1 : 0;
    }
    CHECK(flaps <= 2, "pacing across the edge of the awake ring (148 m, 154 m) opens and closes at most once");
}

static void test_view_removal() {
    world::ActorRegistry reg3;
    Pair p3;
    ActorView v3(reg3, view_config(), reserved_3_to_6());
    reg3.add(make_npc(1000, 5000, 0));
    reg3.add(make_npc(1002, 20000, 0));
    run(v3, p3, 0, 0, 5);
    take(p3);
    CHECK(v3.state_of(1000) == world::ClientState::Awake && v3.state_of(1002) == world::ClientState::Dormant, "an awake actor and a dormant one");
    reg3.remove(1000);
    run(v3, p3, 0, 0, 1);
    auto s = take(p3);
    uint32_t reason = 99;
    for (const Seen& x : s) if (x.close) reason = x.reason;
    CHECK(reason == kCloseDestroyed && v3.tracked() == 1 && v3.stats().closes_destroyed == 1, "an awake actor that is removed is closed with Destroyed");
    reg3.remove(1002);
    run(v3, p3, 0, 0, 2);
    s = take(p3);
    size_t opens = 0, destroyed = 0;
    for (const Seen& x : s) { opens += x.open ? 1 : 0; destroyed += (x.close && x.reason == kCloseDestroyed) ? 1 : 0; }
    CHECK(opens == 1 && destroyed == 1 && v3.tracked() == 0, "a dormant one is opened once more and closed with Destroyed so that the client forgets it");

    ActorView* v4 = new ActorView(reg3, view_config(), reserved_3_to_6());
    delete v4;
    reg3.add(make_npc(1010, 0, 0));
    CHECK(reg3.remove(1010), "a finished view leaves no listener behind");
}

static void test_view_never_dormant_and_removal() {
    std::printf("the view: a never-dormant actor, removal, channels held\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    world::WorldActor fx = make_npc(3000, 5000, 0, "Effect");
    fx.kind = world::ActorKind::Effect;
    fx.never_dormant = true;
    reg.add(std::move(fx));
    run(v, p, 0, 0, 2);
    take(p);
    CHECK(v.state_of(3000) == world::ClientState::Awake, "a never-dormant actor in range is awake");
    run(v, p, 30000, 0, 1);
    auto s = take(p);
    uint32_t reason = 99;
    for (const Seen& x : s) if (x.close) reason = x.reason;
    CHECK(v.state_of(3000) == world::ClientState::None && reason == kCloseRelevancy, "it leaves with a Relevancy close, never a Dormancy one");
    run(v, p, 0, 0, 1);
    CHECK(v.state_of(3000) == world::ClientState::Awake, "and opens again when the player returns");

    world::ActorRegistry reg2;
    Pair p2;
    ActorView v2(reg2, view_config(), reserved_3_to_6());
    reg2.add(make_npc(1000, 5000, 0));
    reg2.add(make_npc(1002, 90000, 0));
    v2.tick(p2.s, 0, 0, p2.t);
    p2.step(&v2);
    CHECK(v2.channel_of(1000) == 7, "the first actor is on 7");
    v2.tick(p2.s, 100000, 0, p2.t + milliseconds(100));
    CHECK(p2.s.channel_closing(7) && v2.channel_of(1000) == 0, "the close is queued: the channel is closing");
    reg2.add(make_npc(1004, 99000, 0));
    v2.tick(p2.s, 100000, 0, p2.t + milliseconds(200));
    CHECK(v2.channel_of(1004) != 7 && v2.channel_of(1004) >= 8, "a closing channel's index is not reused until the close is acknowledged");
    p2.step(&v2);
    p2.step(&v2);
    CHECK(!p2.s.channel_closing(7), "the acknowledgement frees it");
    reg2.add(make_npc(1006, 99500, 0));
    v2.tick(p2.s, 100000, 0, p2.t + milliseconds(400));
    CHECK(v2.channel_of(1006) == 7, "and the next open takes it: the lowest free index");
    test_view_removal();
}

static void test_view_cap_and_exports() {
    std::printf("the view: the cap on opens, nearest first; exports\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    ActorView v(reg, cfg, reserved_3_to_6());
    for (int i = 0; i < 100; ++i) reg.add(make_npc(10000 + i * 100, 5000 + i * 100, 0));
    v.tick(p.s, 0, 0, p.t);
    CHECK(v.stats().opens == 16 && v.stats().capped == 1, "the first pass opens 16 of 100 (the cap)");
    bool nearest = true;
    for (int i = 0; i < 100; ++i) nearest = nearest && ((i < 16) == (v.state_of(10000 + i * 100) == world::ClientState::Awake));
    CHECK(nearest, "the 16 are the nearest");
    for (int i = 0; i < 7; ++i) run(v, p, 0, 0, 1);
    CHECK(v.stats().opens == 100 && v.count(world::ClientState::Awake) == 100, "seven more passes open the rest");
    CHECK(v.channels_in_use() == 100, "100 channels in use");
    std::set<uint32_t> channels;
    for (int i = 0; i < 100; ++i) channels.insert(v.channel_of(10000 + i * 100));
    CHECK(channels.size() == 100 && *channels.begin() == 7 && *channels.rbegin() == 106, "they are 7 to 106, each once");

    world::ActorRegistry reg2;
    Pair p2;
    ActorView v2(reg2, view_config(), reserved_3_to_6());
    reg2.add(make_npc(1000, 5000, 0, "Wolf"));
    reg2.add(make_npc(1002, 6000, 0, "Wolf"));
    std::vector<size_t> bits;
    v2.set_trace([&](uint32_t, bool open, bool, size_t b) { if (open) bits.push_back(b); });
    v2.tick(p2.s, 0, 0, p2.t);
    CHECK(bits.size() == 2 && !v2.export_acks().acked(reg2.find(1000)->actor.open.archetype[0].guid), "both opens of the first pass carry the archetype's export (nothing acknowledged yet)");
    p2.step(&v2);
    p2.step(&v2);
    CHECK(v2.export_acks().acked(reg2.find(1000)->actor.open.archetype[0].guid), "the archetype is marked once the client acknowledged the packet");
    const size_t first_bits = bits[0];
    reg2.add(make_npc(1004, 7000, 0, "Wolf"));
    bits.clear();
    run(v2, p2, 0, 0, 1);
    CHECK(bits.size() == 1 && bits[0] < first_bits, "a later open of the same archetype is smaller: the export the client acknowledged is left out");
}

static void test_view_owner_and_component_index() {
    std::printf("the view: a player's own actors; the registry's component index\n");
    for (const uint64_t player : {7ull, 8ull}) {
        world::ActorRegistry reg;
        Pair p;
        ActorView::Config cfg = view_config();
        cfg.player_id = player;
        ActorView v(reg, cfg, reserved_3_to_6());
        world::WorldActor fx = make_npc(3000, 5000, 0, "Effect");
        fx.kind = world::ActorKind::Effect;
        fx.never_dormant = true;
        fx.owner = 7;
        fx.open_exports.push_back({link(0xC001, "CharacterMesh0", false, 0x77aa)});
        reg.add(std::move(fx));
        run(v, p, 0, 0, 2);
        const auto s = take(p);
        if (player == 7) {
            CHECK(v.state_of(3000) == world::ClientState::Awake && !s.empty() && s[0].open, "player 7's effect is opened for player 7");
        } else {
            CHECK(v.state_of(3000) == world::ClientState::None && s.empty() && v.tracked() == 0, "...and never sent to player 8");
        }
    }
    {
        size_t with = 0, without = 0;
        for (const bool has : {true, false}) {
            world::ActorRegistry reg;
            Pair p;
            ActorView v(reg, view_config(), reserved_3_to_6());
            world::WorldActor fx = make_npc(3000, 5000, 0, "Effect");
            fx.kind = world::ActorKind::Effect;
            fx.never_dormant = true;
            if (has) fx.open_exports.push_back({link(0xC001, "CharacterMesh0", false, 0x77aa)});
            reg.add(std::move(fx));
            std::vector<size_t> bits;
            v.set_trace([&](uint32_t, bool open, bool, size_t b) { if (open) bits.push_back(b); });
            v.tick(p.s, 0, 0, p.t);
            (has ? with : without) = bits.empty() ? 0 : bits[0];
        }
        CHECK(with > without && without > 0, "an actor with an export to carry has a longer open than the same actor without");
    }
    world::ActorRegistry reg;
    world::WorldActor a = make_npc(1000, 0, 0);
    a.npc.interact_component = 1016;
    world::WorldActor b = make_npc(1100, 0, 0);
    b.npc.interact_component = 1016;
    world::WorldActor c = make_npc(1200, 0, 0);
    c.npc.interact_component = 1216;
    CHECK(reg.add(std::move(a)) && !reg.add(std::move(b)) && reg.add(std::move(c)), "two actors cannot claim one component");
    CHECK(reg.find_by_component(1016) && reg.find_by_component(1016)->id == 1000 && reg.find_by_component(1216)->id == 1200 && !reg.find_by_component(1017), "a component finds its actor");
    reg.remove(1000);
    CHECK(!reg.find_by_component(1016) && reg.find_by_component(1216), "a removed actor's component is gone");
    world::WorldActor d = make_npc(1300, 0, 0);
    d.npc.interact_component = 1016;
    CHECK(reg.add(std::move(d)) && reg.find_by_component(1016)->id == 1300, "and can be claimed again");
}

static void check_state_actor_reentry_and_removal(world::ActorRegistry& reg, ActorView& v, Pair& p, size_t first_bits, size_t& first_forms) {
    run(v, p, 60000, 0, 2);
    take(p);
    CHECK(v.state_of(5000) == world::ClientState::None, "out of the snapshot ring the view forgets the actor");
    reg.modify(5000, world::Change::State, [](world::WorldActor& a) {
        repl::FastArrayState* fa = a.actor.subobjects[0].obj.arrays.get(*a.actor.subobjects[0].obj.schema, "IndexedStates");
        repl::ElementState es; es.values[1] = repl::Value::u64(1); es.values[2] = repl::Value::u64(2);
        fa->change(fa->items()[2].id, std::move(es));
    });
    run(v, p, 0, 0, 1);
    auto s = take(p);
    const size_t back_bits = open_bits(s);
    CHECK(back_bits > 0 && back_bits < first_bits / 2 && first_forms == 1, "walking back, the open carries only what changed since the view last sent it (the shadow was kept), and no first-open form");
    Pair p2;
    ActorView v2(reg, view_config(), reserved_3_to_6());
    run(v2, p2, 0, 0, 1);
    s = take(p2);
    CHECK(open_bits(s) > first_bits - 200 && first_forms == 2, "a connection that never had the cell is sent the whole list, in the first-open form");
    run(v, p, 0, 0, 3);
    take(p);
    reg.remove(5000);
    run(v, p, 0, 0, 2);
    CHECK(v.state_of(5000) == world::ClientState::None || v.tracked() == 0, "a removed actor is let go");
}

static void test_state_actor() {
    std::printf("the view: a foliage cell's state actor (a snapshot-only actor with kept fast arrays)\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView v(reg, view_config(), reserved_3_to_6());
    size_t first_forms = 0;
    world::WorldActor w = make_state_actor(5000, 3000, 0, 3, 40);
    w.first_open_form = [&](repl::Actor&) { ++first_forms; };
    reg.add(std::move(w));
    run(v, p, 0, 0, 1);
    auto s = take(p);
    CHECK(v.state_of(5000) == world::ClientState::Snapshot && v.stats().snapshots == 1 && v.stats().opens == 0, "30 m away it is opened as a snapshot, not awake: it never is");
    const size_t first_bits = open_bits(s);
    CHECK(first_bits > 3000 && first_forms == 1, "the first open carries the whole list (3 components of 40 instances) and was shown the first-open form once");
    p.t += milliseconds(10);
    run(v, p, 0, 0, 1);
    s = take(p);
    bool closed = false;
    for (const Seen& x : s) closed = closed || x.close;
    CHECK(!closed && v.state_of(5000) == world::ClientState::Snapshot, "140 ms after the open it is still open: its Dormancy close is 265 ms after (the relevancy config's 134 ms is not its)");
    run(v, p, 0, 0, 1);
    s = take(p);
    uint32_t reason = 99;
    for (const Seen& x : s) if (x.close) reason = x.reason;
    CHECK(reason == kCloseDormancy && v.state_of(5000) == world::ClientState::Dormant, "280 ms after the open it is closed with Dormancy");
    reg.modify(5000, world::Change::State, [](world::WorldActor& a) {
        repl::FastArrayState* fa = a.actor.subobjects[1].obj.arrays.get(*a.actor.subobjects[1].obj.schema, "IndexedStates");
        repl::ElementState es; es.values[1] = repl::Value::u64(0); es.values[2] = repl::Value::u64(7);
        fa->change(fa->items()[7].id, std::move(es));
    });
    run(v, p, 0, 0, 1);
    s = take(p);
    const size_t again_bits = open_bits(s);
    CHECK(again_bits > 0 && again_bits < first_bits / 2 && v.stats().snapshots == 2 && first_forms == 1,
          "a change makes a new snapshot that carries the changed item only: far fewer bits than the first open, and no first-open form");
    run(v, p, 0, 0, 3);
    take(p);
    reg.modify(5000, world::Change::State, [](world::WorldActor& a) {
        repl::FastArrayState* fa = a.actor.subobjects[2].obj.arrays.get(*a.actor.subobjects[2].obj.schema, "IndexedStates");
        fa->remove(fa->items()[3].id);
    });
    run(v, p, 0, 0, 1);
    s = take(p);
    const size_t delete_bits = open_bits(s);
    CHECK(delete_bits > 0 && delete_bits < first_bits / 2, "a deleted item is a delete in the open, not a rebuilt list");
    run(v, p, 0, 0, 3);
    take(p);
    check_state_actor_reentry_and_removal(reg, v, p, first_bits, first_forms);
}

static void test_tick_loop_and_cadence() {
    std::printf("tick loop budget and relevancy tier cadence\n");
    TickLoopBudget budget;
    CHECK(TickLoopBudget::kTickRateHz == 30 && TickLoopBudget::kTickIntervalUs == 33333 && TickLoopBudget::kTickSloP99Us == 15000, "30 Hz tick loop constants");
    budget.record_tick_phases(1000, 4000, 3000, 1000);
    CHECK(budget.tick_index() == 1 && budget.last_duration_us() == 9000 && budget.slo_violation_count() == 0 && budget.overrun_count() == 0, "tick under 15 ms SLO");
    budget.record_tick_duration_us(20000);
    CHECK(budget.tick_index() == 2 && budget.slo_violation_count() == 1 && budget.overrun_count() == 0 && budget.max_duration_us() == 20000, "tick over 15 ms SLO but under 33.33 ms");
    budget.record_tick_duration_us(40000);
    CHECK(budget.tick_index() == 3 && budget.slo_violation_count() == 2 && budget.overrun_count() == 1 && budget.max_duration_us() == 40000, "tick over 33.33 ms overrun");
    using world::RelevancyTier;
    const world::RelevancyConfig cfg;
    CHECK(world::classify_relevancy_tier(5000.0f * 5000.0f, cfg) == RelevancyTier::Tier0_Close &&
              world::classify_relevancy_tier(10000.0f * 10000.0f, cfg) == RelevancyTier::Tier1_Mid &&
              world::classify_relevancy_tier(20000.0f * 20000.0f, cfg) == RelevancyTier::Tier2_Far &&
              world::classify_relevancy_tier(30000.0f * 30000.0f, cfg) == RelevancyTier::Tier3_Horizon &&
              world::classify_relevancy_tier(40000.0f * 40000.0f, cfg) == RelevancyTier::Culled,
          "classify_relevancy_tier across distance bands");
    CHECK(world::should_replicate_on_tick(RelevancyTier::Tier0_Close, 1) && !world::should_replicate_on_tick(RelevancyTier::Tier1_Mid, 1) &&
              world::should_replicate_on_tick(RelevancyTier::Tier1_Mid, 2) && world::should_replicate_on_tick(RelevancyTier::Tier2_Far, 6) &&
              !world::should_replicate_on_tick(RelevancyTier::Tier2_Far, 5) && world::should_replicate_on_tick(RelevancyTier::Tier3_Horizon, 30) &&
              !world::should_replicate_on_tick(RelevancyTier::Tier3_Horizon, 29) && !world::should_replicate_on_tick(RelevancyTier::Culled, 0),
          "should_replicate_on_tick cadence 1/1, 1/2, 1/6, 1/30, 0");
    world::ActorRegistry reg;
    ActorView v(reg, view_config(), reserved_3_to_6());
    CHECK(v.classify_tier(5000.0) == RelevancyTier::Tier0_Close && v.should_update_for_tick(10000.0, 2) && !v.should_update_for_tick(10000.0, 3) &&
              !v.should_update_for_tick(40000.0, 0),
          "ActorView tier and tick-cadence helpers");
    CHECK((world::compute_spatial_cell(0.0f, 9999.0f) == world::SpatialCellCoord{0, 0}) &&
              (world::compute_spatial_cell(10000.0f, -1.0f) == world::SpatialCellCoord{1, -1}) &&
              (world::compute_spatial_cell(-10001.0f, -10000.0f) == world::SpatialCellCoord{-2, -1}) &&
              (world::compute_spatial_cell(std::nanf(""), 5000.0f) == world::SpatialCellCoord{0, 0}) &&
              world::spatial_cell_chebyshev_dist({1, -1}, {-2, 3}) == 4 &&
              world::is_cell_in_aoi_ring({0, 0}, {3, -3}, 3) && !world::is_cell_in_aoi_ring({0, 0}, {4, 0}, 3),
          "100 m spatial cell bucketing, negative floor, NaN fallback and AoI ring bounds");
    CHECK(world::should_replicate_subring(RelevancyTier::Tier0_Close, 1, 7) &&
              world::should_replicate_subring(RelevancyTier::Tier1_Mid, 1, 1) && !world::should_replicate_subring(RelevancyTier::Tier1_Mid, 1, 2) &&
              world::should_replicate_subring(RelevancyTier::Tier2_Far, 4, 2) && !world::should_replicate_subring(RelevancyTier::Tier2_Far, 4, 1) &&
              world::should_replicate_subring(RelevancyTier::Tier3_Horizon, 25, 5) && !world::should_replicate_subring(RelevancyTier::Tier3_Horizon, 25, 4) &&
              !world::should_replicate_subring(RelevancyTier::Culled, 0, 0),
          "should_replicate_subring staggered tick distribution across actor buckets");
    reg.add(make_npc(1000, 0, 0)); reg.add(make_npc(1002, 9999, 0)); reg.add(make_npc(1004, 25000, -15000)); reg.add(make_npc(1006, 45000, 0));
    uint64_t aoi_ids[4] = {};
    CHECK(reg.occupied_cell_count() == 3 && reg.count_actors_in_cell_ring(0.0f, 0.0f, 0) == 2 && reg.count_actors_in_cell_ring(0.0f, 0.0f, 2) == 3 &&
              reg.count_actors_in_cell_ring(0.0f, 0.0f, 4) == 4 && reg.count_actors_in_cell_ring(std::nanf(""), 0.0f, 3) == 0 &&
              reg.gather_aoi_candidates(0.0f, 0.0f, 2, aoi_ids, 4) == 3 && reg.gather_aoi_candidates(0.0f, 0.0f, 4, aoi_ids, 2) == 2,
          "ActorRegistry zero-allocation spatial AoI cell ring counting and candidate gathering");
}

int main() {
    test_view_owner_and_component_index();
    test_registry();
    test_classify();
    test_view_rings();
    test_wide_actor_leaves_the_query_alone();
    test_view_updates();
    test_view_never_dormant_and_removal();
    test_view_cap_and_exports();
    test_state_actor();
    test_tick_loop_and_cadence();
    run_actor_view_players_tests();
    run_actor_view_shared_tests();
    run_actor_view_threads_tests();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
