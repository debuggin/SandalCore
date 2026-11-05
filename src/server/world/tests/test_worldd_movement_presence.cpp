// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "movement_presence_rig.h"

static BunchWriter hex_bits(const capture_blocks::Bits& b) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    BunchWriter w;
    w.write_bit_range(bytes.data(), 0, b.bits);
    return w;
}

static emit::FIntrepidNetworkGUID view_test_guid(uint64_t object, uint32_t server) {
    emit::FIntrepidNetworkGUID g;
    g.ObjectId = object;
    g.ServerId = server;
    g.Randomizer = server == 0 ? 0 : kRandomizer;
    return g;
}

static void view_test_set(repl::ObjectState& o, const char* path, repl::Value v) {
    if (!o.state.set(*o.schema, path, std::move(v))) {
        std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
        g_fail++;
    }
}

static repl::ExportLink view_test_link(uint64_t object, const char* name, uint32_t checksum) {
    repl::ExportLink l;
    l.guid = view_test_guid(object, 0);
    l.name = name;
    l.no_load = false;
    l.has_checksum = true;
    l.checksum = checksum;
    return l;
}

static world::WorldActor view_test_mob(uint64_t id, double x, double y, double z) {
    world::WorldActor w;
    w.id = id;
    w.kind = world::ActorKind::Npc;
    w.x = x;
    w.y = y;
    w.z = z;
    repl::Actor& a = w.actor;
    a.guid = view_test_guid(id, kServerId);
    a.obj.schema = &class_named("ANPCCharacter");
    view_test_set(a.obj, "AuthServerIDReplicated", repl::Value::u64(kServerId));
    repl::RepMovement m;
    m.location = repl::QuantVec{
        repl::QuantVec::Mode::Scaled,
        static_cast<int64_t>(x * 100),
        static_cast<int64_t>(y * 100),
        static_cast<int64_t>(z * 100)};
    m.rotation = repl::Rot{0, 64, 0};
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    view_test_set(a.obj, "ReplicatedMovement", repl::Value(m));
    view_test_set(a.obj, "RemoteRole", repl::Value::u64(1));
    view_test_set(a.obj, "Owner", repl::Value(view_test_guid(id + 2, kServerId)));
    view_test_set(a.obj, "Role", repl::Value::u64(4));
    view_test_set(a.obj, "Controller", repl::Value(view_test_guid(id + 2, kServerId)));
    view_test_set(a.obj, "ReplicatedMovementMode", repl::Value::u64(10));
    view_test_set(a.obj, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{0}));
    a.open.archetype = {
        view_test_link(0xA001, "Default__Wolf_C", 0x1234),
        view_test_link(0xB001, "/Game/DesignAssets/Population/NPC/Beast/Wolf/Wolf", 0)};
    a.open.level = bs::persistent_level_export();
    a.open.spawn.location = repl::QuantVec{
        repl::QuantVec::Mode::Scaled,
        static_cast<int64_t>(x * 10),
        static_cast<int64_t>(y * 10),
        static_cast<int64_t>(z * 10)};
    a.open.spawn.rotation = repl::Rot{0, 11000, 0};
    repl::Subobject info;
    info.guid = view_test_guid(id + 6, kServerId);
    info.name = "BaseCharacterInfo";
    info.checksum = 0xaff9746f;
    info.obj.schema = &class_named("UNPCInfo");
    view_test_set(info.obj, "CharacterGuid.A", repl::Value::i64(static_cast<int32_t>(id)));
    view_test_set(info.obj, "Level", repl::Value::i64(14));
    a.subobjects.push_back(std::move(info));
    w.npc.name = "A Wolf";
    w.npc.level = 14;
    return w;
}

using ThreadedEndpoints = PresenceEndpoints;

struct SentDigest {
    uint64_t h = 14695981039346656037ull;
    size_t datagrams = 0;
    void add(const std::vector<uint8_t>& p) {
        for (const uint8_t b : p) h = (h ^ b) * 1099511628211ull;
        h = (h ^ p.size()) * 1099511628211ull;
        ++datagrams;
    }
};

