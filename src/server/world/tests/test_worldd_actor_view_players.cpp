// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "net/tick_profile.h"
#include "session/scan_order.h"
#include "test_worldd_actor_view_common.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace update_stream_checks {

struct Fnv {
    uint64_t h = 14695981039346656037ull;
    void add(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    void add(uint64_t v) {
        for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xff)) * 1099511628211ull;
    }
};

struct Result {
    uint64_t digest = 0;
    size_t   packets = 0;
    size_t   updates = 0;
    size_t   refused = 0;
    size_t   naks = 0;
};

inline void stream_step(Pair& p, ActorView& v, Fnv& fnv, bool deliver, milliseconds dt) {
    p.t += dt;
    const auto pkts = p.s.flush(p.t);
    p.packets += pkts.size();
    fnv.add(static_cast<uint64_t>(pkts.size()));
    for (const auto& pk : pkts) {
        fnv.add(pk.data(), pk.size());
        if (deliver) p.cl.receive(pk, p.t);
    }
    p.t += dt;
    if (deliver) {
        for (const auto& pk : p.cl.flush(p.t)) p.s.receive(pk, p.t);
    }
    for (const AckEvent& e : p.s.take_ack_events()) v.on_ack(e);
}

constexpr int kStreamActors = 16;

inline void step_stream_actors(world::ActorRegistry& reg, std::vector<bool>& dead, int step, bool flips) {
    for (int k = 0; k < 3; ++k) {
        const int i = (step * 3 + k * 5) % kStreamActors;
        const uint64_t id = 1000 + 10 * static_cast<uint64_t>(i);
        const world::WorldActor* a = reg.find(id);
        if (!a) continue;
        const double x = a->x + ((step + k) % 2 ? 12.0 : -12.0), y = a->y + 7.0;
        reg.move(id, x, y, a->z, static_cast<float>((step * 13) % 360));
        reg.modify(id, world::Change::Movement, [x, y](world::WorldActor& w) {
            repl::RepMovement m;
            m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), 1200000};
            m.rotation = repl::Rot{0, 64, 0};
            m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 0, 0};
            w.actor.obj.state.set(*w.actor.obj.schema, "ReplicatedMovement", repl::Value(m));
        });
    }
    if (!flips) return;
    for (int k = 0; k < 8; ++k) {
        const int i = k == 0 ? 0 : 1 + (step * 5 + k * 2) % (kStreamActors - 1);
        const uint64_t id = 1000 + 10 * static_cast<uint64_t>(i);
        if (!reg.find(id)) continue;
        dead[static_cast<size_t>(i)] = !dead[static_cast<size_t>(i)];
        const bool now_dead = dead[static_cast<size_t>(i)];
        reg.modify(id, world::Change::State, [now_dead](world::WorldActor& w) {
            for (repl::Subobject& s : w.actor.subobjects) {
                if (std::string(s.name) == "CombatInfo") s.obj.state.set(*s.obj.schema, "bIsDead", repl::Value::boolean(now_dead));
            }
        });
    }
}

inline Result run_update_stream() {
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    cfg.max_packets_in_flight = 1000000;
    cfg.max_queued_bunches = 1000000;
    ActorView v(reg, cfg, reserved_3_to_6());
    for (int i = 0; i < kStreamActors; ++i) {
        const double angle = 0.4 * i;
        reg.add(make_npc(1000 + 10 * static_cast<uint64_t>(i), 5000.0 + 400.0 * i * std::cos(angle), 5000.0 + 400.0 * i * std::sin(angle), i % 3 == 0 ? "Wolf" : "Boar"));
    }
    Fnv fnv;
    std::vector<bool> dead(kStreamActors, false);
    constexpr int kSteps = 460;
    for (int step = 0; step < kSteps; ++step) {
        const bool silent_moves = step >= 20 && step < 40;
        const bool blackout = silent_moves || (step >= 60 && step < 360);
        if (step < 400) step_stream_actors(reg, dead, step, !silent_moves);
        if (step == 100 || step == 380) reg.remove(1000 + 10 * (step == 100 ? 7 : 9));
        if (step == 130) reg.add(make_npc(1300, 6000, -3000, "Wolf"));
        v.tick(p.s, 0, 0, p.t);
        stream_step(p, v, fnv, !blackout, milliseconds(65));
    }
    Result r;
    r.digest = fnv.h;
    r.packets = p.packets;
    r.updates = v.stats().updates + v.stats().updates_unreliable;
    r.refused = v.stats().refused_updates;
    r.naks = v.stats().naks;
    return r;
}

}  // namespace update_stream_checks

