// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "protocol/repl/block_reader.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "world/node_wire.h"

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

#ifndef NODES_CONTENT_DB_PATH
#define NODES_CONTENT_DB_PATH "dist/content/content.db"
#endif
#ifndef NODES_SEED_PATH
#define NODES_SEED_PATH "deploy/citynode_seed.json"
#endif

inline std::string db_path() {
    const char *env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : NODES_CONTENT_DB_PATH;
}

inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kSister = "0123456789abcdef0123456789abcdef";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(5000)};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const aoc::worldd::svc::ItemContainer Bag = aoc::worldd::svc::ItemContainer::Backpack;
inline const aoc::worldd::svc::ItemContainer Worn = aoc::worldd::svc::ItemContainer::Equipment;
inline constexpr int64_t kJoeva = 108646121231993ll;
inline constexpr int64_t kHalcyon = 108646610851452ll;

inline const aoc::protocol::schema::ClassSchema &class_named(const char *name) {
    const aoc::protocol::schema::ClassSchema *c = aoc::protocol::schema::gen::find_class_schema(name);
    if (!c) {
        std::printf("  [FAIL] no schema %s\n", name);
        std::exit(2);
    }
    return *c;
}

inline std::string guid_n(uint32_t n) {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%08x%08x%08x%08x", 0xA0000000u + n, 0x1000u + n, 0x2000u + n, n);
    return buf;
}

inline aoc::worldd::svc::InventoryItem make_item(aoc::worldd::svc::ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
    aoc::worldd::svc::InventoryItem it;
    it.container = c; it.slot = slot; it.item_record_id = record; it.instance_guid = guid_n(n);
    it.quantity = quantity; it.quality = 1; it.durability = 11; it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

inline std::vector<aoc::worldd::svc::InventoryItem> kit() {
    namespace fi = content_fixture::item;
    const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}};
    std::vector<aoc::worldd::svc::InventoryItem> out;
    for (const auto &[slot, record] : worn)
        out.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    out.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    return out;
}

inline aoc::worldd::svc::Character cyb_record(bool with_sister = true) {
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
    c.items = kit();
    if (with_sister)
        c.account_characters = {{kCyb, "cyb"}, {kSister, "sister"}};
    return c;
}