static void exchange_digest(
    Rig& rig,
    Joined& j,
    const Address& addr,
    std::vector<fake::Bunch> bunches,
    Clock::time_point now,
    SentDigest& digest) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    for (const auto& p : rig.take(addr)) {
        digest.add(p);
        const auto r = j.client->receive(p);
        for (const auto& d : r.delivered) j.got.push_back(d);
    }
}

static void test_endpoint_runs_the_view_passes_on_the_pool() {
    std::printf("through the endpoint with the view passes on the pool, a mob 20 m from the player\n");
    char text[360];
    uint64_t digests[3] = {};
    size_t datagrams[3] = {};
    size_t index = 0;
    for (const unsigned threads : {0u, 1u, 3u}) {
        g_counter = 1;
        ThreadedEndpoints e(threads);
        Rig& rig = *e.rig;
        CHECK(e.fx.world->registry().add(view_test_mob(77000, kX + 2000.0, kY, kZ)),
              "the mob joins the registry, 20 m from where the player spawns");
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        exchange(
            rig,
            j,
            kA,
            {j.client->actor(5, hex_bits(capture_blocks::kClientReadyToPhaseIn), true)},
            t0 + milliseconds(60));
        SentDigest digest;
        for (int i = 1; i <= 40; ++i) {
            exchange_digest(
                rig,
                j,
                kA,
                {j.client->actor(
                    5, client_move(move_at(1.0f + 0.001f * static_cast<float>(i), kX + 10.0 + i, kY, kZ, 8192)), false)},
                t0 + milliseconds(100 + 40 * i),
                digest);
        }
        const PlayerSession* player = rig.ep.player_at(kA);
        const ActorView* view = player ? player->actor_view() : nullptr;
        const size_t opened = view ? view->stats().opens + view->stats().snapshots : 0;
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): the player's session lives, %s, the mob was opened on its client (%zu opens), %zu datagrams sent in the walk, and the registry is thawed",
            threads,
            threads == 0 ? "no pool runs" : "its passes ran on the pool",
            opened,
            digest.datagrams);
        const bool pooled = threads == 0 ? rig.ep.view_jobs() == 0 : rig.ep.view_jobs() > 0;
        CHECK(rig.ep.view_threads() == threads && rig.ep.session_count() == 1 && pooled && opened > 0 &&
                  digest.datagrams > 0 && !e.fx.world->registry().frozen(),
              text);
        digests[index] = digest.h;
        datagrams[index] = digest.datagrams;
        ++index;
    }
    std::snprintf(
        text,
        sizeof(text),
        "the datagrams the player got are the same bytes with no pool, one thread and three (%zu, %zu and %zu datagrams; 0x%016llx, 0x%016llx and 0x%016llx)",
        datagrams[0],
        datagrams[1],
        datagrams[2],
        static_cast<unsigned long long>(digests[0]),
        static_cast<unsigned long long>(digests[1]),
        static_cast<unsigned long long>(digests[2]));
    CHECK(digests[0] == digests[1] && digests[0] == digests[2] && datagrams[0] == datagrams[1] &&
              datagrams[0] == datagrams[2],
          text);
}

static bool presence_at_join_spot(const world::WorldActor* a) {
    return a && near(a->x, kX) && near(a->y, kY);
}