namespace scan_checks {

struct Lcg {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    int64_t below(int64_t n) { return static_cast<int64_t>(next()) % n; }
};

using update_stream_checks::Fnv;
using update_stream_checks::stream_step;

struct Pt {
    int64_t x = 0;
    int64_t y = 0;
};

inline void add_creatures(world::ActorRegistry& reg, int count, Lcg& rng, uint64_t first_id, Pt center = {}) {
    for (int i = 0; i < count; ++i) {
        const int64_t r_min = (i % 5 < 2) ? 0 : 14000, r_max = (i % 5 < 2) ? 14000 : 35000;
        int64_t x = 0, y = 0;
        for (;;) {
            x = rng.below(2 * r_max + 1) - r_max;
            y = rng.below(2 * r_max + 1) - r_max;
            const int64_t d2 = x * x + y * y;
            if (d2 >= r_min * r_min && d2 <= r_max * r_max) break;
        }
        reg.add(make_npc(first_id + 10 * static_cast<uint64_t>(i), static_cast<double>(center.x + x), static_cast<double>(center.y + y), i % 3 == 0 ? "Wolf" : "Boar"));
    }
}

inline void step_creature(world::ActorRegistry& reg, uint64_t id, int step, int k) {
    const world::WorldActor* a = reg.find(id);
    if (!a) return;
    const double x = a->x + ((step + k) % 2 ? 12.0 : -12.0), y = a->y + 7.0;
    reg.move(id, x, y, a->z, static_cast<float>((step * 13) % 360));
    reg.modify(id, world::Change::Movement, [x, y](world::WorldActor& w) {
        repl::RepMovement m;
        m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), 1200000};
        m.rotation = repl::Rot{0, 64, 0};
        m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 0, 0};
        w.actor.obj.state.set(*w.actor.obj.schema, "ReplicatedMovement", repl::Value(m));
    });
}

inline void walk_one(int64_t& x, int64_t& y, int step, int leg) {
    static const int dx[8] = {42, 30, 0, -30, -42, -30, 0, 30};
    static const int dy[8] = {0, 30, 42, 30, 0, -30, -42, -30};
    const int d = (step / leg) % 8;
    x += dx[d];
    y += dy[d];
}

struct ScanResult {
    uint64_t digest = 0;
    size_t   packets = 0;
    size_t   passes = 0;
    size_t   opens = 0, snapshots = 0, wakes = 0, forgotten = 0, closes = 0;
    size_t   updates = 0;
    size_t   throttled = 0;
};

constexpr int kScanCreatures = 600;

inline void seed_scan_scene(world::ActorRegistry& reg, Lcg& rng) {
    constexpr int kMirrored = 60;
    add_creatures(reg, kScanCreatures, rng, 1000);
    for (int i = 0; i < kMirrored; ++i) {
        const world::WorldActor* a = reg.find(1000 + 10 * static_cast<uint64_t>(i));
        reg.add(make_npc(20000 + 10 * static_cast<uint64_t>(i), -a->x, -a->y, "Wolf"));
    }
    world::WorldActor town = make_npc(9000, 8000, 3000);
    town.awake_radius_cm = 145000;
    reg.add(std::move(town));
    world::WorldActor big = make_npc(9010, -12000, 9000);
    big.awake_radius_cm = 38000;
    reg.add(std::move(big));
    for (int i = 0; i < 4; ++i) {
        world::WorldActor w = make_npc(9020 + 10 * static_cast<uint64_t>(i), 3000.0 + 1500.0 * i, -2000);
        w.snapshot_only = true;
        reg.add(std::move(w));
    }
    for (int i = 0; i < 5; ++i) {
        world::WorldActor w = make_npc(9100 + 10 * static_cast<uint64_t>(i), -1500.0 * i, 2500);
        w.owner = 77;
        reg.add(std::move(w));
    }
}

