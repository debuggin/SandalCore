// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "test_worldd_movement_common.h"

#include "bootstrap/constants.h"
#include "capture_blocks.h"
#include "capture_packets.h"

#include <algorithm>

namespace {

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;

uint8_t g_counter = 1;

void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
}

struct FakeCharacters : svc::CharacterApi {
    svc::CharacterResult get_character(const std::string&) override { return {}; }
    svc::CallStatus save_position(const std::string&, const svc::Position&) override {
        return svc::CallStatus::Ok;
    }
};

CharacterSaver::Config quick(milliseconds destructor_wait = milliseconds(2000)) {
    CharacterSaver::Config c;
    c.retry_delay = milliseconds(5);
    c.destructor_wait = destructor_wait;
    return c;
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

Joined handshake(Rig& rig, const Address& addr, Clock::time_point now) {
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

void drain(Rig& rig, Joined& j, const Address& addr) {
    for (const auto& p : rig.take(addr)) {
        const auto r = j.client->receive(p);
        for (const auto& d : r.delivered) j.got.push_back(d);
    }
}

void exchange(Rig& rig, Joined& j, const Address& addr, std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}

void log_in(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login(kMachine, kMachine)}, now + milliseconds(2));
}

void join_world(Rig& rig, Joined& j, const Address& addr, const svc::Character& rec, Clock::time_point now) {
    log_in(rig, j, addr, rec, now);
    exchange(rig, j, addr, {j.client->netspeed(200000), j.client->nmt(conn::Nmt::Join)}, now + milliseconds(40));
}

struct PresenceEndpoints {
    Fixture fx;
    FakeCharacters api;
    std::unique_ptr<CharacterSaver> saver;
    EndpointConfig cfg;
    std::unique_ptr<Rig> rig;
    explicit PresenceEndpoints(unsigned threads) {
        saver = std::make_unique<CharacterSaver>(api, quick());
        fx.world->set_position_sink(saver.get());
        cfg.players = fx.world.get();
        cfg.view_threads = threads;
        rig = std::make_unique<Rig>(cfg);
    }
    ~PresenceEndpoints() {
        rig.reset();
        saver.reset();
    }
};

svc::Character presence_record(const std::string& id, const std::string& name, int account) {
    svc::Character c = cyb_record();
    c.id = id;
    c.name = name;
    c.account_id = account;
    return c;
}

std::string presence_id(int n) {
    std::string id(32, '0');
    id[31] = "0123456789abcdef"[n & 15];
    id[30] = "0123456789abcdef"[(n >> 4) & 15];
    return id;
}

bool presence_has_text(const std::vector<uint8_t>& packet, const std::string& needle) {
    if (needle.empty() || packet.size() < needle.size()) return false;
    std::vector<uint8_t> shifted(packet.size());
    for (int shift = 0; shift < 8; ++shift) {
        for (size_t i = 0; i < packet.size(); ++i) {
            const unsigned low = static_cast<unsigned>(packet[i]) >> shift;
            const unsigned high = (shift == 0 || i + 1 >= packet.size())
                                      ? 0u
                                      : (static_cast<unsigned>(packet[i + 1]) << (8 - shift));
            shifted[i] = static_cast<uint8_t>((low | high) & 0xFFu);
        }
        if (std::search(shifted.begin(), shifted.end(), needle.begin(), needle.end()) != shifted.end()) {
            return true;
        }
    }
    return false;
}

bool presence_has_name(const std::vector<uint8_t>& packet, const std::string& text) {
    std::string wide;
    for (const char ch : text) {
        wide += ch;
        wide += '\0';
    }
    return presence_has_text(packet, text) || presence_has_text(packet, wide);
}

struct PresenceClient {
    Address addr;
    Joined j;
    std::string name;
    std::vector<fake::Bunch> out;
    float ts = 0;
    double x = kX, y = kY, z = kZ;
    std::vector<std::string> needles;
    size_t hits = 0;
    size_t packets = 0;
    explicit PresenceClient(uint16_t port, std::string who)
        : addr{0x7F000001u, port}, name(std::move(who)) {}
};

void presence_drain(Rig& rig, PresenceClient& c) {
    for (const auto& p : rig.take(c.addr)) {
        ++c.packets;
        for (const std::string& n : c.needles) {
            if (presence_has_name(p, n)) ++c.hits;
        }
        const auto r = c.j.client->receive(p);
        for (const auto& d : r.delivered) c.j.got.push_back(d);
    }
}

