// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_worldd_movement_common.h"

#include "capture_packets.h"

using namespace aoc::worldd;
namespace conn = aoc::protocol::conn;
using std::chrono::milliseconds;
using std::chrono::seconds;

static uint8_t g_counter = 1;
static void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
}

template <class Pred>
static bool wait_until(Pred pred, milliseconds limit = milliseconds(3000)) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

struct FakeCharacters : svc::CharacterApi {
    struct Save {
        std::string id;
        svc::Position position;
    };
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Save> saves;
    std::vector<svc::CallStatus> answers;
    std::deque<svc::CallStatus> script;
    bool block = false;
    milliseconds max_block{2000};
    int running = 0;
    bool throw_once = false;

    svc::CharacterResult get_character(const std::string&) override { return {}; }

    svc::CallStatus save_position(const std::string& id, const svc::Position& position) override {
        std::unique_lock<std::mutex> lock(mu);
        ++running;
        cv.notify_all();
        cv.wait_for(lock, max_block, [this] { return !block; });
        svc::CallStatus status = svc::CallStatus::Ok;
        if (!script.empty()) {
            status = script.front();
            script.pop_front();
        }
        --running;
        if (throw_once) {
            throw_once = false;
            answers.push_back(svc::CallStatus::Failed);
            cv.notify_all();
            throw std::runtime_error("the fake service threw");
        }
        answers.push_back(status);
        if (status == svc::CallStatus::Ok) saves.push_back({id, position});
        cv.notify_all();
        return status;
    }

    void hold() { std::lock_guard<std::mutex> l(mu); block = true; }
    void release() {
        { std::lock_guard<std::mutex> l(mu); block = false; }
        cv.notify_all();
    }
    size_t calls() { std::lock_guard<std::mutex> l(mu); return answers.size(); }
    size_t saved_count() { std::lock_guard<std::mutex> l(mu); return saves.size(); }
    std::vector<Save> saved() { std::lock_guard<std::mutex> l(mu); return saves; }
    bool in_flight() { std::lock_guard<std::mutex> l(mu); return running > 0; }
};

static svc::Position pos(double x, double y = 0, double z = 0, float yaw = 0) {
    return {x, y, z, yaw};
}

static CharacterSaver::Config quick(milliseconds destructor_wait = milliseconds(2000)) {
    CharacterSaver::Config c;
    c.retry_delay = milliseconds(5);
    c.destructor_wait = destructor_wait;
    return c;
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

struct FakeLogins : LoginQueue {
    std::vector<std::pair<uint64_t, LoginRequest>> submitted;
    void submit(uint64_t id, LoginRequest r) override { submitted.emplace_back(id, std::move(r)); }
};

struct Rig {
    CookieSigner signer{counting_random};
    FakeLogins logins;
    std::vector<std::pair<Address, std::vector<uint8_t>>> sent;
    Endpoint ep;

    explicit Rig(EndpointConfig cfg)
        : ep(
              std::move(cfg),
              signer,
              logins,
              [this](const Address& a, const std::vector<uint8_t>& p) { sent.emplace_back(a, p); },
              counting_random,
              t0) {}

    std::vector<std::vector<uint8_t>> take(const Address& a) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<std::pair<Address, std::vector<uint8_t>>> rest;
        for (auto& [to, p] : sent) {
            if (to == a) out.push_back(p);
            else rest.emplace_back(to, p);
        }
        sent.swap(rest);
        return out;
    }
};

struct Joined {
    Address addr = kA;
    uint32_t pawn_ch = 5;
    std::optional<conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    uint64_t session = 0;
    std::vector<fake::Delivered> got;
};

static Joined handshake(Rig& rig, const Address& addr, Clock::time_point now) {
    Joined j;
    j.addr = addr;
    rig.take(addr);
    const auto initial = capture::bytes(capture::kInitial);
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1) return j;
    j.challenge = conn::parse_handshake(out[0].data(), out[0].size());
    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    const auto bytes = conn::build_handshake(resp);
    rig.ep.on_datagram(addr, bytes.data(), bytes.size(), now);
    out = rig.take(addr);
    if (out.size() != 1) return j;
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server, seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}

static void drain(Rig& rig, Joined& j, const Address& addr) {
    for (const auto& p : rig.take(addr)) {
        const auto r = j.client->receive(p);
        for (const auto& d : r.delivered) j.got.push_back(d);
    }
}

static void exchange(Rig& rig, Joined& j, const Address& addr, std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}

static void log_in(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login(kMachine, kMachine)}, now + milliseconds(2));
}

static void join_world(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    log_in(rig, j, addr, rec, now);
    exchange(rig, j, addr, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, now + milliseconds(40));
}

static void walk(Rig& rig, Joined& j, float ts, double x, Clock::time_point now) {
    exchange(rig, j, j.addr, {j.client->actor(j.pawn_ch, client_move(move_at(ts, x, kY, kZ, 8192)), false)}, now);
}