static void test_presence_idle_join_is_in_the_world_within_two_ticks() {
    std::printf(
        "players seeing each other: a player who joins and stands still is in the world for the other players within two ticks, with 0, 1 and 3 view threads\n");
    char text[420];
    for (const unsigned threads : {0u, 1u, 3u}) {
        g_counter = 1;
        PresenceEndpoints e(threads);
        Rig& rig = *e.rig;
        const world::ActorRegistry& reg = e.fx.world->registry();
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient a(50510, "cyb"), b(50511, "ayla");
        presence_join(rig, all, a, cyb_record(), now);
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        const int took_a = presence_phase_in(rig, all, a, now);
        const int took_b = presence_phase_in(rig, all, b, now);
        presence_idle(rig, all, now, 2);
        const uint64_t ida = presence_player_id(rig, a.addr), idb = presence_player_id(rig, b.addr);
        const world::WorldActor* pa = presence_pawn(reg, ida);
        const world::WorldActor* pb = presence_pawn(reg, idb);
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): two ticks after the phase-ins (%d and %d steps) both players are Player actors of the registry, at the join spot, with no move sent",
            threads,
            took_a,
            took_b);
        CHECK(took_a >= 0 && took_b >= 0 && ida != 0 && idb != 0 && ida != idb && pa && pb &&
                  presence_count(reg) == 2 && presence_at_join_spot(pa) && presence_at_join_spot(pb),
              text);
        presence_idle(rig, all, now, 12);
        const PlayerSession* sa = rig.ep.player_at(a.addr);
        const PlayerSession* sb = rig.ep.player_at(b.addr);
        const ActorView* va = sa ? sa->actor_view() : nullptr;
        const ActorView* vb = sb ? sb->actor_view() : nullptr;
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): each player's view opens the other's pawn and holds nothing else (one Player actor for each player)",
            threads);
        CHECK(va && vb && pa && pb && va->state_of(pb->id) == world::ClientState::Awake &&
                  vb->state_of(pa->id) == world::ClientState::Awake && va->tracked() == 1 && vb->tracked() == 1,
              text);
        std::snprintf(
            text, sizeof(text), "%u view thread(s): a player's own pawn is not one of its view's actors", threads);
        CHECK(va && vb && pa && pb && va->state_of(pa->id) == world::ClientState::None &&
                  vb->state_of(pb->id) == world::ClientState::None,
              text);
        CHECK(!reg.frozen(), "(the registry is thawed after the ticks)");
    }
}

static void test_presence_leaves_with_the_player() {
    std::printf(
        "players seeing each other: a player who quits, is kicked for a login elsewhere or times out leaves the registry, and its view's partner closes it\n");
    for (const unsigned threads : {0u, 3u}) {
        char text[300];
        g_counter = 1;
        PresenceEndpoints e(threads);
        Rig& rig = *e.rig;
        const world::ActorRegistry& reg = e.fx.world->registry();
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient a(50510, "cyb"), b(50511, "ayla"), c(50512, "mira");
        presence_join(rig, all, a, cyb_record(), now);
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        presence_join(rig, all, c, presence_record(presence_id(3), "mira", 5), now);
        for (PresenceClient* p : all) presence_phase_in(rig, all, *p, now);
        presence_idle(rig, all, now, 12);
        std::snprintf(text, sizeof(text), "%u view thread(s): three players, three Player actors", threads);
        CHECK(rig.ep.session_count() == 3 && presence_count(reg) == 3, text);
        const uint64_t id_c = presence_player_id(rig, c.addr);
        c.out.push_back(c.j.client->close_channel());
        presence_step(rig, all, now += milliseconds(40));
        all.erase(std::remove(all.begin(), all.end(), &c), all.end());
        presence_idle(rig, all, now, 12);
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): a player who quits is out of the registry, the other two stay, and the others' views have closed it",
            threads);
        const PlayerSession* sa = rig.ep.player_at(a.addr);
        CHECK(rig.ep.session_count() == 2 && presence_count(reg) == 2 && !presence_pawn(reg, id_c) && sa &&
                  sa->actor_view() && sa->actor_view()->tracked() == 1,
              text);
        const uint64_t id_b = presence_player_id(rig, b.addr);
        PresenceClient b2(50513, "ayla");
        presence_join(rig, all, b2, presence_record(presence_id(2), "ayla", 4), now);
        presence_phase_in(rig, all, b2, now);
        presence_idle(rig, all, now, 10);
        const uint64_t id_b2 = presence_player_id(rig, b2.addr);
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): kicked for a login elsewhere, the first session's pawn is gone and the new session has its own (%zu Player actors)",
            threads,
            presence_count(reg));
        CHECK(id_b2 != 0 && id_b2 != id_b && !presence_pawn(reg, id_b) && presence_pawn(reg, id_b2) &&
                  presence_count(reg) == 2,
              text);
        all.erase(std::remove(all.begin(), all.end(), &b), all.end());
        presence_idle(rig, all, now, 80);
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): after the linger the kicked session is gone and the registry still holds the two players",
            threads);
        CHECK(rig.ep.session_count() == 2 && presence_count(reg) == 2, text);
        const uint64_t id_a = presence_player_id(rig, a.addr);
        all.erase(std::remove(all.begin(), all.end(), &a), all.end());
        now += seconds(70);
        presence_step(rig, all, now);
        presence_idle(rig, all, now, 4);
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): a player whose connection times out is out of the registry, the last player's pawn stays",
            threads);
        CHECK(rig.ep.session_count() == 1 && presence_count(reg) == 1 && !presence_pawn(reg, id_a) &&
                  presence_pawn(reg, id_b2),
              text);
        CHECK(!reg.frozen(), "(the registry is thawed after the ticks)");
    }
}

