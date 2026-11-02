// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "bootstrap/captured_replies.h"
#include "bootstrap/captured_setup.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/schema/generated/index.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
// C.O.A / Cyb

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(5000)};
inline const aoc::worldd::Address kA{0x7F000001u, 50510};
inline const aoc::worldd::Address kB{0x7F000001u, 50511};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const std::array<uint32_t, 4> kChannels = {3, 4, 5, 6};

inline uint8_t g_counter = 1;
inline void counting_random(uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        out[i] = g_counter++;
    }
}

struct Fixture {
    std::filesystem::path dir;
    std::optional<aoc::worldd::content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;

    explicit Fixture(aoc::worldd::bootstrap::GameStateMode mode = aoc::worldd::bootstrap::GameStateMode::Tier2,
                     bool with_content = true, const std::string& real_db = "") {
        std::string problem = "no such file";
        if (with_content) {
            std::filesystem::path db = real_db;
            if (real_db.empty()) {
                dir = content_fixture::make_temp_dir("worldd_session_");
                db = dir / "content.db";
                if (!content_fixture::make_db(db)) {
                    std::exit(2);
                }
            }
            std::string error;
            content = aoc::worldd::content::ContentDb::open(db.string(), error);
            if (!content) {
                std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
                std::exit(2);
            }
            problem.clear();
        }
        world = std::make_unique<aoc::worldd::PlayerWorld>(content ? &*content : nullptr, problem, mode, kServerId, guids);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        if (!dir.empty()) {
            std::filesystem::remove_all(dir, ec);
        }
    }
};

inline aoc::worldd::svc::Character cyb_record() {
    aoc::worldd::svc::Character c;
    c.id = kCyb;
    c.account_id = 3;
    c.name = "cyb";
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    return c;
}

inline aoc::worldd::svc::Character second_record() {
    aoc::worldd::svc::Character c = cyb_record();
    c.id = "11112222333344445555666677778888";
    c.name = "dee";
    return c;
}

inline aoc::worldd::bootstrap::LoginIdentity identity() {
    return {kMachine, kMachine};
}

inline size_t count_lines(const std::string& log, const char* text) {
    size_t n = 0;
    for (size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) {
        ++n;
    }
    return n;
}

inline double world_time_at(aoc::worldd::Clock::time_point t) {
    return std::chrono::duration<double>(t - t0).count();
}

inline std::unique_ptr<aoc::worldd::bootstrap::PlayerBootstrap> expected_bootstrap(
    const aoc::worldd::content::ContentDb& content, const aoc::worldd::svc::Character& rec, double world_time,
    aoc::worldd::bootstrap::GameStateMode mode, aoc::protocol::repl::ActionbarPayload* bar_out = nullptr) {
    aoc::protocol::ServerGuidAllocator guids(kServerId, kRandomizer);
    aoc::worldd::PlayerWorld w(&content, "", mode, kServerId, guids);
    const aoc::worldd::bootstrap::SessionValues sv = w.new_session(world_time);
    auto built = aoc::worldd::bootstrap::build_player_setup(rec, content, identity(), sv, w.stat_context());
    if (!built.setup) {
        std::printf("  [FAIL] the expected setup does not build: %s\n", built.error.c_str());
        std::exit(2);
    }
    if (bar_out) {
        bar_out->slots = built.action_bar.wire_slots();
    }
    auto pb = std::make_unique<aoc::worldd::bootstrap::PlayerBootstrap>(std::move(*built.setup), mode);
    if (!pb->ok()) {
        std::printf("  [FAIL] the expected bootstrap does not build: %s\n", pb->error().c_str());
        std::exit(2);
    }
    return pb;
}

// [Cyb - C | O | A]

inline bool same_bits(const aoc::protocol::emit::BunchWriter& a, const aoc::protocol::emit::BunchWriter& b) {
    return a.bit_pos() == b.bit_pos() && capture_blocks::same_bit_range(a.data(), b.data(), a.bit_pos(), nullptr);
}