inline ScanResult run_scan_scene(bool paced) {
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    cfg.max_packets_in_flight = paced ? 20 : 1000000;
    cfg.max_queued_bunches = paced ? 24 : 1000000;
    ActorView v(reg, cfg, reserved_3_to_6());
    Lcg rng{0x5ca17e57ull};
    seed_scan_scene(reg, rng);
    Fnv fnv;
    int64_t px = 0, py = 0;
    constexpr int kSteps = 960, kLeg = 120, kStill = 40;
    for (int step = 0; step < kSteps; ++step) {
        if (step >= kStill) {
            walk_one(px, py, step - kStill, kLeg);
            for (int k = 0; k < 40; ++k) step_creature(reg, 1000 + 10 * static_cast<uint64_t>((step * 7 + k * 13) % kScanCreatures), step, k);
        }
        if (step == 700) {
            px = -18000;
            py = 12000;
        }
        if (step == 300) add_creatures(reg, 20, rng, 30000, {px, py});
        if (step == 500) {
            for (int i = 100; i < 115; ++i) reg.remove(1000 + 10 * static_cast<uint64_t>(i));
        }
        v.tick(p.s, static_cast<double>(px), static_cast<double>(py), p.t);
        const bool silent = paced && ((step >= 100 && step < 140) || (step >= 520 && step < 545));
        stream_step(p, v, fnv, !silent, milliseconds(35));
    }
    ScanResult r;
    r.digest = fnv.h;
    r.packets = p.packets;
    r.passes = v.stats().passes;
    r.opens = v.stats().opens;
    r.snapshots = v.stats().snapshots;
    r.wakes = v.stats().wakes;
    r.forgotten = v.stats().forgotten;
    r.closes = v.stats().closes_dormancy + v.stats().closes_relevancy + v.stats().closes_destroyed;
    r.updates = v.stats().updates + v.stats().updates_unreliable;
    r.throttled = v.stats().throttled;
    return r;
}

struct Item {
    double   d;
    uint64_t id;
};

inline bool item_nearer(const Item& l, const Item& r) { return l.d != r.d ? l.d < r.d : l.id < r.id; }

inline bool check_scan_order_case(Lcg& rng, size_t n, int64_t moved_cm, size_t fresh, std::vector<Item>& spare) {
    const size_t held = n - fresh;
    std::vector<Item> list;
    for (size_t i = 0; i < held; ++i) list.push_back({static_cast<double>(rng.below(2000) * 100), 1000 + 10 * static_cast<uint64_t>(i)});
    std::sort(list.begin(), list.end(), item_nearer);
    for (Item& it : list) {
        const int64_t d = static_cast<int64_t>(it.d) + rng.below(2 * moved_cm + 1) - moved_cm;
        it.d = static_cast<double>((d < 0 ? 0 : d) / 100 * 100);
    }
    for (size_t i = 0; i < fresh; ++i) list.push_back({static_cast<double>(rng.below(2000) * 100), 50000 + 10 * static_cast<uint64_t>(i)});
    std::vector<Item> expect = list;
    std::sort(expect.begin(), expect.end(), item_nearer);
    sort_after_small_changes(list, held, item_nearer, spare);
    bool same = list.size() == expect.size();
    for (size_t i = 0; same && i < list.size(); ++i) same = list[i].id == expect[i].id && list[i].d == expect[i].d;
    return same;
}

inline size_t check_scan_order(size_t& wrong) {
    Lcg rng{0x0dd5ull};
    std::vector<Item> spare;
    size_t cases = 0;
    const size_t sizes[] = {0, 1, 2, 3, 7, 8, 9, 10, 25, 100, 650};
    for (int round = 0; round < 12; ++round) {
        for (const size_t n : sizes) {
            for (const int64_t moved_cm : {0, 40, 400, 100000}) {
                for (const size_t fresh : {size_t{0}, size_t{1}, size_t{5}, size_t{9}, n / 3}) {
                    if (fresh > n) continue;
                    ++cases;
                    if (!check_scan_order_case(rng, n, moved_cm, fresh, spare)) ++wrong;
                }
            }
        }
    }
    return cases;
}

struct Cost {
    double scan_us = 0;
    double tick_us = 0;
    size_t tracked = 0;
};