struct PresencePair {
    PresenceEndpoints e;
    std::vector<PresenceClient*> all;
    PresenceClient a, b;
    Clock::time_point now = t0;
    explicit PresencePair(unsigned threads) : e(threads), a(50510, "cyb"), b(50511, "ayla") {
        g_counter = 1;
        Rig& rig = *e.rig;
        presence_join(rig, all, a, cyb_record(), now);
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        presence_phase_in(rig, all, a, now);
        presence_phase_in(rig, all, b, now);
        presence_idle(rig, all, now, 12);
    }
    Rig& rig() { return *e.rig; }
    const world::ActorRegistry& reg() const { return e.fx.world->registry(); }
};

static void test_presence_walk_then_stop_with_no_more_moves() {
    std::printf(
        "players seeing each other: a walk shows a velocity, and a stop of a client that sends nothing more shows none within 300 ms, with 0, 1 and 3 view threads\n");
    char text[420];
    for (const unsigned threads : {0u, 1u, 3u}) {
        PresencePair p(threads);
        const uint64_t ida = presence_player_id(p.rig(), p.a.addr);
        p.a.ts = 1.0f;
        presence_move(p.a, 0.0f, 40.0);
        p.now += milliseconds(100);
        presence_step(p.rig(), p.all, p.now);
        double fastest = 0;
        for (int i = 0; i < 10; ++i) {
            presence_move(p.a, 0.1f, 40.0);
            p.now += milliseconds(100);
            presence_step(p.rig(), p.all, p.now);
            if (const world::WorldActor* pawn = presence_pawn(p.reg(), ida)) {
                const PresenceMotion m = presence_motion(*pawn);
                fastest = std::max(fastest, std::hypot(m.vx, m.vy));
            }
        }
        const world::WorldActor* pawn = presence_pawn(p.reg(), ida);
        PresenceMotion walking = pawn ? presence_motion(*pawn) : PresenceMotion{};
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): while it walks the pawn shows 400 cm/s and the walking input, and stands where the last move put it (x %.0f, %.0f from the first; the fastest was %.0f)",
            threads,
            walking.x,
            walking.x - kX,
            fastest);
        CHECK(pawn && walking.ok && walking.moving && std::fabs(std::hypot(walking.vx, walking.vy) - 400.0) < 25.0 &&
                  near(pawn->x, p.a.x) && near(walking.x, p.a.x),
              text);
        const Clock::time_point last_move = p.now;
        double velocity_at_300 = -1;
        for (int ms = 50; ms <= 300; ms += 50) {
            presence_step(p.rig(), p.all, last_move + milliseconds(ms));
            p.now = last_move + milliseconds(ms);
        }
        if (const world::WorldActor* stopped = presence_pawn(p.reg(), ida)) {
            const PresenceMotion m = presence_motion(*stopped);
            velocity_at_300 = std::hypot(m.vx, m.vy);
            std::snprintf(
                text,
                sizeof(text),
                "%u view thread(s): 300 ms after the last move the pawn shows no velocity and no walking input (%.0f cm/s), still at x %.0f",
                threads,
                velocity_at_300,
                m.x);
            CHECK(m.ok && velocity_at_300 == 0.0 && !m.moving && near(m.x, p.a.x), text);
        } else {
            CHECK(false, "the pawn is in the registry");
        }
        const PlayerSession* sb = p.rig().ep.player_at(p.b.addr);
        const uint32_t channel = (sb && sb->actor_view() && pawn) ? sb->actor_view()->channel_of(pawn->id) : 0;
        size_t updates = 0;
        for (const fake::Delivered& d : p.b.j.got) {
            if (d.channel == channel && !d.open && !d.close) ++updates;
        }
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): the other player's client got the pawn's updates on its channel (%zu so far)",
            threads,
            updates);
        CHECK(channel != 0 && updates >= 3, text);
    }
}

