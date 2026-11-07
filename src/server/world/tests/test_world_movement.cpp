// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_movement_common.h"

#include "bootstrap/captured_setup.h"
#include "capture_blocks.h"
#include "capture_moves.h"
#include "nav_fixture.h"

#include <limits>
#include <spdlog/sinks/ostream_sink.h>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;

static repl::NetworkMove move_with_mode(float ts, double x, double y, double z, uint8_t mode) {
    return move_at(ts, x, y, z, kSpawnYaw, mode);
}

static void test_correction_wire() {
    std::printf("PlayerBootstrap::correction\n");
    auto setup = bs::captured_character_setup();
    if (!setup) {
        std::printf("  [FAIL] the captured character's setup does not build\n");
        std::exit(2);
    }
    bs::PlayerBootstrap pb(std::move(*setup), bs::GameStateMode::Tier2);
    CHECK(pb.ok(), "(the captured character's bootstrap builds)");
    const repl::Vec3d loc{-777664.5, 616578.6875, 15952.0498046875};
    const auto fix = pb.correction(12.375f, loc, repl::Vec3d{}, repl::kMoveWalking);
    CHECK(fix.has_value(), "a correction builds");
    if (!fix) return;
    CHECK(fix->role == bs::Role::Pawn && !fix->opens_channel && !fix->reliable() && fix->emission.exports.empty(),
          "it goes on the pawn's channel, unreliable (the function has no FUNC_NetReliable: flags 0x01820c42), with no exports");
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const DecodedRpc d = decode_rpc(fix->emission.payload, pawn);
    CHECK(d.ok && d.name == "ClientAdjustPosition", "the client's reader finds one RPC of the pawn: ClientAdjustPosition, its parameters read to the last bit");
    if (!d.ok) return;
    const repl::Value* ts = param(d, "TimeStamp");
    const repl::Value* nl = param(d, "NewLoc");
    const repl::Value* nv = param(d, "NewVel");
    const repl::Value* mode = param(d, "ServerMovementMode");
    CHECK(ts && ts->get_if<float>() && *ts->get_if<float>() == 12.375f, "TimeStamp 12.375, the refused move's");
    CHECK(nl && nl->get_if<repl::Vec3d>() && *nl->get_if<repl::Vec3d>() == loc, "NewLoc is the last good position, three doubles");
    CHECK(!nv || (nv->get_if<repl::Vec3d>() && *nv->get_if<repl::Vec3d>() == repl::Vec3d{}), "NewVel is zero (world does not simulate)");
    CHECK(mode && mode->get_if<uint64_t>() && *mode->get_if<uint64_t>() == repl::kMoveWalking, "ServerMovementMode is the last good move's: walking (1)");
    const repl::Value* base = param(d, "NewBase");
    const repl::Value* has_base = param(d, "bHasBase");
    const repl::Value* relative = param(d, "bBaseRelativePosition");
    CHECK(!base && (!has_base || *has_base == repl::Value::boolean(false)) && (!relative || *relative == repl::Value::boolean(false)),
          "no base: NewBase unset, bHasBase and bBaseRelativePosition false");

    const auto falling = pb.correction(12.375f, loc, repl::Vec3d{0, 0, -100}, repl::kMoveFalling);
    const DecodedRpc f = falling ? decode_rpc(falling->emission.payload, pawn) : DecodedRpc{};
    const repl::Value* fv = f.ok ? param(f, "NewVel") : nullptr;
    const repl::Value* fm = f.ok ? param(f, "ServerMovementMode") : nullptr;
    CHECK(fv && fv->get_if<repl::Vec3d>() && fv->get_if<repl::Vec3d>()->z == -100 && fm && *fm->get_if<uint64_t>() == repl::kMoveFalling,
          "another velocity and mode come back as given");
    const auto second = pb.correction(12.375f, loc, repl::Vec3d{}, repl::kMoveWalking);
    CHECK(second && second->emission.payload.bit_pos() == fix->emission.payload.bit_pos() && fix->emission.payload.bit_pos() > 2 * 192,
          "a second correction is the same bits (it touches no shadow) and more than its two vectors' 384");
}