inline std::string compare_one_send(const fake::Delivered& d, const aoc::worldd::bootstrap::Send& s, size_t idx,
                                    const std::array<uint32_t, 4>& channels) {
    const std::string at = "chain " + std::to_string(idx) + " (" + s.what + "): ";
    if (d.channel != channels[static_cast<size_t>(s.role)]) {
        return at + "channel " + std::to_string(d.channel);
    }
    if (d.open != s.opens_channel) {
        return at + "open flag";
    }
    if (d.reliable != s.reliable()) {
        return at + "reliability";
    }
    if (d.exports.size() != s.emission.exports.size()) {
        return at + std::to_string(d.exports.size()) + " export bunches";
    }
    for (size_t e = 0; e < d.exports.size(); ++e) {
        if (!same_bits(d.exports[e], s.emission.exports[e])) {
            return at + "export bunch " + std::to_string(e);
        }
    }
    if (!same_bits(d.content, s.emission.payload)) {
        return at + "content";
    }
    return "";
}

inline std::string compare_sends(const std::vector<fake::Delivered>& got, size_t first,
                                 const std::vector<aoc::worldd::bootstrap::Send>& want,
                                 const std::array<uint32_t, 4>& channels) {
    if (got.size() < first + want.size()) {
        return "only " + std::to_string(got.size() - std::min(first, got.size())) + " chains arrived";
    }
    for (size_t i = 0; i < want.size(); ++i) {
        const std::string diff = compare_one_send(got[first + i], want[i], i, channels);
        if (!diff.empty()) {
            return diff;
        }
    }
    return "";
}