static void test_presence_a_server_move_is_followed_with_no_client_move() {
    std::printf(
        "players seeing each other: a teleport is in the registry on the next tick with no move from the client, with 0, 1 and 3 view threads\n");
    char text[420];
    for (const unsigned threads : {0u, 1u, 3u}) {
        PresencePair p(threads);
        const uint64_t ida = presence_player_id(p.rig(), p.a.addr);
        const world::WorldActor* pawn = presence_pawn(p.reg(), ida);
        const uint32_t channel = [&] {
            const PlayerSession* sb = p.rig().ep.player_at(p.b.addr);
            return (sb && sb->actor_view() && pawn) ? sb->actor_view()->channel_of(pawn->id) : 0u;
        }();
        size_t updates_before = 0;
        for (const fake::Delivered& d : p.b.j.got) {
            if (d.channel == channel && !d.open && !d.close) ++updates_before;
        }
        const double to_x = kX + 3000.0, to_y = kY + 500.0;
        PlayerSession::GmMoveResult moved;
        for (PlayerHandle& h : p.rig().ep.players()) {
            if (h.player->name() == "cyb") moved = h.player->gm_teleport(*h.connection, to_x, to_y, kZ, p.now, "a test");
        }
        CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "(the game master's teleport is taken)");
        presence_idle(p.rig(), p.all, p.now, 8);
        pawn = presence_pawn(p.reg(), ida);
        const PresenceMotion m = pawn ? presence_motion(*pawn) : PresenceMotion{};
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): the pawn is where the teleport put the player (x %+.0f, y %+.0f from the join spot) and stands still",
            threads,
            pawn ? pawn->x - kX : 0.0,
            pawn ? pawn->y - kY : 0.0);
        CHECK(pawn && near(pawn->x, to_x) && near(pawn->y, to_y) && m.ok && near(m.x, to_x) && near(m.y, to_y) &&
                  !m.moving,
              text);
        size_t updates_after = 0;
        for (const fake::Delivered& d : p.b.j.got) {
            if (d.channel == channel && !d.open && !d.close) ++updates_after;
        }
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): the other player's client was sent the new place (%zu updates before, %zu after)",
            threads,
            updates_before,
            updates_after);
        CHECK(channel != 0 && updates_after > updates_before, text);
    }
}

static void test_presence_churn_never_writes_the_registry_in_a_view_pass() {
    std::printf(
        "players seeing each other: 200 ticks of players joining, walking and leaving next to one who stays, with 0, 1 and 3 view threads\n");
    char text[420];
    for (const unsigned threads : {0u, 1u, 3u}) {
        g_counter = 1;
        PresenceEndpoints e(threads);
        Rig& rig = *e.rig;
        const world::ActorRegistry& reg = e.fx.world->registry();
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient stays(50510, "cyb");
        presence_join(rig, all, stays, cyb_record(), now);
        presence_phase_in(rig, all, stays, now);
        stays.ts = 1.0f;
        const size_t log_from = g_log.str().size();
        int ticks = 0, joined = 0;
        for (int round = 0; round < 6; ++round) {
            PresenceClient visitor(static_cast<uint16_t>(50600 + round), "mx" + std::to_string(round));
            presence_join(
                rig, all, visitor, presence_record(presence_id(16 + round), "mx" + std::to_string(round), 10 + round), now);
            ++joined;
            presence_phase_in(rig, all, visitor, now);
            visitor.ts = 1.0f;
            for (int k = 0; k < 6; ++k) {
                presence_move(visitor, k == 0 ? 0.0f : 0.1f, 25.0);
                presence_move(stays, k == 0 ? 0.0f : 0.1f, -25.0);
                now += milliseconds(100);
                presence_step(rig, all, now);
                ++ticks;
            }
            visitor.out.push_back(visitor.j.client->close_channel());
            now += milliseconds(40);
            presence_step(rig, all, now);
            all.erase(std::remove(all.begin(), all.end(), &visitor), all.end());
            presence_idle(rig, all, now, 3);
            ticks += 4;
        }
        presence_idle(rig, all, now, 20);
        ticks += 20;
        const std::string log = g_log.str().substr(log_from);
        const bool threw = log.find("the tick threw") != std::string::npos ||
                           log.find("was changed (") != std::string::npos;
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): %d joins and leaves in %d+ ticks: the player who stays is still there, alone in the registry, the registry is thawed and no tick threw",
            threads,
            joined,
            ticks);
        CHECK(rig.ep.session_count() == 1 && presence_count(reg) == 1 && !reg.frozen() && !threw, text);
    }
}