static BunchWriter client_move_raw(const capture_moves::Move& mv) {
    const schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const schema::NetField* f = cls.field("ServerMovePacked");
    const auto bytes = capture_blocks::bytes_of(mv.hex);
    BunchWriter params;
    params.write_bit_range(bytes.data(), 0, mv.bits);
    BunchWriter field;
    BunchWriter out;
    if (!f || repl::write_field(cls, f->index, params, field) == 0) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, false);
    return out;
}

static void check_captured_moves_replay(bool native) {
    Harness c;
    size_t answered = 0, acks_ok = 0;
    const capture_moves::Move* first = native ? std::begin(capture_moves::kNative) : std::begin(capture_moves::kE2e);
    const capture_moves::Move* last = native ? std::end(capture_moves::kNative) : std::end(capture_moves::kE2e);
    double first_ts = -1;
    for (const capture_moves::Move* mv = first; mv != last; ++mv) {
        const auto data = capture_blocks::bytes_of(mv->hex);
        const auto parsed = repl::read_move_packed(data.data(), data.size(), 0, mv->bits);
        if (parsed && first_ts < 0) first_ts = parsed->new_move.time_stamp;
        if (parsed) {
            const Clock::time_point at = t0 + std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(parsed->new_move.time_stamp - first_ts));
            if (at > c.t) c.t = at;
        }
        const auto replies = c.send_move(client_move_raw(*mv));
        answered += replies.size();
        if (replies.size() == 1 && parsed && is_ack(*replies[0], parsed->new_move.time_stamp)) ++acks_ok;
    }
    const size_t n = static_cast<size_t>(last - first);
    CHECK(answered == n && acks_ok == n && c.s->moves_acknowledged() == n && c.s->moves_refused() == 0 && c.s->corrections_sent() == 0,
          native ? "all 1,843 moves of the 2026-09-29 run (walking, sprint, dodge, jumps), as the client sent them: each acknowledged with its own TimeStamp, none refused"
                 : "all 640 moves of the e2e capture, as the client sent them: each acknowledged with its own TimeStamp, none refused");
    const auto last_data = capture_blocks::bytes_of((last - 1)->hex);
    const auto lp = repl::read_move_packed(last_data.data(), last_data.size(), 0, (last - 1)->bits);
    CHECK(lp && c.s->movement().good().time_stamp == lp->new_move.time_stamp && near(c.s->movement().good().x, lp->new_move.location_cm().x) &&
              near(c.s->movement().good().y, lp->new_move.location_cm().y),
          "(and the last good position is the capture's last move)");
}

static void test_good_moves() {
    std::printf("a walk\n");
    Harness h;
    float ts = 1.0f;
    double x = kX;
    size_t acked = 0;
    bool all_acks = true;
    for (int i = 0; i < 20; ++i) {
        ts += 0.017f;
        x += 10.0;
        const auto replies = h.send_move(move_at(ts, x, kY, kZ));
        all_acks = all_acks && replies.size() == 1 && is_ack(*replies[0], ts);
        acked += replies.size();
    }
    CHECK(all_acks && acked == 20 && h.s->moves_acknowledged() == 20, "20 moves a frame apart are each answered with a ClientMoveResponsePacked of their own TimeStamp");
    CHECK(h.s->moves_refused() == 0 && h.s->corrections_sent() == 0, "none refused, none corrected");
    const GoodPosition& g = h.s->movement().good();
    CHECK(near(g.x, x) && near(g.y, kY) && g.time_stamp == ts && std::fabs(g.yaw_degrees - kSpawnYaw * 360.0 / 65536.0) < 1e-9,
          "the last good position is the last move's (location, TimeStamp, the control rotation's yaw in degrees)");

    for (const bool native : {false, true}) {
        check_captured_moves_replay(native);
    }
}

