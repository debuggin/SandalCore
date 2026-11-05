// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "capture_packets.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/wire/ue5_primitives.h"
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

using aoc::worldd::Address;
using aoc::worldd::Clock;
using aoc::worldd::CookieSigner;
using aoc::worldd::Endpoint;
using aoc::worldd::EndpointConfig;
using aoc::worldd::LoginQueue;
using aoc::worldd::LoginRequest;
using aoc::worldd::LoginResult;
namespace login_text = aoc::worldd::login_text;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace wire = aoc::protocol::wire;
using std::chrono::milliseconds;
using std::chrono::seconds;

inline std::ostringstream g_log;
inline int g_pass = 0;
inline int g_fail = 0;
#define CHECK(cond, msg)                                       \
    do {                                                       \
        if (cond) {                                            \
            std::printf("  [ok ] %s\n", msg);                  \
            g_pass++;                                          \
        } else {                                               \
            std::printf("  [FAIL] %s\n", msg);                 \
            g_fail++;                                          \
        }                                                      \
    } while (0)

inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kTicket = "9ddc8eac805cc2fc3e75df52b74613c8";
inline const Clock::time_point t0{seconds(5000)};
inline const Address kA{0x7F000001u, 50510};
inline const Address kB{0x7F000001u, 50511};
inline const Address kC{0x7F000001u, 50512};

inline uint8_t g_counter = 1;
inline void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = g_counter++;
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

    explicit Rig(EndpointConfig cfg = {})
        : ep(
              std::move(cfg), signer, logins,
              [this](const Address& a, const std::vector<uint8_t>& p) { sent.emplace_back(a, p); },
              counting_random, t0) {}

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
    std::optional<conn::HandshakePacket> challenge;
    std::optional<conn::HandshakePacket> ack;
    std::vector<uint8_t> response;
    std::optional<fake::Client> client;
    uint64_t session = 0;
};

inline std::optional<conn::HandshakePacket> parse(const std::vector<uint8_t>& b) {
    return conn::parse_handshake(b.data(), b.size());
}

inline std::vector<uint8_t> initial_with(uint8_t ticket_mask, uint8_t character_mask) {
    const auto bytes = capture::bytes(capture::kInitial);
    auto p = conn::parse_handshake(bytes.data(), bytes.size());
    p->ext->bytes[22] ^= character_mask;
    p->ext->bytes[54] ^= ticket_mask;
    return conn::build_handshake(*p);
}

inline Joined handshake(Rig& rig, const Address& addr = kA, Clock::time_point now = t0,
                        std::vector<uint8_t> initial = capture::bytes(capture::kInitial)) {
    Joined j;
    rig.take(addr);
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1) return j;
    j.challenge = parse(out[0]);
    if (!j.challenge) return j;
    conn::HandshakePacket resp = *j.challenge;
    resp.type = conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    j.response = conn::build_handshake(resp);
    rig.ep.on_datagram(addr, j.response.data(), j.response.size(), now);
    out = rig.take(addr);
    if (out.size() != 1) return j;
    j.ack = parse(out[0]);
    const auto seqs = conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server, seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}

inline std::vector<fake::Received> drain(Rig& rig, Joined& j, const Address& addr = kA) {
    std::vector<fake::Received> got;
    for (const auto& p : rig.take(addr)) got.push_back(j.client->receive(p));
    return got;
}

inline std::vector<fake::Received> exchange(Rig& rig, Joined& j, const Address& addr,
                                            std::vector<fake::Bunch> bunches, Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    return drain(rig, j, addr);
}

inline bool has(const std::vector<fake::Received>& got, conn::Nmt type, bool* closed = nullptr) {
    bool found = false;
    for (const auto& r : got) {
        for (const auto& m : r.messages) found = found || m.type == type;
        if (closed && r.closed) *closed = true;
    }
    return found;
}

inline bool any_valid(const std::vector<fake::Received>& got) {
    for (const auto& r : got)
        if (r.valid) return true;
    return false;
}

inline LoginResult accepted() {
    LoginResult r;
    r.accepted = true;
    r.character.id = kCyb;
    r.character.name = "cyb";
    r.character.account_id = 3;
    return r;
}

inline void log_in(Rig& rig, Joined& j, const Address& addr, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    rig.ep.post_login_result(j.session, accepted());
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login()}, now + milliseconds(2));
}

inline void log_in(Rig& rig, Joined& j, const Address& addr, const LoginResult& res, Clock::time_point now) {
    exchange(rig, j, addr, {j.client->hello()}, now);
    rig.ep.post_login_result(j.session, res);
    rig.ep.tick(now + milliseconds(1));
    drain(rig, j, addr);
    exchange(rig, j, addr, {j.client->login()}, now + milliseconds(2));
}

void run_endpoint_tick_tests();