struct Viewer {
    Pair      p;
    Fnv       fnv;
    ActorView v;
    Viewer(world::ActorRegistry& reg, const ActorView::Config& cfg) : v(reg, cfg, reserved_3_to_6()) {}
};

inline Cost run_scan_cost(bool walking, int views) {
    world::ActorRegistry reg;
    ActorView::Config cfg = view_config();
    cfg.max_packets_in_flight = 1000000;
    cfg.max_queued_bunches = 1000000;
    std::vector<std::unique_ptr<Viewer>> viewers;
    for (int i = 0; i < views; ++i) viewers.push_back(std::make_unique<Viewer>(reg, cfg));
    Lcg rng{0xbe5c4ull};
    constexpr int kCreatures = 650;
    add_creatures(reg, kCreatures, rng, 1000);
    int64_t px = 0, py = 0;
    int step = 0;
    const auto tick_all = [&]() {
        for (size_t i = 0; i < viewers.size(); ++i) {
            Viewer& w = *viewers[i];
            w.v.tick(w.p.s, static_cast<double>(px + 300 * static_cast<int64_t>(i)), static_cast<double>(py), w.p.t);
            stream_step(w.p, w.v, w.fnv, true, milliseconds(35));
        }
    };
    for (; step < 120; ++step) tick_all();
    TickProfile& profile = tick_profile();
    profile.on = true;
    std::vector<double> scan, tick;
    for (int stretch = 0; stretch < 5; ++stretch) {
        profile.restart();
        for (int i = 0; i < 60; ++i, ++step) {
            if (walking) walk_one(px, py, step, 40);
            for (int k = 0; k < 40; ++k) step_creature(reg, 1000 + 10 * static_cast<uint64_t>((step * 7 + k * 13) % kCreatures), step, k);
            tick_all();
        }
        if (profile.view_passes > 0) {
            scan.push_back(profile.view_scan_us / static_cast<double>(profile.view_passes));
            tick.push_back(profile.view_us / static_cast<double>(profile.view_passes));
        }
    }
    profile.on = false;
    profile.restart();
    Cost c;
    if (!scan.empty()) {
        std::sort(scan.begin(), scan.end());
        std::sort(tick.begin(), tick.end());
        c.scan_us = scan[scan.size() / 2];
        c.tick_us = tick[tick.size() / 2];
    }
    c.tracked = viewers.empty() ? 0 : viewers.front()->v.tracked();
    return c;
}

}  // namespace scan_checks

static void test_view_update_stream_is_the_same_bytes() {
    using namespace update_stream_checks;
    std::printf("the view: the update chains it sends are the same bytes whether they are built on a copy of the shadow or on the shadow itself\n");
    const Result a = run_update_stream();
    const Result b = run_update_stream();
    char text[300];
    CHECK(a.digest == b.digest && a.packets == b.packets, "the scene is repeatable: two runs send the same datagrams");
    CHECK(a.updates > 400 && a.naks > 0, "...and it is a real one: hundreds of update chains, and lost ones put back for the next update");
    char refused[300];
    std::snprintf(refused, sizeof(refused), "the silent stretch fills the connection: it refused %zu update chains and the view built each of them again at a later pass", a.refused);
    CHECK(a.refused >= 20, refused);
    constexpr uint64_t kDigest = 0xa5cceeafbf606940ull;
    std::snprintf(text, sizeof(text), "the datagrams of the scene (%zu, %zu update chains, %zu put back after losses) fold to 0x%016llx; held: 0x%016llx", a.packets, a.updates, a.naks,
                  static_cast<unsigned long long>(a.digest), static_cast<unsigned long long>(kDigest));
    CHECK(a.digest == kDigest, text);
}