static void check_correction_modes_and_nan(Harness& h, double x, const GoodPosition& g) {
    const auto back = h.send_move(move_at(0.5f, x + 10.0, kY, kZ));
    CHECK(back.empty() && h.s->moves_refused() == 1 && h.s->corrections_sent() == 1 && h.s->moves_stale() == 1 && h.s->movement().good() == g,
          "a TimeStamp going back: no acknowledgement, no correction, not counted as refused (counted apart), nothing changes");
    const auto again = h.send_move(move_at(g.time_stamp, x + 10.0, kY, kZ));
    CHECK(again.empty() && h.s->moves_refused() == 1 && h.s->corrections_sent() == 1 && h.s->moves_stale() == 2 && h.s->movement().good() == g,
          "the same TimeStamp again (a duplicate datagram): the same");
    const auto zero = h.send_move(move_at(0.0f, x + 10.0, kY, kZ));
    CHECK(zero.empty() && h.s->moves_stale() == 3, "a TimeStamp of 0 (UE's IsClientTimeStampValid: ignored): the same");

    repl::NetworkMove nan = move_at(1.068f, x + 10.0, kY, kZ);
    nan.location.mode = repl::QuantVec::Mode::RawDouble;
    nan.location.x = static_cast<int64_t>(std::bit_cast<uint64_t>(std::numeric_limits<double>::quiet_NaN()));
    nan.location.y = static_cast<int64_t>(std::bit_cast<uint64_t>(kY));
    nan.location.z = static_cast<int64_t>(std::bit_cast<uint64_t>(kZ));
    const auto nans = h.send_move(nan);
    CHECK(nans.size() == 1 && correction_is(*nans[0], 1.068f, g.x, g.y, g.z), "a Location with a NaN: corrected");
    const auto nan_ts = h.send_move(move_at(std::numeric_limits<float>::quiet_NaN(), x + 10.0, kY, kZ));
    CHECK(nan_ts.size() == 1 && correction_is(*nan_ts[0], 0.0f, g.x, g.y, g.z), "a TimeStamp that is NaN: corrected, with TimeStamp 0 (a NaN names no saved move)");
    const auto far = h.send_move(move_at(1.085f, 2000000.0, kY, kZ));
    CHECK(far.size() == 1 && correction_is(*far[0], 1.085f, g.x, g.y, g.z), "a location outside the world: corrected");
    const auto up = h.send_move(move_at(1.102f, x + 10.0, kY, kZ + 900.0));
    CHECK(up.size() == 1 && correction_is(*up[0], 1.102f, g.x, g.y, g.z), "9 m up in a frame: corrected");
    CHECK(h.s->moves_refused() == 5 && h.s->corrections_sent() == 5, "five refused (the teleport, a NaN location, a NaN TimeStamp, outside the world, 9 m up), five corrections");
    const auto ok = h.send_move(move_at(1.119f, x + 15.0, kY, kZ));
    CHECK(ok.size() == 1 && is_ack(*ok[0], 1.119f), "and the client is acknowledged again once it is plausible");

    h.send_move(move_with_mode(1.136f, x + 15.0, kY, kZ - 10.0, repl::kMoveFalling));
    const GoodPosition fall = h.s->movement().good();
    const auto fall_fix = h.send_move(move_with_mode(1.153f, x + 9000.0, kY, kZ - 10.0, repl::kMoveFalling));
    CHECK(fall_fix.size() == 1 && correction_is(*fall_fix[0], 1.153f, fall, repl::kMoveFalling), "the correction carries the last good move's movement mode (falling)");

    h.send_move(move_with_mode(1.17f, x + 15.0, kY, kZ - 10.0, 0));
    const GoodPosition none = h.s->movement().good();
    CHECK(none.movement_mode == 0 && none.time_stamp == 1.17f, "(a move in mode 0 is accepted and kept as it is)");
    const auto none_fix = h.send_move(move_with_mode(1.187f, x + 9000.0, kY, kZ - 10.0, 0));
    CHECK(none_fix.size() == 1 && correction_is(*none_fix[0], 1.187f, none, repl::kMoveWalking),
          "the correction after it says walking (1), never mode 0: a client put back in mode 0 would not walk");
}