static void test_presence_another_players_client_is_sent_one_pawn_and_no_login_text() {
    std::printf(
        "players seeing each other: another player's client is sent one actor for a player and none of the login's ?Name= text, with 0 and 3 view threads\n");
    char text[420];
    const std::string marker = "ZQXMARKERNAME1234";
    for (const unsigned threads : {0u, 3u}) {
        g_counter = 1;
        PresenceEndpoints e(threads);
        Rig& rig = *e.rig;
        const world::ActorRegistry& reg = e.fx.world->registry();
        Clock::time_point now = t0;
        std::vector<PresenceClient*> all;
        PresenceClient a(50510, "cyb"), b(50511, "ayla");
        a.needles = {marker};
        b.needles = {marker};
        presence_join(rig, all, a, cyb_record(), now, marker);
        presence_join(rig, all, b, presence_record(presence_id(2), "ayla", 4), now);
        presence_phase_in(rig, all, a, now);
        presence_phase_in(rig, all, b, now);
        presence_idle(rig, all, now, 25);
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): (the search works: the marker is in the packets of cyb's own client, %zu of %zu)",
            threads,
            a.hits,
            a.packets);
        CHECK(a.hits > 0, text);
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): the marker is in none of the %zu packets the other player's client is sent, as one byte or two per character",
            threads,
            b.packets);
        CHECK(b.hits == 0 && b.packets > 10, text);
        size_t opens = 0;
        for (const fake::Delivered& d : b.j.got) {
            if (d.open && d.channel > 6) ++opens;
        }
        const uint64_t ida = presence_player_id(rig, a.addr);
        std::snprintf(
            text,
            sizeof(text),
            "%u view thread(s): the registry holds one Player actor for each of the two players and ayla's client opened exactly one actor for cyb (%zu opens, %zu Player actors)",
            threads,
            opens,
            presence_count(reg));
        CHECK(presence_count(reg) == 2 && presence_pawn(reg, ida) && opens == 1, text);
    }
}