struct Endpoints {
    Fixture fx;
    FakeCharacters api;
    std::unique_ptr<CharacterSaver> saver;
    EndpointConfig cfg;
    std::unique_ptr<Rig> rig;
    Endpoints() {
        saver = std::make_unique<CharacterSaver>(api, quick());
        fx.world->set_position_sink(saver.get());
        cfg.players = fx.world.get();
        rig = std::make_unique<Rig>(cfg);
    }
    ~Endpoints() {
        rig.reset();
        saver.reset();
    }
    std::vector<FakeCharacters::Save> saves() {
        saver->flush(seconds(3));
        return api.saved();
    }
};

static void check_endpoint_quit_timeout_kick_stop() {
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        walk(rig, j, 1.0f, kX + 10.0, t0 + milliseconds(100));
        walk(rig, j, 1.017f, kX + 25.0, t0 + milliseconds(120));
        CHECK(rig.ep.session_count() == 1 && e.api.saved_count() == 0, "(a player in the world, two moves, nothing saved yet)");
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && s[0].id == kCyb && std::fabs(s[0].position.x - (kX + 25.0)) < 0.01 &&
                  std::fabs(s[0].position.yaw - 45.0f) < 1e-4,
              "the client quits: the character service gets the last good position (x +25 cm, yaw 45) for the character, once");
    }
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        walk(rig, j, 1.0f, kX + 40.0, t0 + milliseconds(100));
        rig.ep.tick(t0 + seconds(30));
        CHECK(rig.ep.session_count() == 1 && e.saves().empty(), "(30 s of silence: still there, nothing saved)");
        rig.ep.tick(t0 + seconds(70));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && std::fabs(s[0].position.x - (kX + 40.0)) < 0.01, "the client times out: its last good position is saved");
        g_log.str("");
    }
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        walk(rig, a, 1.0f, kX + 60.0, t0 + milliseconds(100));
        Joined b = handshake(rig, kB, t0 + milliseconds(200));
        LoginResult r;
        r.accepted = true;
        r.character = cyb_record();
        exchange(rig, b, kB, {b.client->hello()}, t0 + milliseconds(210));
        rig.ep.post_login_result(b.session, r);
        rig.ep.tick(t0 + milliseconds(220));
        rig.ep.tick(t0 + seconds(3));
        const auto s = e.saves();
        CHECK(s.size() == 1 && s[0].id == kCyb && std::fabs(s[0].position.x - (kX + 60.0)) < 0.01, "kicked for a login elsewhere: the first session's last good position is saved");
    }
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined a = handshake(rig, kA, t0);
        join_world(rig, a, kA, cyb_record(), t0);
        walk(rig, a, 1.0f, kX + 70.0, t0 + milliseconds(100));
        rig.ep.kick_all(login_text::kShuttingDown, t0 + milliseconds(200));
        rig.ep.finish_players("worldd is stopping");
        const auto s = e.saves();
        CHECK(s.size() == 1 && std::fabs(s[0].position.x - (kX + 70.0)) < 0.01, "worldd stops: finish_players saves the open session's position");
        rig.ep.tick(t0 + seconds(5));
        CHECK(rig.ep.session_count() == 0 && e.saves().size() == 1, "and the session's removal afterwards saves nothing again");
    }
}

static void check_endpoint_periodic_and_retry() {
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        walk(rig, j, 1.0f, kX + 80.0, t0 + milliseconds(100));
        size_t saves_at_30 = 0;
        for (int sec = 1; sec <= 65; ++sec) {
            exchange(rig, j, kA, {}, t0 + seconds(sec));
            if (sec == 30) saves_at_30 = e.api.saved_count() + e.saver->queued();
        }
        e.saver->flush(seconds(3));
        const auto s = e.api.saved();
        CHECK(saves_at_30 == 0 && s.size() == 1 && std::fabs(s[0].position.x - (kX + 80.0)) < 0.01,
              "a player who moved and stays connected is saved once at about 60 s, not before, and not again while still");
    }
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + milliseconds(200));
        rig.ep.tick(t0 + milliseconds(250));
        CHECK(rig.ep.session_count() == 0 && e.saves().empty(), "a client that joins and quits without moving saves nothing (its record is left as it was)");
    }
    {
        Endpoints e;
        Rig& rig = *e.rig;
        Joined j = handshake(rig, kA, t0);
        join_world(rig, j, kA, cyb_record(), t0);
        walk(rig, j, 1.0f, kX + 80.0, t0 + milliseconds(100));
        e.api.script = {svc::CallStatus::Unavailable, svc::CallStatus::Unavailable, svc::CallStatus::Unavailable};
        for (int sec = 1; sec <= 65; ++sec) exchange(rig, j, kA, {}, t0 + seconds(sec));
        e.saver->flush(seconds(3));
        CHECK(e.api.calls() == 3 && e.api.saved_count() == 0 && e.saver->failed() == 1,
              "(the 60 s save: three Unavailable answers, given up on; the service holds nothing)");
        exchange(rig, j, kA, {j.client->close_channel()}, t0 + seconds(66));
        rig.ep.tick(t0 + seconds(67));
        const auto s = e.saves();
        CHECK(rig.ep.session_count() == 0 && s.size() == 1 && std::fabs(s[0].position.x - (kX + 80.0)) < 0.01,
              "the player quits standing still: the end of the session hands the last good position over again, and the service has it");
    }
}