static void test_corrections() {
    std::printf("a move that is not plausible\n");
    Harness h;
    h.send_move(move_at(1.0f, kX, kY, kZ));
    const double x = kX + 20.0;
    h.send_move(move_at(1.017f, x, kY, kZ));
    const size_t acked = h.s->moves_acknowledged();
    const GoodPosition before = h.s->movement().good();

    bs::PlayerBootstrap pb(h.s->bootstrap().setup(), bs::GameStateMode::Tier2);
    const auto replies = h.send_move(move_at(1.034f, x + 5000.0, kY, kZ));
    CHECK(replies.size() == 1 && correction_is(*replies[0], 1.034f, before.x, before.y, before.z), "a teleport: one ClientAdjustPosition with the move's TimeStamp and the last good position");
    const auto want = pb.correction(1.034f, repl::Vec3d{before.x, before.y, before.z}, repl::Vec3d{}, repl::kMoveWalking);
    CHECK(replies.size() == 1 && want && replies[0]->content.bit_pos() == want->emission.payload.bit_pos() &&
              capture_blocks::same_bit_range(replies[0]->content.data(), want->emission.payload.data(), want->emission.payload.bit_pos(), nullptr),
          "its bits are PlayerBootstrap::correction's");
    CHECK(h.s->moves_acknowledged() == acked && h.s->moves_refused() == 1 && h.s->corrections_sent() == 1 && h.s->movement().good() == before,
          "the move is not acknowledged and the last good position is as it was");

    const auto next = h.send_move(move_at(1.051f, x + 10.0, kY, kZ));
    CHECK(next.size() == 1 && is_ack(*next[0], 1.051f), "the next move, near the last good position, is acknowledged");
    const GoodPosition g = h.s->movement().good();
    check_correction_modes_and_nan(h, x, g);
}

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

static void adopt_ground_and_rejoin(Harness& h) {
    h.s.reset();
    std::string err;
    auto mesh = nav::AreaNav::from_file(navfix::build_file(), &err);
    if (!mesh || !h.fx.world->adopt_ground(std::move(mesh), err)) {
        std::printf("  [FAIL] the fixture's ground does not load: %s\n", err.c_str());
        std::exit(2);
    }
    h.got.clear();
    h.t = t0;
    h.conn = Connection{
        ConnectionConfig{
            aoc::protocol::conn::OuterHeader{0x500c7696u, 0, 5},
            aoc::protocol::conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d},
            14265,
            15870},
        t0};
    h.client = fake::Client{
        aoc::protocol::conn::OuterHeader{0x500c7696u, 0, 5},
        aoc::protocol::conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d},
        14265,
        15870};
    h.s = PlayerSession::join(h.conn, *h.fx.world, cyb_record(), identity(), world_time_at(h.t), h.error);
    if (!h.s) {
        std::printf("  [FAIL] the session does not join: %s\n", h.error.c_str());
        std::exit(2);
    }
    h.pawn_ch = h.s->channel_of(bs::Role::Pawn);
    h.settle();
}

static void place_on_ground(Harness& h, double x, double y, double z) {
    const PlayerSession::GmMoveResult moved = h.s->gm_teleport(h.conn, x, y, z, h.t, "the test: on the ground");
    CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved, "(the player is put on the fixture's ground)");
    h.flush();
    h.step(milliseconds(3000));
}

static void test_moves_under_the_ground() {
    std::printf("a walking move under the ground\n");
    Harness h;
    adopt_ground_and_rejoin(h);
    float ts = 1.0f;

    place_on_ground(h, 2000, 0, 1000);
    auto replies = h.send_move(move_at(ts, 2000, 0, 1000));
    CHECK(replies.size() == 1 && is_ack(*replies[0], ts), "(a walking move on the fixture's ground is acknowledged)");
    h.step(milliseconds(1000));
    const GoodPosition on_ground = h.s->movement().good();
    ts += 1.0f;
    replies = h.send_move(move_at(ts, 2100, 0, 600));
    CHECK(replies.size() == 1 && correction_is(*replies[0], ts, on_ground.x, on_ground.y, on_ground.z) && h.s->moves_refused() == 1,
          "a walking move 4 m under the ground is refused: one ClientAdjustPosition to the last good position");
    CHECK(h.s->movement().good() == on_ground, "(the last good position is the ground's: the refused move changed nothing)");

    h.step(milliseconds(1000));
    ts += 1.0f;
    replies = h.send_move(move_at(ts, 2200, 0, 900));
    CHECK(replies.size() == 1 && is_ack(*replies[0], ts) && h.s->moves_refused() == 1, "a walking move 1 m under the ground is acknowledged: the allowance is 300 cm");
}

int main() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    test_correction_wire();
    test_good_moves();
    test_corrections();
    test_one_correction_per_datagram();
    test_racing_the_time_stamp();
    test_pending_and_old();
    test_unreadable();
    test_low_limit();
    test_refusal_logging();
    test_saving_in_the_session();
    run_movement_saver();
    run_movement_based_tests();
    run_movement_presence_tests();
    test_moves_under_the_ground();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
