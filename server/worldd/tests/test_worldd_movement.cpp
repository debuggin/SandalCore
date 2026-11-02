// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_movement_common.h"

#include "bootstrap/captured_setup.h"
#include "capture_blocks.h"
#include "capture_moves.h"

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;

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
    CHECK(!nv || (nv->get_if<repl::Vec3d>() && *nv->get_if<repl::Vec3d>() == repl::Vec3d{}), "NewVel is zero (worldd does not simulate)");
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

static void check_saver_queue_and_bounds() {
    {
        FakeCharacters api;
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(1, 2, 3, 90.5f));
        CHECK(saver.flush(seconds(3)), "a position is saved; flush says everything is");
        const auto s = api.saved();
        CHECK(s.size() == 1 && s[0].id == "aa" && s[0].position.x == 1 && s[0].position.y == 2 && s[0].position.z == 3 && s[0].position.yaw == 90.5f &&
                  saver.saved() == 1 && saver.failed() == 0 && saver.queued() == 0,
              "the character service got the id and the position as given");
    }
    {
        FakeCharacters api;
        api.hold();
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(1));
        CHECK(wait_until([&] { return api.in_flight(); }), "(the worker is inside the first call)");
        saver.save("aa", pos(2));
        saver.save("aa", pos(3));
        saver.save("aa", pos(4));
        saver.save("bb", pos(10));
        CHECK(saver.queued() == 2 && saver.replaced() == 2, "while it is busy a character has one entry: later positions replace the waiting one (two replaced)");
        api.release();
        CHECK(saver.flush(seconds(3)), "(released)");
        const auto s = api.saved();
        CHECK(s.size() == 3 && s[0].id == "aa" && s[0].position.x == 1 && s[1].id == "aa" && s[1].position.x == 4 && s[2].id == "bb" && s[2].position.x == 10,
              "the first position was already on its way, then the latest of aa (4, not 2 or 3) and bb: three calls, in the order the characters first waited");
    }
    {
        FakeCharacters api;
        api.hold();
        CharacterSaver saver(api, quick());
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 20000; ++i) saver.save("c" + std::to_string(i % 50), pos(i));
        const auto took = std::chrono::steady_clock::now() - begin;
        CHECK(took < seconds(1) && saver.queued() <= 50, "20,000 saves for 50 characters while the service hangs return at once: at most one entry each");
        api.release();
        saver.flush(seconds(3));
    }
    {
        FakeCharacters api;
        api.hold();
        CharacterSaver::Config cfg = quick();
        cfg.max_queued = 3;
        CharacterSaver saver(api, cfg);
        saver.save("c0", pos(0));
        wait_until([&] { return api.in_flight(); });
        for (int i = 1; i <= 4; ++i) saver.save("c" + std::to_string(i), pos(i));
        CHECK(saver.queued() == 3 && saver.dropped() == 1, "a fourth waiting character: the oldest entry (c1) is dropped and counted");
        api.release();
        saver.flush(seconds(3));
        const auto s = api.saved();
        CHECK(s.size() == 4 && s[0].id == "c0" && s[1].id == "c2" && s[2].id == "c3" && s[3].id == "c4", "c0 (in flight), c2, c3, c4 were saved; c1 was not");
    }
}

static void check_saver_retries_and_errors() {
    {
        FakeCharacters api;
        api.script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(7));
        CHECK(saver.flush(seconds(3)) && api.calls() == 3 && saver.saved() == 1 && saver.failed() == 0 && api.saved().size() == 1,
              "an Unavailable service is tried again: two refusals then Ok is one saved position, not a failure");
    }
    {
        FakeCharacters api;
        api.script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(7));
        saver.flush(seconds(3));
        CHECK(api.calls() == 3 && saver.saved() == 0 && saver.failed() == 1 && saver.queued() == 0,
              "three Unavailable answers (max_attempts) and the position is given up: counted as failed");
    }
    {
        FakeCharacters api;
        api.script = {svc::CallStatus::NotFound};
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(7));
        saver.flush(seconds(3));
        api.script = {svc::CallStatus::Refused};
        saver.save("bb", pos(8));
        saver.flush(seconds(3));
        CHECK(api.calls() == 2 && saver.failed() == 2 && saver.saved() == 0, "NotFound and Refused are final: one call each, no retry");
    }
    {
        FakeCharacters api;
        api.script = {svc::CallStatus::Unavailable, svc::CallStatus::Ok};
        CharacterSaver::Config cfg = quick();
        cfg.retry_delay = milliseconds(300);
        CharacterSaver saver(api, cfg);
        saver.save("aa", pos(1));
        wait_until([&] { return api.calls() == 1; });
        saver.save("aa", pos(2));
        CHECK(!saver.flush(milliseconds(30)), "(flush does not report an empty queue while a retry is due)");
        CHECK(saver.flush(seconds(3)), "(after the pause everything is saved)");
        const auto s = api.saved();
        CHECK(s.size() == 1 && s[0].position.x == 2 && api.calls() == 2, "the retry sends the newer position (2), not the old one: two calls, one saved");
    }
    {
        FakeCharacters api;
        api.throw_once = true;
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(1));
        saver.flush(seconds(3));
        saver.save("bb", pos(2));
        CHECK(saver.flush(seconds(3)) && saver.failed() == 1 && saver.saved() == 1, "a call that throws is a failure and the worker goes on: the next position is saved");
    }
}

static void check_saver_flush_and_destructor() {
    {
        FakeCharacters api;
        api.hold();
        api.max_block = milliseconds(400);
        CharacterSaver saver(api, quick());
        saver.save("aa", pos(1));
        const auto begin = std::chrono::steady_clock::now();
        const bool done = saver.flush(milliseconds(50));
        const auto took = std::chrono::steady_clock::now() - begin;
        CHECK(!done && took >= milliseconds(40) && took < milliseconds(300), "flush(50 ms) on a hung service returns false after about 50 ms");
        api.release();
        CHECK(saver.flush(seconds(3)), "(released: flushed)");
    }
    {
        FakeCharacters api;
        {
            CharacterSaver saver(api, quick());
            saver.save("aa", pos(1));
            saver.save("bb", pos(2));
        }
        CHECK(api.saved_count() == 2, "the destructor flushes what is queued");
    }
    {
        FakeCharacters api;
        api.hold();
        api.max_block = milliseconds(300);
        const auto begin = std::chrono::steady_clock::now();
        {
            CharacterSaver saver(api, quick(milliseconds(50)));
            saver.save("aa", pos(1));
            wait_until([&] { return api.in_flight(); });
            saver.save("bb", pos(2));
        }
        const auto took = std::chrono::steady_clock::now() - begin;
        const auto s = api.saved();
        CHECK(took < seconds(2) && s.size() == 1 && s[0].id == "aa",
              "with a hung service the destructor gives up after its bound: the call in flight finished, the waiting one was left behind");
        spdlog::default_logger()->flush();
        CHECK(g_log.str().find("not saved") != std::string::npos, "and it says so");
    }
}

static void test_saver() {
    std::printf("CharacterSaver\n");
    check_saver_queue_and_bounds();
    check_saver_retries_and_errors();
    check_saver_flush_and_destructor();
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

int main() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);

    test_correction_wire();
    test_saver();
    test_good_moves();
    test_corrections();
    run_movement_part2();
    run_movement_part3();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