static void check_endpoint_duplicate_login() {
    Endpoints e;
    Rig& rig = *e.rig;
    Joined a = handshake(rig, kA, t0);
    join_world(rig, a, kA, cyb_record(), t0);
    walk(rig, a, 1.0f, kX + 60.0, t0 + milliseconds(100));
    walk(rig, a, 1.017f, kX + 70.0, t0 + milliseconds(120));
    Joined b = handshake(rig, kB, t0 + milliseconds(200));
    join_world(rig, b, kB, cyb_record(), t0 + milliseconds(210));
    const PlayerSession* pb = rig.ep.player_at(kB);
    CHECK(pb && near(pb->movement().good().x, kX + 70.0) && near(pb->movement().good().y, kY),
          "the new session starts at A's last good position (x +70 cm), not at the record's spawn point");
    CHECK(e.saves().size() == 1 && near(e.saves()[0].position.x, kX + 70.0), "and A's position was handed to the saver when B took over");
    rig.ep.tick(t0 + seconds(3));
    const auto s = e.saves();
    CHECK(rig.ep.session_count() == 1 && s.size() == 1 && near(s[0].position.x, kX + 70.0),
          "A's removal saves nothing more, and B (which has not moved) saves nothing: the character service ends with A's position");
    walk(rig, b, 1.0f, kX + 90.0, t0 + seconds(4));
    exchange(rig, b, kB, {b.client->close_channel()}, t0 + seconds(5));
    rig.ep.tick(t0 + seconds(6));
    const auto s2 = e.saves();
    CHECK(s2.size() == 2 && near(s2[0].position.x, kX + 70.0) && near(s2[1].position.x, kX + 90.0), "B's own position lands after A's: 70, then 90");
}

static void test_endpoint_saves() {
    std::printf("through the endpoint\n");
    check_endpoint_quit_timeout_kick_stop();
    check_endpoint_periodic_and_retry();
    check_endpoint_duplicate_login();
}

struct OrderingService : svc::CharacterApi {
    std::mutex mu;
    std::map<std::string, double> last_ok;
    size_t calls = 0, ok = 0, regressions = 0, unavailable = 0;
    std::mt19937_64 rng{99};
    svc::CharacterResult get_character(const std::string&) override { return {}; }
    svc::CallStatus save_position(const std::string& id, const svc::Position& p) override {
        std::lock_guard<std::mutex> l(mu);
        ++calls;
        for (const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50); std::chrono::steady_clock::now() < until;) {
        }
        if (rng() % 10 < 3) {
            ++unavailable;
            return svc::CallStatus::Unavailable;
        }
        const auto it = last_ok.find(id);
        if (it != last_ok.end() && p.x < it->second) ++regressions;
        last_ok[id] = p.x;
        ++ok;
        return svc::CallStatus::Ok;
    }
};

static void test_saver_ordering() {
    std::printf("the saver under load\n");
    OrderingService api;
    CharacterSaver::Config cfg;
    cfg.retry_delay = milliseconds(1);
    cfg.max_attempts = 12;
    cfg.destructor_wait = seconds(10);
    constexpr int kSaves = 1000000, kCharacters = 40;
    std::map<std::string, double> submitted_last;
    size_t replaced = 0, dropped = 0, failed = 0;
    const auto begin = std::chrono::steady_clock::now();
    {
        CharacterSaver saver(api, cfg);
        std::thread producer([&] {
            for (int i = 1; i <= kSaves; ++i) {
                const std::string id = "c" + std::to_string(i % kCharacters);
                saver.save(id, pos(i));
                submitted_last[id] = i;
                if ((i & 31) == 0) std::this_thread::yield();
            }
        });
        producer.join();
        saver.flush(seconds(20));
        replaced = saver.replaced();
        dropped = saver.dropped();
        failed = saver.failed();
    }
    const auto took = std::chrono::steady_clock::now() - begin;
    size_t last_is_final = 0;
    for (const auto& [id, x] : submitted_last) last_is_final += (api.last_ok.count(id) && api.last_ok[id] == x);
    std::printf("    %d saves for %d characters in %lld ms: %zu service calls, %zu Ok, %zu Unavailable, replaced %zu, dropped %zu, failed %zu\n",
                kSaves, kCharacters, static_cast<long long>(std::chrono::duration_cast<milliseconds>(took).count()),
                api.calls, api.ok, api.unavailable, replaced, dropped, failed);
    CHECK(api.calls > 100 && api.unavailable > 20 && api.regressions == 0,
          "30% of the calls are answered Unavailable and retried: the service never sees an older position after a newer one");
    CHECK(dropped == 0 && failed == 0 && last_is_final == static_cast<size_t>(kCharacters),
          "and every character ends with the last position submitted (latest wins; nothing dropped or given up)");
}

void run_movement_saver() {
    test_saver();
    test_endpoint_saves();
    test_saver_ordering();
}