static void test_view_scan_sends_the_same_bytes() {
    using namespace scan_checks;
    std::printf("the view: the relevancy scan sends the same bytes whichever way it orders the actors\n");
    char text[400];
    for (const bool paced : {false, true}) {
        const ScanResult a = run_scan_scene(paced);
        const ScanResult b = run_scan_scene(paced);
        std::snprintf(text, sizeof(text), "%s: the scene is repeatable: two runs send the same datagrams", paced ? "paced" : "unpaced");
        CHECK(a.digest == b.digest && a.packets == b.packets, text);
        std::snprintf(text, sizeof(text), "...and it is a real one: %zu passes, %zu opens, %zu snapshots, %zu wakes, %zu closes, %zu forgotten, %zu update chains, %zu datagrams, %zu passes held back",
                      a.passes, a.opens, a.snapshots, a.wakes, a.closes, a.forgotten, a.updates, a.packets, a.throttled);
        CHECK(a.passes >= 900 && a.opens > 100 && a.snapshots > 300 && a.wakes > 20 && a.closes > 100 && a.forgotten > 20 && a.updates > 100 && (!paced || a.throttled > 100), text);
        constexpr uint64_t kDigest = 0x84fd4b8c384e6db3ull, kPacedDigest = 0xe6ba2049b1aaadd6ull;
        const uint64_t held = paced ? kPacedDigest : kDigest;
        std::snprintf(text, sizeof(text), "%s: the datagrams of the scene fold to 0x%016llx; held: 0x%016llx", paced ? "paced" : "unpaced", static_cast<unsigned long long>(a.digest),
                      static_cast<unsigned long long>(held));
        CHECK(a.digest == held, text);
    }
}

static void test_view_scan_order_is_the_order_of_a_full_sort() {
    using namespace scan_checks;
    std::printf("the view: the scan's ordering starts from the last pass's order and gives the order a full sort gives\n");
    size_t wrong = 0;
    const size_t cases = check_scan_order(wrong);
    char text[300];
    std::snprintf(text, sizeof(text), "%zu lists of 0 to 650 actors (ties on the distance, new actors, distances moved by 0 cm to 1 km): %zu came out in another order than std::sort's", cases, wrong);
    CHECK(cases > 2000 && wrong == 0, text);
}

static void test_view_scan_cost() {
    using namespace scan_checks;
    std::printf("the view: what one pass costs at 650 creatures (the work of the server for one view; not a count of players)\n");
    char text[400];
    for (const int views : {1, 16}) {
        const Cost standing = run_scan_cost(false, views);
        const Cost walking = run_scan_cost(true, views);
        std::snprintf(text, sizeof(text), "%d view(s), the players standing: a pass scans in %.1f us of a tick of %.1f us (a view holds %zu actors)", views, standing.scan_us, standing.tick_us, standing.tracked);
        CHECK(standing.tracked >= 600 && standing.scan_us > 0 && standing.scan_us < 5000, text);
        std::snprintf(text, sizeof(text), "%d view(s), the players walking 6 m a second: a pass scans in %.1f us of a tick of %.1f us (a view holds %zu actors)", views, walking.scan_us, walking.tick_us, walking.tracked);
        CHECK(walking.tracked >= 600 && walking.scan_us > 0 && walking.scan_us < 5000, text);
    }
}

static world::WorldActor make_player_actor(uint64_t id, double x, double y, uint64_t player, const char* name) {
    world::WorldActor w = make_npc(id, x, y, "Wolf");
    w.kind = world::ActorKind::Player;
    w.never_dormant = true;
    w.owner = player;
    w.player.name = name;
    w.label = name;
    return w;
}

static void test_view_another_players_pawn_is_sent_and_its_own_is_not() {
    std::printf("the view: another player's pawn is sent, this player's own is not\n");
    for (const uint64_t viewer : {7ull, 8ull}) {
        world::ActorRegistry reg;
        Pair p;
        ActorView::Config cfg = view_config();
        cfg.player_id = viewer;
        ActorView v(reg, cfg, reserved_3_to_6());
        reg.add(make_player_actor(5000, 3000, 0, 7, "cyb"));
        reg.add(make_player_actor(5100, 5000, 0, 8, "ayla"));
        run(v, p, 0, 0, 3);
        const uint64_t other = viewer == 7 ? 5100 : 5000, own = viewer == 7 ? 5000 : 5100;
        char text[200];
        std::snprintf(text, sizeof(text), "player %llu's view holds the other player's pawn awake and none of its own (one actor tracked)", static_cast<unsigned long long>(viewer));
        CHECK(v.state_of(other) == world::ClientState::Awake && v.state_of(own) == world::ClientState::None && v.tracked() == 1, text);
        const auto s = take(p);
        size_t opens = 0;
        for (const Seen& x : s) opens += x.open ? 1 : 0;
        CHECK(opens == 1, "...and exactly one open went out");
    }
}