inline const aoc::protocol::schema::ClassSchema& class_named(const char* name) {
    const aoc::protocol::schema::ClassSchema* c = aoc::protocol::schema::gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

inline std::vector<std::string> rpc_names(const fake::Delivered& d, const aoc::protocol::schema::ClassSchema& cls) {
    std::vector<std::string> out;
    const auto blocks = aoc::protocol::repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) {
        return {"<unreadable>"};
    }
    for (const aoc::protocol::repl::BlockView& v : *blocks) {
        size_t p = v.payload_pos;
        const auto calls = aoc::protocol::repl::read_rpc_fields(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
        if (!calls) {
            out.push_back("<not an rpc>");
            continue;
        }
        for (const aoc::protocol::repl::RpcCall& c : *calls) {
            out.push_back(c.rpc->name);
        }
    }
    return out;
}

struct NewActor {
    aoc::protocol::emit::FIntrepidNetworkGUID actor;
    aoc::protocol::emit::FIntrepidNetworkGUID archetype;
    size_t end = 0;
    bool ok = false;
};

inline aoc::protocol::emit::FIntrepidNetworkGUID read_actor_guid(aoc::protocol::wire::PacketReader& r) {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    const uint64_t lo = r.read_uint32();
    const uint64_t hi = r.read_uint32();
    g.ObjectId = lo | (hi << 32);
    g.ServerId = r.read_uint32();
    g.Randomizer = r.read_uint32();
    return g;
}

inline bool skip_quantized_vector(aoc::protocol::wire::PacketReader& r) {
    if (!r.read_bit()) {
        return true;
    }
    if (!r.read_bit()) {
        return false;
    }
    return aoc::protocol::repl::read_quantized_vector(r).has_value();
}

inline NewActor read_new_actor(const aoc::protocol::emit::BunchWriter& w, bool player_controller) {
    NewActor n;
    aoc::protocol::wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    n.actor = read_actor_guid(r);
    n.archetype = read_actor_guid(r);
    read_actor_guid(r);
    if (!skip_quantized_vector(r)) {
        return n;
    }
    if (r.read_bit() && !aoc::protocol::repl::read_struct("FRotator", r)) {
        return n;
    }
    if (!skip_quantized_vector(r) || !skip_quantized_vector(r)) {
        return n;
    }
    if (player_controller) {
        r.read_uint8();
        r.read_uint32();
    }
    n.ok = !r.overflowed();
    n.end = r.pos();
    return n;
}
// ~ C:Y:B - C O A ~

inline std::optional<aoc::protocol::repl::RepState> block_of(const fake::Delivered& d, size_t start, bool actor,
                                                             const aoc::protocol::emit::FIntrepidNetworkGUID& object,
                                                             const aoc::protocol::schema::ClassSchema& cls) {
    const auto blocks = aoc::protocol::repl::read_content_blocks(d.content.data(), d.content.bit_pos(), start);
    if (!blocks) {
        return std::nullopt;
    }
    for (const aoc::protocol::repl::BlockView& v : *blocks) {
        if (!v.header.has_rep_layout) {
            continue;
        }
        const bool match = actor ? v.header.is_actor : (!v.header.is_actor && v.header.object == object);
        if (!match) {
            continue;
        }
        size_t p = v.payload_pos;
        return aoc::protocol::repl::read_rep_block(cls, d.content.data(), v.payload_pos + v.payload_bits, p);
    }
    return std::nullopt;
}

inline aoc::protocol::emit::FIntrepidNetworkGUID guid(uint64_t id) {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = kServerId;
    g.Randomizer = kRandomizer;
    return g;
}

inline aoc::protocol::emit::BunchWriter client_rpc(const aoc::protocol::schema::ClassSchema& cls, const char* name,
                                                   const aoc::protocol::repl::RpcParams& params = {}) {
    const aoc::protocol::schema::RpcSchema* rpc = cls.rpc(name);
    aoc::protocol::emit::BunchWriter out;
    if (!rpc) {
        std::printf("  [FAIL] no RPC %s\n", name);
        return out;
    }
    aoc::protocol::emit::BunchWriter field;
    bool failed = false;
    if (aoc::protocol::repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    aoc::protocol::repl::BlockHeader header;
    aoc::protocol::repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

inline aoc::protocol::emit::BunchWriter hex_bits(const capture_blocks::Bits& b) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    aoc::protocol::emit::BunchWriter w;
    w.write_bit_range(bytes.data(), 0, b.bits);
    return w;
}

inline aoc::protocol::emit::BunchWriter hex_bits(const capture_bootstrap::Bits& b) {
    const auto bytes = capture_blocks::bytes_of(b.hex);
    aoc::protocol::emit::BunchWriter w;
    w.write_bit_range(bytes.data(), 0, b.bits);
    return w;
}

inline std::optional<float> legacy_time_stamp(const aoc::protocol::emit::BunchWriter& bunch) {
    aoc::protocol::wire::FServerMoveNoBase move;
    if (!aoc::protocol::wire::parse_server_move_no_base(bunch.data(), bunch.byte_size(), 0, bunch.bit_pos(), move)) {
        return std::nullopt;
    }
    return move.timestamp;
}

inline std::optional<float> response_time_stamp(const fake::Delivered& d) {
    const auto blocks = aoc::protocol::repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) {
        return std::nullopt;
    }
    const aoc::protocol::schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    size_t pos = (*blocks)[0].payload_pos;
    const auto h = aoc::protocol::repl::read_field_header(pawn, d.content.data(), pos + (*blocks)[0].payload_bits, pos);
    if (!h || !pawn.field(h->field) || std::string(pawn.field(h->field)->name) != "ClientMoveResponsePacked") {
        return std::nullopt;
    }
    aoc::protocol::wire::PacketReader r(d.content.data(), d.content.byte_size(), pos + h->payload_bits);
    r.set_pos(pos);
    const uint64_t header = r.read_bits(10);
    const uint32_t bits = static_cast<uint32_t>(r.read_bits(32));
    if (r.overflowed() || header != 645 || h->payload_bits != 42) {
        return std::nullopt;
    }
    return std::bit_cast<float>(bits);
}

struct FakeLogins : aoc::worldd::LoginQueue {
    std::vector<std::pair<uint64_t, aoc::worldd::LoginRequest>> submitted;
    void submit(uint64_t id, aoc::worldd::LoginRequest r) override {
        submitted.emplace_back(id, std::move(r));
    }
};

struct Rig {
    aoc::worldd::CookieSigner signer{counting_random};
    FakeLogins logins;
    std::vector<std::pair<aoc::worldd::Address, std::vector<uint8_t>>> sent;
    aoc::worldd::Endpoint ep;

    explicit Rig(aoc::worldd::EndpointConfig cfg)
        : ep(
              std::move(cfg), signer, logins,
              [this](const aoc::worldd::Address& a, const std::vector<uint8_t>& p) { sent.emplace_back(a, p); },
              counting_random, t0) {}

    std::vector<std::vector<uint8_t>> take(const aoc::worldd::Address& a) {
        std::vector<std::vector<uint8_t>> out;
        std::vector<std::pair<aoc::worldd::Address, std::vector<uint8_t>>> rest;
        for (auto& [to, p] : sent) {
            if (to == a) {
                out.push_back(p);
            } else {
                rest.emplace_back(to, p);
            }
        }
        sent.swap(rest);
        return out;
    }
};

struct Joined {
    aoc::worldd::Address addr = kA;
    std::optional<aoc::protocol::conn::HandshakePacket> challenge;
    std::optional<fake::Client> client;
    uint64_t session = 0;
    std::vector<fake::Delivered> got;
};
// [C | O | A + C:Y:B]

inline Joined handshake(Rig& rig, const aoc::worldd::Address& addr, aoc::worldd::Clock::time_point now) {
    Joined j;
    j.addr = addr;
    rig.take(addr);
    const auto initial = capture::bytes(capture::kInitial);
    rig.ep.on_datagram(addr, initial.data(), initial.size(), now);
    auto out = rig.take(addr);
    if (out.size() != 1) {
        return j;
    }
    j.challenge = aoc::protocol::conn::parse_handshake(out[0].data(), out[0].size());
    aoc::protocol::conn::HandshakePacket resp = *j.challenge;
    resp.type = aoc::protocol::conn::HandshakeType::Response;
    resp.sent_count = 2;
    resp.padding.assign(11, 0x5A);
    const auto bytes = aoc::protocol::conn::build_handshake(resp);
    rig.ep.on_datagram(addr, bytes.data(), bytes.size(), now);
    out = rig.take(addr);
    if (out.size() != 1) {
        return j;
    }
    const auto seqs = aoc::protocol::conn::initial_sequences(j.challenge->cookie);
    j.client.emplace(aoc::protocol::conn::outer_header_of(*j.challenge), j.challenge->ext->custom_field(), seqs.server,
                     seqs.client);
    j.session = rig.logins.submitted.empty() ? 0 : rig.logins.submitted.back().first;
    return j;
}

inline std::vector<fake::Received> drain(Rig& rig, Joined& j, const aoc::worldd::Address& addr) {
    std::vector<fake::Received> got;
    for (const auto& p : rig.take(addr)) {
        got.push_back(j.client->receive(p));
        for (const auto& d : got.back().delivered) {
            j.got.push_back(d);
        }
    }
    return got;
}

inline std::vector<fake::Received> exchange(Rig& rig, Joined& j, const aoc::worldd::Address& addr,
                                            std::vector<fake::Bunch> bunches, aoc::worldd::Clock::time_point now) {
    const auto pkt = j.client->packet(std::move(bunches));
    rig.ep.on_datagram(addr, pkt.data(), pkt.size(), now);
    rig.ep.tick(now);
    return drain(rig, j, addr);
}

inline const aoc::protocol::conn::ControlMessage* find_message(const std::vector<fake::Received>& got,
                                                               aoc::protocol::conn::Nmt type) {
    for (const auto& r : got) {
        for (const auto& m : r.messages) {
            if (m.type == type) {
                return &m;
            }
        }
    }
    return nullptr;
}

inline bool closed_by_server(const std::vector<fake::Received>& got) {
    for (const auto& r : got) {
        if (r.closed) {
            return true;
        }
    }
    return false;
}

inline aoc::worldd::LoginResult cyb_login() {
    aoc::worldd::LoginResult r;
    r.accepted = true;
    r.character = cyb_record();
    return r;
}

inline void log_in_as(Rig& rig, Joined& j, const aoc::worldd::svc::Character& rec, aoc::worldd::Clock::time_point now,
                      const std::string& machine) {
    exchange(rig, j, j.addr, {j.client->hello()}, now);
    aoc::worldd::LoginResult r;
    r.accepted = true;
    r.character = rec;
    rig.ep.post_login_result(j.session, r);
    rig.ep.tick(now + std::chrono::milliseconds(1));
    drain(rig, j, j.addr);
    exchange(rig, j, j.addr, {j.client->login(machine, machine)}, now + std::chrono::milliseconds(2));
}

inline void log_in(Rig& rig, Joined& j, const aoc::worldd::Address& addr, const aoc::worldd::svc::Character& rec,
                   aoc::worldd::Clock::time_point now) {
    j.addr = addr;
    log_in_as(rig, j, rec, now, kMachine);
}

inline void join_now(Rig& rig, Joined& j, const aoc::worldd::Address& addr, aoc::worldd::Clock::time_point now) {
    const auto p = j.client->packet({j.client->netspeed(200000), j.client->nmt(aoc::protocol::conn::Nmt::Join)});
    rig.ep.on_datagram(addr, p.data(), p.size(), now);
    rig.ep.tick(now);
    drain(rig, j, addr);
}

// ~ C_Y_B - CoA ~

inline const aoc::protocol::conn::OuterHeader kOuter{0x500c7696u, 0, 5};
inline const aoc::protocol::conn::CustomField kCf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
inline constexpr uint16_t kServerSeq = 14265;
inline constexpr uint16_t kClientSeq = 15870;

struct Harness {
    Fixture fx;
    aoc::worldd::Connection conn{aoc::worldd::ConnectionConfig{kOuter, kCf, kServerSeq, kClientSeq}, t0};
    fake::Client client{kOuter, kCf, kServerSeq, kClientSeq};
    std::unique_ptr<aoc::worldd::PlayerSession> s;
    aoc::worldd::Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::vector<std::vector<uint8_t>> held;
    bool client_receives = true;
    size_t packets = 0;
    size_t bytes = 0;
    std::string error;

    explicit Harness(aoc::worldd::bootstrap::GameStateMode mode = aoc::worldd::bootstrap::GameStateMode::Tier2,
                     const std::string& real_db = "")
        : fx(mode, true, real_db) {}

    bool join(const aoc::worldd::svc::Character& rec = cyb_record()) {
        s = aoc::worldd::PlayerSession::join(conn, *fx.world, rec, identity(), world_time_at(t), error);
        return s != nullptr;
    }
    void take(const fake::Received& r) {
        for (const auto& d : r.delivered) {
            got.push_back(d);
        }
    }
    void flush() {
        for (auto& pkt : conn.flush(t)) {
            ++packets;
            bytes += pkt.size();
            if (client_receives) {
                take(client.receive(pkt));
            } else {
                held.push_back(std::move(pkt));
            }
        }
    }
    void give_held() {
        for (const auto& pkt : held) {
            take(client.receive(pkt));
        }
        held.clear();
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        const std::vector<aoc::worldd::InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const aoc::worldd::InBunch& b : in) {
            s->on_bunch(conn, b, t);
        }
    }
    size_t client_repeats(uint32_t channel, const aoc::protocol::emit::BunchWriter& one, size_t n,
                          std::chrono::milliseconds apart = std::chrono::milliseconds(1)) {
        const size_t fit = std::max<size_t>(1, (aoc::worldd::Connection::kMaxBunchBits - 200) / one.bit_pos());
        size_t sent_pkts = 0;
        for (size_t left = n; left > 0; ++sent_pkts) {
            const size_t count = std::min(fit, left);
            aoc::protocol::emit::BunchWriter many;
            for (size_t i = 0; i < count; ++i) {
                many.write_bit_range(one.data(), 0, one.bit_pos());
            }
            t += apart;
            client_sends({client.actor(channel, many, true)});
            s->tick(conn);
            flush();
            left -= count;
        }
        return sent_pkts;
    }
    void step() {
        t += std::chrono::milliseconds(20);
        s->tick(conn);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = got.size();
            step();
            const bool idle = got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0;
            quiet = idle ? quiet + 1 : 0;
        }
    }
    std::vector<const fake::Delivered*> on_channel(uint32_t channel, size_t from = 0) const {
        std::vector<const fake::Delivered*> out;
        for (size_t i = from; i < got.size(); ++i) {
            if (got[i].channel == channel) {
                out.push_back(&got[i]);
            }
        }
        return out;
    }
};