void presence_step(Rig& rig, const std::vector<PresenceClient*>& all, Clock::time_point now) {
    for (PresenceClient* c : all) {
        const auto pkt = c->j.client->packet(std::move(c->out));
        c->out.clear();
        rig.ep.on_datagram(c->addr, pkt.data(), pkt.size(), now);
    }
    rig.ep.tick(now);
    for (PresenceClient* c : all) presence_drain(rig, *c);
}

void presence_idle(
    Rig& rig,
    const std::vector<PresenceClient*>& all,
    Clock::time_point& now,
    int steps,
    milliseconds gap = milliseconds(40)) {
    for (int i = 0; i < steps; ++i) {
        now += gap;
        presence_step(rig, all, now);
    }
}

void presence_join(
    Rig& rig,
    std::vector<PresenceClient*>& all,
    PresenceClient& c,
    const svc::Character& rec,
    Clock::time_point& now,
    const std::string& login_name = kMachine) {
    c.j = handshake(rig, c.addr, now);
    all.push_back(&c);
    c.out.push_back(c.j.client->hello());
    presence_step(rig, all, now);
    LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(c.j.session, r);
    now += milliseconds(1);
    presence_step(rig, all, now);
    c.out.push_back(c.j.client->login(login_name, login_name));
    now += milliseconds(2);
    presence_step(rig, all, now);
    c.out.push_back(c.j.client->netspeed(200000));
    c.out.push_back(c.j.client->nmt(conn::Nmt::Join));
    now += milliseconds(38);
    presence_step(rig, all, now);
    BunchWriter ready;
    {
        const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
        ready.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    }
    c.out.push_back(c.j.client->actor(5, ready, true));
    now += milliseconds(20);
    presence_step(rig, all, now);
}

int presence_phase_in(
    Rig& rig,
    const std::vector<PresenceClient*>& all,
    PresenceClient& c,
    Clock::time_point& now,
    int limit = 200) {
    for (int i = 0; i < limit; ++i) {
        const PlayerSession* s = rig.ep.player_at(c.addr);
        if (s && s->join_pending() == 0 && s->replies_pending() == 0) return i;
        presence_idle(rig, all, now, 1);
    }
    return -1;
}

void presence_move(PresenceClient& c, float dt, double dx, uint8_t mode = repl::kMoveWalking) {
    c.ts += dt;
    c.x += dx;
    c.out.push_back(c.j.client->actor(5, client_move(move_at(c.ts, c.x, c.y, c.z, 8192, mode)), false));
}

uint64_t presence_player_id(const Rig& rig, const Address& addr) {
    const PlayerSession* s = rig.ep.player_at(addr);
    return s ? static_cast<uint64_t>(s->bootstrap().setup().player_id) : 0;
}

std::vector<const world::WorldActor*> presence_actors(const world::ActorRegistry& reg) {
    std::vector<const world::WorldActor*> out;
    reg.for_each([&](const world::WorldActor& a) {
        if (a.kind == world::ActorKind::Player) out.push_back(&a);
    });
    return out;
}

const world::WorldActor* presence_pawn(const world::ActorRegistry& reg, uint64_t player_id) {
    for (const world::WorldActor* a : presence_actors(reg)) {
        if (a->owner == player_id) return a;
    }
    return nullptr;
}

struct PresenceMotion {
    double x = 0, y = 0, z = 0;
    double vx = 0, vy = 0;
    bool moving = false;
    uint64_t mode = 0;
    bool ok = false;
};

PresenceMotion presence_motion(const world::WorldActor& a) {
    PresenceMotion out;
    const repl::ObjectState& o = a.actor.obj;
    if (!o.schema) return out;
    const repl::Value* v = o.state.get(*o.schema, "ReplicatedMovement");
    const auto* m = v ? v->get_if<repl::RepMovement>() : nullptr;
    if (!m) return out;
    const repl::Vec3d at = repl::dequantize_location(o.opts, m->location);
    const repl::Vec3d vel = repl::dequantize_velocity(o.opts, m->linear_velocity);
    out.x = at.x;
    out.y = at.y;
    out.z = at.z;
    out.vx = vel.x;
    out.vy = vel.y;
    if (const repl::Value* in = o.state.get(*o.schema, "CurrentMovementInput")) {
        if (const auto* input = in->get_if<repl::PlayerMovementInput>()) out.moving = input->bits != 0;
    }
    if (const repl::Value* mode = o.state.get(*o.schema, "ReplicatedMovementMode")) {
        if (const auto* u = mode->get_if<uint64_t>()) out.mode = *u;
    }
    out.ok = true;
    return out;
}

size_t presence_count(const world::ActorRegistry& reg) { return presence_actors(reg).size(); }

}  // namespace