static void test_view_player_actors_pets_zones_and_mounts() {
    std::printf("the view: a pet and a player's pawn are both sent; another player's zone and mount are not\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    cfg.player_id = 7;
    ActorView v(reg, cfg, reserved_3_to_6());
    world::WorldActor pet = make_npc(6000, 2000, 0, "Wolf");
    pet.never_dormant = true;
    reg.add(std::move(pet));
    reg.add(make_player_actor(5100, 4000, 0, 8, "ayla"));
    reg.add(make_player_actor(5000, 3000, 0, 7, "cyb"));
    const auto owned = [&](uint64_t id, world::ActorKind kind, uint64_t owner) {
        world::WorldActor w = make_npc(id, 2500, 0, "Effect");
        w.kind = kind;
        w.never_dormant = true;
        w.owner = owner;
        reg.add(std::move(w));
    };
    owned(7000, world::ActorKind::Effect, 8);
    owned(7100, world::ActorKind::Mount, 8);
    owned(7200, world::ActorKind::Effect, 7);
    owned(7300, world::ActorKind::Mount, 7);
    run(v, p, 0, 0, 3);
    CHECK(v.state_of(6000) == world::ClientState::Awake, "the pet (an NPC of nobody's) is sent");
    CHECK(v.state_of(5100) == world::ClientState::Awake, "another player's pawn is sent beside it");
    CHECK(v.state_of(5000) == world::ClientState::None, "this player's own Player actor is not");
    CHECK(v.state_of(7000) == world::ClientState::None && v.state_of(7100) == world::ClientState::None, "another player's zone and mount are not (they stay its owner's)");
    CHECK(v.state_of(7200) == world::ClientState::Awake && v.state_of(7300) == world::ClientState::Awake, "this player's own zone and mount are");
    CHECK(v.tracked() == 4, "four actors in all: the pet, the other pawn, the own zone and the own mount");
}

static void test_view_player_actor_rings() {
    std::printf("the view: a player's pawn opens within 150 m, closes with Relevancy beyond it and is never dormant\n");
    world::ActorRegistry reg;
    Pair p;
    ActorView::Config cfg = view_config();
    cfg.player_id = 7;
    ActorView v(reg, cfg, reserved_3_to_6());
    reg.add(make_player_actor(5100, 10000, 0, 8, "ayla"));
    run(v, p, 0, 0, 2);
    take(p);
    CHECK(v.state_of(5100) == world::ClientState::Awake, "100 m away: awake");
    run(v, p, -10000, 0, 2);
    auto s = take(p);
    uint32_t reason = 99;
    for (const Seen& x : s) {
        if (x.close) reason = x.reason;
    }
    CHECK(v.state_of(5100) == world::ClientState::None && reason == kCloseRelevancy, "200 m away: closed with Relevancy, not put to sleep (never dormant)");
    run(v, p, -10000, 0, 5);
    s = take(p);
    CHECK(v.state_of(5100) == world::ClientState::None && s.empty(), "and nothing more is sent while it is out of range (no snapshot of it)");
    run(v, p, 0, 0, 2);
    CHECK(v.state_of(5100) == world::ClientState::Awake, "back within 150 m: it is opened again");
    reg.remove(5100);
    run(v, p, 0, 0, 2);
    s = take(p);
    uint32_t last_reason = 99;
    for (const Seen& x : s) {
        if (x.close) last_reason = x.reason;
    }
    CHECK(v.state_of(5100) == world::ClientState::None && last_reason == kCloseDestroyed, "when its player leaves the world the registry removes it and the view closes it with Destroyed");
}

void run_actor_view_players_tests() {
    test_view_update_stream_is_the_same_bytes();
    test_view_scan_sends_the_same_bytes();
    test_view_scan_order_is_the_order_of_a_full_sort();
    test_view_scan_cost();
    test_view_another_players_pawn_is_sent_and_its_own_is_not();
    test_view_player_actors_pets_zones_and_mounts();
    test_view_player_actor_rings();
}
