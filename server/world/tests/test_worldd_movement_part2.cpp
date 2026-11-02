// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_movement_common.h"

using namespace aoc::worldd;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
using emit::BunchWriter;
using std::chrono::seconds;

static void test_one_correction_per_datagram() {
    std::printf("at most one correction per datagram\n");
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const GoodPosition g = h.s->movement().good();
        std::vector<repl::NetworkMove> five;
        for (int i = 0; i < 5; ++i) five.push_back(move_at(2.0f + 0.017f * i, kX + 90000.0 + i, kY, kZ));
        const auto r = h.send_move(client_moves(five));
        CHECK(r.size() == 1 && correction_is(*r[0], five.back().time_stamp, g.x, g.y, g.z) && h.s->moves_refused() == 5 && h.s->corrections_sent() == 1,
              "five refused moves in one datagram: five refusals counted, one ClientAdjustPosition, with the latest refused move's TimeStamp");
        const auto next = h.send_move(move_at(2.2f, kX + 20.0, kY, kZ));
        CHECK(next.size() == 1 && is_ack(*next[0], 2.2f), "(and the next datagram is judged afresh)");
    }
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const auto r = h.send_move(client_moves({move_at(2.0f, kX + 90000.0, kY, kZ), move_at(2.017f, kX + 10.0, kY, kZ)}));
        CHECK(r.size() == 1 && is_ack(*r[0], 2.017f) && h.s->moves_refused() == 1 && h.s->corrections_sent() == 0,
              "a refused move, then an accepted one in the same datagram: only the acknowledgement goes out (the later move supersedes)");
    }
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        const auto r = h.send_move(client_moves({move_at(2.0f, kX + 10.0, kY, kZ), move_at(2.017f, kX + 90000.0, kY, kZ)}));
        const GoodPosition g = h.s->movement().good();
        CHECK(r.size() == 2 && is_ack(*r[0], 2.0f) && correction_is(*r[1], 2.017f, g.x, g.y, g.z) && near(g.x, kX + 10.0),
              "an accepted move, then a refused one: an acknowledgement and a correction to the accepted position");
    }
}