inline aoc::worldd::bootstrap::LoginIdentity identity() { return {kMachine, kMachine}; }
inline double world_time_at(aoc::worldd::Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

struct RecordingSink : aoc::worldd::PositionSink {
    void save(const std::string &, const aoc::worldd::svc::Position &) override {}
    void save_inventory(const std::string &, std::vector<aoc::worldd::svc::InventoryItem>) override {}
    void save_progress(const std::string &, aoc::worldd::svc::Progress) override {}
};

/// A node control that remembers what the console asked.
struct RecordingControl : aoc::worldd::world::NodeControl {
    std::vector<std::string> calls;
    void set_level(const std::string &n, int level, bool dv, const std::string &by) override {
        calls.push_back("set_level " + n + " " + std::to_string(level) + " " + (dv ? "dv" : "-") + " | " + by);
    }
    void add_experience(const std::string &n, int64_t amount, const std::string &by) override {
        calls.push_back("add_xp " + n + " " + std::to_string(amount) + " | " + by);
    }
    void node_data(const std::string &n, const std::string &by) override { calls.push_back("node_data " + n + " | " + by); }
    void clock(const std::string &by) override { calls.push_back("clock | " + by); }
    void advance_clock(int64_t s, const std::string &by) override { calls.push_back("advance " + std::to_string(s) + " | " + by); }
};

inline std::unique_ptr<aoc::worldd::world::CityNodes> g_nodes;
inline std::unique_ptr<aoc::worldd::world::NodeWire> g_wire;

inline const aoc::protocol::schema::RpcSchema &pc_rpc(const char *name) {
    const aoc::protocol::schema::RpcSchema *r = class_named("AoCPlayerControllerBP_C").rpc(name);
    if (!r) { std::printf("  [FAIL] no RPC %s\n", name); std::exit(2); }
    return *r;
}

struct Fixture {
    std::filesystem::path dir;
    std::optional<aoc::worldd::content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;
    std::unique_ptr<aoc::worldd::world::CityNodes> nodes;
    RecordingControl control;

    explicit Fixture(bool seeded = true, bool with_control = true) {
        dir = content_fixture::make_temp_dir("worldd_node_state_");
        const std::filesystem::path db = dir / "content.db";
        if (!content_fixture::make_db(db)) std::exit(2);
        std::string error;
        content = aoc::worldd::content::ContentDb::open(db.string(), error);
        if (!content) { std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str()); std::exit(2); }
        world = std::make_unique<aoc::worldd::PlayerWorld>(&*content, "", aoc::worldd::bootstrap::GameStateMode::Tier3, kServerId, guids);
        nodes = aoc::worldd::world::CityNodes::load(db_path(), error);
        if (!nodes) { std::printf("  [FAIL] the real content.db has no city nodes: %s\n", error.c_str()); std::exit(2); }
        size_t applied = 0;
        if (seeded && !nodes->apply_seed_file(NODES_SEED_PATH, error, &applied)) { std::printf("  [FAIL] the seed does not load: %s\n", error.c_str()); std::exit(2); }
        world->set_city_nodes(nodes.get());
        world->set_node_wire(g_wire.get());
        if (with_control) world->set_node_control(&control);
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() {
        world.reset(); nodes.reset(); content.reset();
        std::error_code ec; std::filesystem::remove_all(dir, ec);
    }
};

struct Harness {
    Fixture fx;
    RecordingSink sink;
    aoc::worldd::Connection conn{aoc::worldd::ConnectionConfig{aoc::protocol::conn::OuterHeader{0x500c7696u, 0, 5}, aoc::protocol::conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{aoc::protocol::conn::OuterHeader{0x500c7696u, 0, 5}, aoc::protocol::conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<aoc::worldd::PlayerSession> s;
    aoc::worldd::Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0, gs_ch = 0;
    aoc::worldd::svc::Character record;

    explicit Harness(aoc::worldd::svc::Character r, bool seeded = true, bool with_control = true)
        : fx(seeded, with_control), record(std::move(r)) {
        fx.world->set_position_sink(&sink);
        s = aoc::worldd::PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) { std::printf("  [FAIL] the session does not join: %s\n", error.c_str()); std::exit(2); }
        pawn_ch = s->channel_of(aoc::worldd::bootstrap::Role::Pawn);
        pc_ch = s->channel_of(aoc::worldd::bootstrap::Role::PlayerController);
        gs_ch = s->channel_of(aoc::worldd::bootstrap::Role::GameState);
    }

    void flush() {
        for (auto &pkt : conn.flush(t))
            for (const auto &d : client.receive(pkt).delivered) got.push_back(d);
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        const std::vector<aoc::worldd::InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const aoc::worldd::InBunch &b : in) s->on_bunch(conn, b, t);
        s->end_of_datagram(conn);
    }
    void step(std::chrono::milliseconds d = std::chrono::milliseconds(20)) {
        t += d; fx.world->tick_world(world_time_at(t)); s->tick(conn, t);
        flush(); s->take_acks(conn); client_sends({});
    }
    void run(std::chrono::milliseconds total) {
        for (std::chrono::milliseconds done{0}; done < total; done += std::chrono::milliseconds(20)) step();
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 8 && i < 1500; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    void play();
};

/// The client's bunch with several RPCs of one class, each its own content block (the retail client's possession bunch has 72, the node requests 98).
inline aoc::protocol::emit::BunchWriter rpcs_bunch(const aoc::protocol::schema::ClassSchema &cls, const std::vector<std::pair<const char *, aoc::protocol::repl::RpcParams>> &calls) {
    aoc::protocol::emit::BunchWriter out;
    for (const auto &[name, params] : calls) {
        const aoc::protocol::schema::RpcSchema *rpc = cls.rpc(name);
        aoc::protocol::emit::BunchWriter field;
        bool failed = false;
        if (!rpc || (aoc::protocol::repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 && !params.values.empty()) || failed) {
            std::printf("  [FAIL] %s does not write\n", name);
            continue;
        }
        aoc::protocol::repl::BlockHeader header;
        aoc::protocol::repl::write_content_block(out, header, field, /*server_form=*/false);
    }
    return out;
}

inline aoc::protocol::emit::BunchWriter actor_rpc(const aoc::protocol::schema::ClassSchema &cls, const char *name, const aoc::protocol::repl::RpcParams &params) {
    return rpcs_bunch(cls, {{name, params}});
}

inline void Harness::play() {
    settle();
    client_sends({client.actor(pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    settle();
    run(std::chrono::milliseconds(400));
}

/// UPackageMapClient::SerializeNewActor (test_worldd_session.cpp read_new_actor): the bit after it, 0 when it does not read.
inline size_t new_actor_end(const aoc::protocol::emit::BunchWriter &w, bool player_controller) {
    aoc::protocol::wire::PacketReader r(w.data(), w.byte_size(), w.bit_pos());
    for (int i = 0; i < 3; ++i) { r.read_uint32(); r.read_uint32(); r.read_uint32(); r.read_uint32(); }
    auto vector = [&] {
        if (!r.read_bit()) return true;
        return r.read_bit() && aoc::protocol::repl::read_quantized_vector(r).has_value();
    };
    if (!vector()) return 0;
    if (r.read_bit() && !aoc::protocol::repl::read_struct("FRotator", r)) return 0;
    if (!vector() || !vector()) return 0;
    if (player_controller) { r.read_uint8(); r.read_uint32(); }
    return r.overflowed() ? 0 : r.pos();
}

using States = std::map<uint64_t, aoc::protocol::repl::ObjectState>;

/// The states a chain's property blocks carry (an open's blocks start after SerializeNewActor).
inline States states_of(const fake::Delivered &d, const aoc::protocol::repl::Actor &actor, bool player_controller = false) {
    States out;
    const size_t start = d.open ? new_actor_end(d.content, player_controller) : 0;
    const auto blocks = aoc::protocol::repl::read_content_blocks(d.content.data(), d.content.bit_pos(), start);
    if (!blocks) return out;
    for (const aoc::protocol::repl::BlockView &b : *blocks) {
        const aoc::protocol::repl::Subobject *sub = b.header.is_actor ? nullptr : actor.find(b.header.object);
        const aoc::protocol::schema::ClassSchema *cls = b.header.is_actor ? actor.obj.schema : (sub ? sub->obj.schema : nullptr);
        const uint64_t id = b.header.is_actor ? 0 : b.header.object.ObjectId;
        if (!cls) continue;
        aoc::protocol::emit::BunchWriter w;
        w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
        auto st = aoc::protocol::repl::load_object_state(*cls, w.data(), b.payload_bits, b.header.has_rep_layout);
        if (st) out.emplace(id, std::move(*st));
    }
    return out;
}

/// The chains of a channel from `from` on.
inline std::vector<const fake::Delivered *> chains_on(const Harness &h, uint32_t channel, size_t from, bool include_open = true) {
    std::vector<const fake::Delivered *> out;
    for (size_t i = from; i < h.got.size(); ++i)
        if (h.got[i].channel == channel && !h.got[i].close && (include_open || !h.got[i].open)) out.push_back(&h.got[i]);
    return out;
}

void run_node_state_control_tests();