static void test_presence_the_pawn_other_players_are_shown_has_nothing_of_the_owners() {
    std::printf(
        "players seeing each other: the pawn other players are shown has no Owner, Controller, PlayerState or UniqueId, and the character's name\n");
    Harness h;
    {
        BunchWriter ready;
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        ready.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        h.client_sends({h.client.actor(h.pawn_ch, ready, true)});
    }
    h.settle();
    const auto pawn = h.s->bootstrap().observer_pawn();
    CHECK(pawn.has_value(), "(the bootstrap builds the pawn other players are shown once the join is built)");
    if (!pawn) return;
    const repl::ObjectState& o = pawn->obj;
    const auto unset = [&](const char* path) {
        return o.schema->command(path) != nullptr && o.state.get(*o.schema, path) == nullptr;
    };
    CHECK(unset("Owner") && unset("Controller") && unset("PlayerState"),
          "the pawn has no Owner, no Controller and no PlayerState (those are objects only its owner's connection has)");
    bool unique_id = false;
    std::string character_name;
    size_t leaked = 0;
    const std::string login_text = h.s->bootstrap().setup().login.name_option;
    const auto scan = [&](const repl::ObjectState& obj) {
        if (obj.schema && obj.schema->command("UniqueId") && obj.state.get(*obj.schema, "UniqueId")) unique_id = true;
        for (const auto& [handle, value] : obj.state.values()) {
            if (const auto* str = value.get_if<std::string>()) {
                if (!login_text.empty() && str->find(login_text.substr(0, 12)) != std::string::npos) ++leaked;
            }
        }
        if (obj.schema && obj.schema->command("CharacterName")) {
            if (const repl::Value* v = obj.state.get(*obj.schema, "CharacterName")) {
                if (const auto* str = v->get_if<std::string>()) character_name = *str;
            }
        }
    };
    scan(pawn->obj);
    for (const repl::Subobject& s : pawn->subobjects) scan(s.obj);
    CHECK(!unique_id, "no object of the pawn has a UniqueId");
    CHECK(leaked == 0, "no text value of the pawn holds the login's ?Name= text");
    CHECK(character_name == "cyb" && !login_text.empty(), "its CharacterName is the character's name, not the login's text");
    CHECK(pawn->subobjects.size() == 5,
          "it has the five components that show the character (Alignment, BaseCharacterInfo, CombatInfo, Stats, Appearance) and nothing of the owner's");
}

static void test_presence_the_movement_mode_other_clients_see() {
    std::printf(
        "players seeing each other: the pawn is shown in walking, falling, swimming, flying or a custom mode 16 to 18, and in walking for any other number\n");
    Harness h;
    {
        BunchWriter ready;
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        ready.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
        h.client_sends({h.client.actor(h.pawn_ch, ready, true)});
    }
    h.settle();
    const uint64_t id = static_cast<uint64_t>(h.s->bootstrap().setup().player_id);
    float ts = 1.0f;
    double x = kX;
    const auto shown = [&](uint8_t mode) -> uint64_t {
        ts += 0.05f;
        x += 10.0;
        h.send_move(move_at(ts, x, kY, kZ, kSpawnYaw, mode));
        h.step(milliseconds(50));
        const world::WorldActor* a = presence_pawn(h.fx.world->registry(), id);
        return a ? presence_motion(*a).mode : 9999;
    };
    struct Case {
        uint8_t sent;
        uint64_t expected;
    };
    for (const Case c : {Case{1, 1}, Case{3, 3}, Case{4, 4}, Case{5, 5}, Case{16, 16}, Case{17, 17}, Case{18, 18}, Case{1, 1}}) {
        char text[200];
        std::snprintf(
            text,
            sizeof(text),
            "a move in mode %u: the other players' copy of the pawn is in mode %llu",
            static_cast<unsigned>(c.sent),
            static_cast<unsigned long long>(c.expected));
        const uint64_t got = shown(c.sent);
        CHECK(got == c.expected, text);
    }
    for (const Case c : {Case{0, 1}, Case{2, 1}, Case{6, 1}, Case{10, 1}, Case{15, 1}, Case{19, 1}, Case{200, 1}, Case{255, 1}}) {
        char text[200];
        std::snprintf(
            text,
            sizeof(text),
            "a move in mode %u: the other players' copy of the pawn is in mode %llu (walking)",
            static_cast<unsigned>(c.sent),
            static_cast<unsigned long long>(c.expected));
        const uint64_t got = shown(c.sent);
        CHECK(got == c.expected, text);
    }
    CHECK(h.s->moves_refused() == 0,
          "(none of the moves was refused: every mode above reached the registry through the move checker)");
}

void run_movement_presence_tests() {
    test_endpoint_runs_the_view_passes_on_the_pool();
    test_presence_idle_join_is_in_the_world_within_two_ticks();
    test_presence_leaves_with_the_player();
    test_presence_walk_then_stop_with_no_more_moves();
    test_presence_a_server_move_is_followed_with_no_client_move();
    test_presence_churn_never_writes_the_registry_in_a_view_pass();
    test_presence_another_players_client_is_sent_one_pawn_and_no_login_text();
    test_presence_the_pawn_other_players_are_shown_has_nothing_of_the_owners();
    test_presence_the_movement_mode_other_clients_see();
}