static void test_racing_the_time_stamp() {
    std::printf("a client that races its TimeStamp\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    float ts = 1.0f;
    double x = kX;
    std::vector<repl::NetworkMove> race;
    for (int i = 0; i < 36; ++i) {
        ts += 5.0f;
        x += 10000.0;
        race.push_back(move_at(ts, x, kY, kZ));
    }
    const BunchWriter bunch = client_moves(race);
    CHECK(bunch.bit_pos() > 0, "(the datagram of 36 racing moves builds)");
    const auto r = h.send_move(bunch);
    const GoodPosition& g = h.s->movement().good();
    const MoveLimits lim;
    std::printf("    36 racing moves in one datagram: %zu replies, %zu acknowledged, %zu refused; the player is %.0f cm from where it was\n",
                r.size(), h.s->moves_acknowledged() - 1, h.s->moves_refused(), g.x - kX);
    CHECK(g.x - kX <= lim.max_speed * lim.pace_bank + lim.slack + 1.0 && h.s->moves_refused() >= 34,
          "one datagram of 36 moves that claim 5 s each moves the player at most what the bank buys (about 100 m, not 3.6 km); the rest are refused");
    CHECK(h.s->corrections_sent() == 1, "and the 34 or more refusals are one correction");
    h.s->finish("racing");
    CHECK(h.sink.saves.size() <= 1 && (h.sink.saves.empty() || h.sink.saves[0].position.x - kX <= lim.max_speed * lim.pace_bank + lim.slack + 1.0),
          "what the character service gets, if anything, is that position, not the 3.6 km");
}

static void test_pending_and_old() {
    std::printf("pending and old moves\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    repl::MovePacked p;
    p.new_move = move_at(1.034f, kX + 10.0, kY, kZ);
    p.pending = move_at(1.017f, kX + 9000.0, kY, kZ);
    auto r = h.send_move(client_move(p));
    CHECK(r.size() == 1 && is_ack(*r[0], 1.034f) && std::fabs(h.s->movement().good().x - (kX + 10.0)) < 0.01,
          "a refused pending move does not condemn the new move: the new one is acknowledged and is the last good position");

    repl::MovePacked q;
    q.old = move_at(1.051f, kX + 12.0, kY, kZ);
    q.pending = move_at(1.068f, kX + 14.0, kY, kZ);
    q.new_move = move_at(1.085f, kX + 16.0, kY, kZ);
    r = h.send_move(client_move(q));
    CHECK(r.size() == 1 && is_ack(*r[0], 1.085f) && std::fabs(h.s->movement().good().x - (kX + 16.0)) < 0.01 && h.s->movement().good().time_stamp == 1.085f,
          "old, pending and new move, all plausible: one acknowledgement, of the new move; the last good position is the new move's");

    repl::MovePacked rep;
    rep.new_move = move_at(1.102f, kX + 18.0, kY, kZ);
    rep.pending = move_at(1.085f, kX + 16.0, kY, kZ);
    r = h.send_move(client_move(rep));
    CHECK(r.size() == 1 && is_ack(*r[0], 1.102f) && std::fabs(h.s->movement().good().x - (kX + 18.0)) < 0.01,
          "a pending move that is the previous new move (seen already) is skipped");

    repl::MovePacked back;
    back.new_move = move_at(1.0f, kX + 18.0, kY, kZ);
    back.pending = move_at(1.119f, kX + 20.0, kY, kZ);
    r = h.send_move(client_move(back));
    CHECK(r.empty() && h.s->moves_stale() == 1 && h.s->moves_refused() == 0 && std::fabs(h.s->movement().good().x - (kX + 20.0)) < 0.01 &&
              h.s->movement().good().time_stamp == 1.119f,
          "a new move whose TimeStamp went back: no reply (neither acknowledged nor corrected); its (plausible) pending move was judged first and moved the player");
}

static void test_unreadable() {
    std::printf("a parameter that does not read\n");
    Harness h;
    BunchWriter good = client_move(move_at(1.0f, kX, kY, kZ));
    const auto parsed = repl::read_client_bunch(
        class_named("PlayerPawn_C"), [](const emit::FIntrepidNetworkGUID&) { return nullptr; }, good.data(), good.bit_pos());
    const size_t flip = parsed && parsed->rpcs.size() == 1 ? parsed->rpcs[0].call.param_pos + 3 : 0;
    CHECK(flip > 0, "(the move's parameter is found in its bunch)");
    std::vector<uint8_t> bytes(good.data(), good.data() + good.byte_size());
    bytes[flip >> 3] ^= static_cast<uint8_t>(1u << (flip & 7));
    BunchWriter bad;
    bad.write_bit_range(bytes.data(), 0, good.bit_pos());
    g_log.str("");
    const size_t acked = h.s->moves_acknowledged();
    auto r = h.send_move(bad);
    r = h.send_move(bad);
    CHECK(r.empty() && h.s->moves_acknowledged() == acked && h.s->corrections_sent() == 0 && h.s->moves_refused() == 0,
          "a ServerMovePacked that does not read gets neither an acknowledgement nor a correction (the client resends)");
    spdlog::default_logger()->flush();
    size_t lines = 0;
    for (size_t at = g_log.str().find("does not read ("); at != std::string::npos; at = g_log.str().find("does not read (", at + 1)) ++lines;
    CHECK(lines == 1, "and it is logged once, with the reason");
}

static void test_low_limit() {
    std::printf("--max-move-speed\n");
    MovementConfig mc;
    mc.limits.max_speed = 300.0;
    Harness h(mc);
    h.send_move(move_at(1.0f, kX, kY, kZ));
    float ts = 1.0f;
    double x = kX;
    size_t acks = 0, fixes = 0;
    bool fixes_at_last_good = true;
    for (int i = 0; i < 30; ++i) {
        ts += 0.1f;
        x += 60.0;
        const GoodPosition g = h.s->movement().good();
        for (const auto* d : h.send_move(move_at(ts, x, kY, kZ))) {
            if (is_ack(*d, ts)) ++acks;
            else if (correction_is(*d, ts, g.x, g.y, g.z)) ++fixes;
            else fixes_at_last_good = false;
        }
    }
    CHECK(fixes_at_last_good && fixes >= 25 && acks <= 3,
          "at a limit of 300 cm/s a walk at 600 cm/s is mostly corrected, each time to the last good position (walking snaps back)");
    CHECK(h.s->movement().limits().max_speed == 300.0, "(the session has the world's limit)");
}

static void test_refusal_logging() {
    std::printf("logging refusals\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    g_log.str("");
    float ts = 1.0f;
    for (int i = 0; i < 30; ++i) {
        ts += 0.017f;
        h.send_move(move_at(ts, kX + 50000.0, kY, kZ));
    }
    spdlog::default_logger()->flush();
    const std::string log = g_log.str();
    size_t info = 0, debug = 0;
    for (size_t at = log.find("is refused"); at != std::string::npos; at = log.find("is refused", at + 1)) {
        const size_t line = log.rfind('\n', at);
        const size_t start = line == std::string::npos ? 0 : line;
        const std::string head = log.substr(start, at - start);
        if (head.find("[info]") != std::string::npos) ++info;
        else if (head.find("[debug]") != std::string::npos) ++debug;
    }
    CHECK(info == PlayerSession::kLoggedPerKind && debug == 30 - PlayerSession::kLoggedPerKind, "30 refusals: the first three at info, the rest at debug");
    CHECK(log.find("too fast along the ground") != std::string::npos && log.find("put back at") != std::string::npos,
          "each line says why and where the player is put back");
}

static void check_session_end_saves() {
    {
        Harness h;
        h.s->finish("test");
        CHECK(h.sink.saves.empty(), "a session that never moved saves nothing at its end");
    }
    {
        Harness h;
        h.send_move(move_at(1.0f, kX, kY, kZ));
        h.s->finish("test");
        CHECK(h.sink.saves.empty(), "moves that stayed at the loaded position save nothing");
    }
    {
        Harness h;
        float ts = 1.0f;
        double x = kX;
        for (int i = 0; i < 10; ++i) {
            ts += 0.017f;
            x += 20.0;
            h.send_move(move_at(ts, x, kY, kZ + 1.0, 8192));
        }
        h.send_move(move_at(ts + 0.017f, x + 99999.0, kY, kZ));
        h.s->finish("the client quit");
        CHECK(h.sink.saves.size() == 1 && h.sink.saves[0].id == kCyb, "the end of the session hands one position to the sink, with the character's id");
        if (h.sink.saves.size() == 1) {
            const svc::Position& p = h.sink.saves[0].position;
            CHECK(std::fabs(p.x - x) < 0.01 && std::fabs(p.y - kY) < 0.01 && std::fabs(p.z - (kZ + 1.0)) < 0.01 && std::fabs(p.yaw - 45.0f) < 1e-4,
                  "it is the last good position, not the refused teleport: x, y, z in cm and the yaw of the control rotation in degrees (8192 is 45)");
        }
        h.s->finish("again");
        CHECK(h.sink.saves.size() == 1 && h.s->positions_handed_over() == 1, "finish is once: a second call saves nothing");
    }
}

static void check_session_periodic_saves() {
    {
        Harness h;
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX + 10.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(59));
        CHECK(h.sink.saves.empty(), "59 s after the first tick: nothing is saved yet");
        h.s->tick(h.conn, start + seconds(60));
        CHECK(h.sink.saves.size() == 1 && std::fabs(h.sink.saves[0].position.x - (kX + 10.0)) < 0.01, "at 60 s the position that changed is saved");
        h.s->tick(h.conn, start + seconds(120));
        CHECK(h.sink.saves.size() == 1, "at 120 s nothing moved since: no second save of the same position");
        h.send_move(move_at(1.017f, kX + 30.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(179));
        CHECK(h.sink.saves.size() == 1, "(a move at 120 s is not saved at 179 s)");
        h.s->tick(h.conn, start + seconds(180));
        CHECK(h.sink.saves.size() == 2 && std::fabs(h.sink.saves[1].position.x - (kX + 30.0)) < 0.01, "but at 180 s");
        h.s->finish("end");
        CHECK(h.sink.saves.size() == 3 && std::fabs(h.sink.saves[2].position.x - (kX + 30.0)) < 0.01,
              "the end of the session hands over the last good position again, although nothing moved since the last save: the saver may have lost that one, and the record is what the session compares with");
        h.send_move(move_at(1.034f, kX + 40.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(240));
        CHECK(h.sink.saves.size() == 3, "after finish the session saves no more");
    }
    {
        Harness h;
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX + 500.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(60));
        CHECK(h.sink.saves.size() == 1 && std::fabs(h.sink.saves[0].position.x - (kX + 500.0)) < 0.01, "(the 60 s save holds the position 5 m away)");
        h.send_move(move_at(1.5f, kX, kY, kZ));
        h.s->finish("end");
        CHECK(h.sink.saves.size() == 2 && std::fabs(h.sink.saves[1].position.x - kX) < 0.01,
              "back at the loaded position when the session ends: the end hands it over, so the record is not left with the position of the 60 s save");
    }
    {
        Harness h;
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX + 5.0, kY, kZ));
        h.send_move(move_at(1.017f, kX + 90000.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(61));
        CHECK(h.sink.saves.size() == 1 && std::fabs(h.sink.saves[0].position.x - (kX + 5.0)) < 0.01,
              "a periodic save after a refused move holds the last good position");
    }
}

static void check_session_interval_sink_and_yaw() {
    {
        MovementConfig mc;
        mc.save_interval = seconds(10);
        Harness h(mc);
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX + 5.0, kY, kZ));
        h.s->tick(h.conn, start + seconds(10));
        CHECK(h.sink.saves.size() == 1, "the interval is the world's save_interval");
    }
    {
        Harness h({}, false);
        h.send_move(move_at(1.0f, kX + 5.0, kY, kZ));
        h.s->tick(h.conn, h.t);
        h.s->tick(h.conn, h.t + seconds(100));
        h.s->finish("end");
        CHECK(h.s->positions_handed_over() == 0 && h.s->moves_acknowledged() == 1, "without a PositionSink nothing is handed over");
    }
    {
        Harness h;
        const Clock::time_point start = h.t;
        h.s->tick(h.conn, start);
        h.send_move(move_at(1.0f, kX, kY, kZ, 4000));
        h.s->tick(h.conn, start + seconds(61));
        CHECK(h.sink.saves.empty(), "turning on the spot is not 'moved': the periodic save skips it");
        h.s->finish("end");
        CHECK(h.sink.saves.size() == 1 && std::fabs(h.sink.saves[0].position.yaw - 4000.0f * 360.0f / 65536.0f) < 1e-3,
              "but the end of the session saves the new yaw");
    }
}

static void test_saving_in_the_session() {
    std::printf("saving, in the session\n");
    check_session_end_saves();
    check_session_periodic_saves();
    check_session_interval_sink_and_yaw();
}

void run_movement_part2() {
    test_one_correction_per_datagram();
    test_racing_the_time_stamp();
    test_pending_and_old();
    test_unreadable();
    test_low_limit();
    test_refusal_logging();
    test_saving_in_the_session();
}
