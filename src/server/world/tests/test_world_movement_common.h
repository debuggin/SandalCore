// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "net/endpoint.h"
#include "protocol/schema/generated/index.h"

#include <bit>
#include <cmath>

inline std::ostringstream g_log;
inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(5000)};
inline const aoc::worldd::Address kA{0x7F000001u, 50510};
inline const aoc::worldd::Address kB{0x7F000001u, 50511};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;

inline constexpr double kX = -777664.5, kY = 616578.6875, kZ = 15952.0498046875;
inline constexpr uint16_t kSpawnYaw = 16550;

inline const aoc::protocol::schema::ClassSchema& class_named(const char* name) {
    const aoc::protocol::schema::ClassSchema* c = aoc::protocol::schema::gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

struct Fixture {
    std::filesystem::path dir;
    std::optional<aoc::worldd::content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;

    Fixture() {
        dir = content_fixture::make_temp_dir("world_movement_");
        const std::filesystem::path db = dir / "content.db";
        if (!content_fixture::make_db(db)) std::exit(2);
        std::string error;
        content = aoc::worldd::content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<aoc::worldd::PlayerWorld>(
            &*content, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId, guids);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
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

inline aoc::protocol::repl::NetworkMove move_at(
    float ts,
    double x,
    double y,
    double z,
    uint16_t yaw = kSpawnYaw,
    uint8_t mode = aoc::protocol::repl::kMoveWalking) {
    aoc::protocol::repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = aoc::protocol::repl::quantize_vector(10, 0, 0, 0);
    m.location = aoc::protocol::repl::quantize_vector(100, x, y, z);
    m.control_rotation.yaw = yaw;
    m.movement_mode = mode;
    return m;
}

inline aoc::protocol::emit::BunchWriter client_move(const aoc::protocol::repl::MovePacked& moves) {
    const aoc::protocol::schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const aoc::protocol::schema::NetField* f = cls.field("ServerMovePacked");
    aoc::protocol::emit::BunchWriter out;
    if (!f) return out;
    aoc::protocol::emit::BunchWriter params;
    if (!aoc::protocol::repl::write_move_packed(params, moves)) return out;
    aoc::protocol::emit::BunchWriter field;
    if (aoc::protocol::repl::write_field(cls, f->index, params, field) == 0) return out;
    aoc::protocol::repl::BlockHeader header;
    aoc::protocol::repl::write_content_block(out, header, field, false);
    return out;
}

inline aoc::protocol::emit::BunchWriter client_move(const aoc::protocol::repl::NetworkMove& m) {
    aoc::protocol::repl::MovePacked p;
    p.new_move = m;
    return client_move(p);
}

inline aoc::protocol::emit::BunchWriter client_moves(
    const std::vector<aoc::protocol::repl::NetworkMove>& moves) {
    const aoc::protocol::schema::ClassSchema& cls = class_named("PlayerPawn_C");
    const aoc::protocol::schema::NetField* f = cls.field("ServerMovePacked");
    aoc::protocol::emit::BunchWriter out;
    if (!f) return out;
    aoc::protocol::emit::BunchWriter field;
    for (const aoc::protocol::repl::NetworkMove& m : moves) {
        aoc::protocol::repl::MovePacked p;
        p.new_move = m;
        aoc::protocol::emit::BunchWriter params;
        if (!aoc::protocol::repl::write_move_packed(params, p) ||
            aoc::protocol::repl::write_field(cls, f->index, params, field) == 0) {
            return aoc::protocol::emit::BunchWriter();
        }
    }
    aoc::protocol::repl::BlockHeader header;
    aoc::protocol::repl::write_content_block(out, header, field, false);
    return out;
}

inline bool near(double a, double b) { return std::fabs(a - b) < 0.01; }

struct DecodedRpc {
    bool ok = false;
    std::string name;
    aoc::protocol::repl::RpcCall call;
};

inline DecodedRpc decode_rpc(
    const aoc::protocol::emit::BunchWriter& content,
    const aoc::protocol::schema::ClassSchema& cls) {
    DecodedRpc d;
    const auto blocks = aoc::protocol::repl::read_content_blocks(content.data(), content.bit_pos(), 0);
    if (!blocks || blocks->size() != 1) return d;
    size_t p = (*blocks)[0].payload_pos;
    auto calls = aoc::protocol::repl::read_rpc_fields(
        cls, content.data(), (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, p);
    if (!calls || calls->size() != 1) return d;
    d.call = std::move((*calls)[0]);
    d.name = d.call.rpc->name;
    d.ok = d.call.decoded;
    return d;
}

inline const aoc::protocol::repl::Value* param(const DecodedRpc& d, std::string_view path) {
    const aoc::protocol::schema::RepCommand* c = aoc::protocol::repl::rpc_command(*d.call.rpc, path);
    if (!c) return nullptr;
    const auto it = d.call.params.values.find(c->handle);
    return it == d.call.params.values.end() ? nullptr : &it->second;
}

struct RecordingSink : aoc::worldd::PositionSink {
    struct Saved {
        std::string id;
        aoc::worldd::svc::Position position;
    };
    std::vector<Saved> saves;
    void save(const std::string& id, const aoc::worldd::svc::Position& p) override { saves.push_back({id, p}); }
};

inline aoc::worldd::bootstrap::LoginIdentity identity() { return {kMachine, kMachine}; }

inline double world_time_at(aoc::worldd::Clock::time_point t) {
    return std::chrono::duration<double>(t - t0).count();
}

struct Harness {
    Fixture fx;
    RecordingSink sink;
    aoc::worldd::Connection conn{
        aoc::worldd::ConnectionConfig{
            aoc::protocol::conn::OuterHeader{0x500c7696u, 0, 5},
            aoc::protocol::conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d},
            14265,
            15870},
        t0};
    fake::Client client{
        aoc::protocol::conn::OuterHeader{0x500c7696u, 0, 5},
        aoc::protocol::conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d},
        14265,
        15870};
    std::unique_ptr<aoc::worldd::PlayerSession> s;
    aoc::worldd::Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::string error;
    uint32_t pawn_ch = 0;

    explicit Harness(const aoc::worldd::MovementConfig& mc = {}, bool with_sink = true) {
        fx.world->set_movement(mc);
        if (with_sink) fx.world->set_position_sink(&sink);
        s = aoc::worldd::PlayerSession::join(conn, *fx.world, cyb_record(), identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(aoc::worldd::bootstrap::Role::Pawn);
        settle();
    }
    void flush() {
        for (auto& pkt : conn.flush(t)) {
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) got.push_back(d);
        }
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        const std::vector<aoc::worldd::InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const aoc::worldd::InBunch& b : in) s->on_bunch(conn, b, t);
        s->end_of_datagram(conn);
    }
    void step(std::chrono::milliseconds d = std::chrono::milliseconds(20)) {
        t += d;
        s->tick(conn, t);
        flush();
        s->take_acks(conn);
        client_sends({});
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 3 && i < 400; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    std::vector<const fake::Delivered*> send_move(const aoc::protocol::emit::BunchWriter& bunch) {
        const size_t from = got.size();
        client_sends({client.actor(pawn_ch, bunch, false)});
        flush();
        std::vector<const fake::Delivered*> out;
        for (size_t i = from; i < got.size(); ++i) {
            if (got[i].channel == pawn_ch) out.push_back(&got[i]);
        }
        return out;
    }
    std::vector<const fake::Delivered*> send_move(const aoc::protocol::repl::NetworkMove& m) {
        return send_move(client_move(m));
    }
};

inline bool is_ack(const fake::Delivered& d, float ts) {
    const auto blocks = aoc::protocol::repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!d.exports.empty() || d.reliable || d.content.bit_pos() != 68 || !blocks || blocks->size() != 1) {
        return false;
    }
    const aoc::protocol::schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    size_t pos_bits = (*blocks)[0].payload_pos;
    const auto h = aoc::protocol::repl::read_field_header(
        pawn, d.content.data(), pos_bits + (*blocks)[0].payload_bits, pos_bits);
    if (!h || std::string(pawn.field(h->field)->name) != "ClientMoveResponsePacked") return false;
    aoc::protocol::wire::PacketReader r(d.content.data(), d.content.byte_size(), pos_bits + h->payload_bits);
    r.set_pos(pos_bits);
    return r.read_bits(10) == 645 && std::bit_cast<float>(static_cast<uint32_t>(r.read_bits(32))) == ts;
}

inline std::optional<DecodedRpc> as_correction(const fake::Delivered& d) {
    DecodedRpc r = decode_rpc(d.content, class_named("PlayerPawn_C"));
    if (!r.ok || r.name != "ClientAdjustPosition") return std::nullopt;
    return r;
}

inline bool correction_is(
    const fake::Delivered& d,
    float ts,
    const aoc::protocol::repl::Vec3d& loc,
    uint8_t mode = aoc::protocol::repl::kMoveWalking) {
    const auto r = as_correction(d);
    if (!r || d.reliable || !d.exports.empty()) return false;
    const aoc::protocol::repl::Value* t = param(*r, "TimeStamp");
    const aoc::protocol::repl::Value* l = param(*r, "NewLoc");
    const aoc::protocol::repl::Value* m = param(*r, "ServerMovementMode");
    const float got_ts = t && t->get_if<float>() ? *t->get_if<float>() : 0.0f;
    return (t || ts == 0.0f) && got_ts == ts && l && l->get_if<aoc::protocol::repl::Vec3d>() &&
           *l->get_if<aoc::protocol::repl::Vec3d>() == loc &&
           (m ? *m->get_if<uint64_t>() : 0) == mode;
}

inline bool correction_is(const fake::Delivered& d, float ts, double x, double y, double z) {
    return correction_is(d, ts, aoc::protocol::repl::Vec3d{x, y, z}, aoc::protocol::repl::kMoveWalking);
}

inline bool correction_is(
    const fake::Delivered& d,
    float ts,
    const aoc::worldd::GoodPosition& p,
    uint8_t mode = aoc::protocol::repl::kMoveWalking) {
    return correction_is(d, ts, aoc::protocol::repl::Vec3d{p.x, p.y, p.z}, mode);
}

void run_movement_saver();
void run_movement_based_tests();
void run_movement_presence_tests();
