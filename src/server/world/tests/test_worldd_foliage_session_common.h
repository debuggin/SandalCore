// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "protocol/repl/block_reader.h"
#include "protocol/repl/fname.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace bs = aoc::worldd::bootstrap;
namespace combat = aoc::worldd::combat;
namespace content = aoc::worldd::content;
namespace foliage = aoc::worldd::foliage;
namespace gather = aoc::worldd::gather;
namespace items = aoc::worldd::items;
namespace spawns = aoc::worldd::spawns;
namespace svc = aoc::worldd::svc;
namespace world = aoc::worldd::world;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace fs = std::filesystem;
namespace cf = content_fixture;
namespace fi = content_fixture::item;
using aoc::worldd::Clock;
using aoc::worldd::Connection;
using aoc::worldd::ConnectionConfig;
using aoc::worldd::GoodPosition;
using aoc::worldd::InBunch;
using aoc::worldd::PlayerSession;
using aoc::worldd::PlayerWorld;
using aoc::worldd::PositionSink;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;
using svc::ItemContainer;

inline int g_pass = 0;
inline int g_fail = 0;
#undef CHECK
#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            std::printf("  [ok ] %s\n", msg);                                                                          \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            std::printf("  [FAIL] %s\n", msg);                                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const Clock::time_point t0{seconds(5000)};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const ItemContainer Bag = ItemContainer::Backpack;
inline const ItemContainer Worn = ItemContainer::Equipment;
inline const ItemContainer R0 = ItemContainer::Resource0;

inline const char *g_path = nullptr;
inline const char *g_tsv = nullptr;

namespace id {
inline constexpr int64_t kMining = 108752769996425ll, kLumberjacking = 108752769996426ll,
                         kHerbalism = 108752770061963ll;
inline constexpr int64_t kLtHerbalist = 109460724995707ll;
inline constexpr int64_t kT1Pickaxe = 6064629375589548039ll, kT3Pickaxe = 6064629376467927053ll;
inline constexpr int64_t kDaffodil = 6064632138835886135ll, kMoonbell = 6064631483095515243ll,
                         kRividium = 6064631669497200654ll, kHalcyonite = 6064631483054948432ll;
inline constexpr int64_t kLimestone = 6064631483017789494ll, kOakLarge2 = 6064632138726178834ll;
inline constexpr int64_t kDaffodilItem = 6064629184431849472ll;
} // namespace id

namespace fid {
inline constexpr int64_t kLumberjacking = 108752769996426ll;
inline constexpr double kOakX = -1032805.5, kOakY = 495823.7, kOakZ = 12846.0;
inline constexpr double kStandX = -1032839.01, kStandY = 495968.38, kStandZ = 12950.98;
inline constexpr const char *kOwner = "InstancedFoliageActor_25600_-41_19_0";
inline constexpr const char *kFismc0 = "IntrepidGatherableFISMC_0";
inline constexpr int64_t kOakLog = 109162169652924ll;
inline constexpr int64_t kCoal = 109162169652923ll;
} // namespace fid

inline const char *db_path() {
    const char *env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef GATHER_CONTENT_DB_PATH
    if (fs::exists(GATHER_CONTENT_DB_PATH)) return GATHER_CONTENT_DB_PATH;
#endif
    return nullptr;
}

inline const schema::ClassSchema &class_named(const char *name) {
    const schema::ClassSchema *c = gen::find_class_schema(name);
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

inline svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = 1;
    it.durability = (c == R0) ? 0.0f : 11.0f;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

inline svc::InventoryItem make_tool(ItemContainer c, int slot, int64_t record, uint32_t n, float durability = 150.0f) {
    svc::InventoryItem it = make_item(c, slot, record, n, 1);
    it.durability = durability;
    return it;
}

inline svc::Character record_of(const char *id_hex, const char *name, bool tools = true) {
    svc::Character c;
    c.id = id_hex;
    c.account_id = 3;
    c.name = name;
    c.race_id = 0;
    c.gender_id = 0;
    c.class_id = 2;
    c.archetype_id = 17747;
    c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb;
    c.level = 1;
    const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet},   {2, fi::kChest},  {5, fi::kLegs},  {6, fi::kFeet},
                                            {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}};
    for (const auto &[slot, record] : worn)
        c.items.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    if (tools) {
        c.items.push_back(make_tool(Worn, 26, id::kT1Pickaxe, 126, 150.0f));
        c.items.push_back(make_tool(Worn, 29, 6064629375589613576ll, 129, 150.0f));
        c.items.push_back(make_tool(Worn, 33, 6064629375589482502ll, 133, 150.0f));
    }
    return c;
}

inline svc::Character record_at(const char *id_hex, const char *name, double x, double y, double z) {
    svc::Character c = record_of(id_hex, name);
    c.has_position = true;
    c.position.x = x;
    c.position.y = y;
    c.position.z = z;
    return c;
}

inline bs::LoginIdentity identity() {
    return {kMachine, kMachine};
}
inline double world_time_at(Clock::time_point t) {
    return std::chrono::duration<double>(t - t0).count();
}

struct RecordingSink : PositionSink {
    struct Progress {
        std::string id;
        svc::Progress progress;
    };
    std::vector<std::pair<std::string, std::vector<svc::InventoryItem>>> inventories;
    std::vector<Progress> progress;
    void save(const std::string &, const svc::Position &) override {}
    void save_inventory(const std::string &id, std::vector<svc::InventoryItem> items) override {
        inventories.emplace_back(id, std::move(items));
    }
    void save_progress(const std::string &id, svc::Progress p) override { progress.push_back({id, std::move(p)}); }
};

inline constexpr double kCx = bs::kDefaultSpawn.x, kCy = bs::kDefaultSpawn.y;

struct Rig;

struct Player {
    Rig &rig;
    svc::Character record;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5},
                                     conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870},
                    t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d},
                        14265, 15870};
    std::unique_ptr<PlayerSession> s;
    std::vector<fake::Delivered> got;
    uint32_t pawn_ch = 0, pc_ch = 0;
    std::string error;

    Player(Rig &r, svc::Character rec) : rig(r), record(std::move(rec)) {}
    void join();
    void flush();
    void client_sends(std::vector<fake::Bunch> bunches);
    void step_session();
    void request(const BunchWriter &rpc, uint32_t channel);
    void send_now(const BunchWriter &rpc, uint32_t channel);
};

inline void patch_gather_node(world::WorldActor &node, int64_t def_guid, int64_t interaction_guid) {
    repl::ObjectState &actor = node.actor.obj;
    actor.state.set(*actor.schema, "GatherableActorDef.Guid", repl::Value::i64(def_guid));
    for (repl::Subobject &sub : node.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UInteractableComponent") continue;
        const repl::Value *data = sub.obj.state.get(*sub.obj.schema, "InteractionInputData");
        const auto *arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
        if (!arr || arr->elements.empty()) continue;
        repl::ArrayValue copy = *arr;
        copy.elements[0].values[6] = repl::Value::i64(interaction_guid);
        sub.obj.state.set(*sub.obj.schema, "InteractionInputData", repl::Value(std::move(copy)));
    }
}

struct Rig {
    std::optional<content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    std::optional<spawns::CapturedData> captured;
    const spawns::OpenTemplate *donor = nullptr;
    RecordingSink sink;
    Clock::time_point t = t0;
    std::vector<std::unique_ptr<Player>> players;

    bool open(const char *path, double respawn_mult = 0.001) {
        std::string error;
        db = content::ContentDb::open(path, error);
        if (!db) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            return false;
        }
        world = std::make_unique<PlayerWorld>(&*db, "", bs::GameStateMode::Tier2, kServerId, guids);
        world->set_position_sink(&sink);
        auto c = spawns::CapturedData::load(error);
        if (!c) {
            std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
            return false;
        }
        captured = std::move(*c);
        for (const spawns::OpenTemplate &o : captured->opens()) {
            if (o.category == "gatherable") {
                donor = &o;
                break;
            }
        }
        gather::Config cfg;
        cfg.board.respawn_mult = respawn_mult;
        if (!world->start_gathering(cfg, error)) {
            std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
            return false;
        }
        return donor != nullptr;
    }
    gather::Gathering &g() { return *world->gathering(); }
    gather::NodeBoard &board() { return g().board(); }

    uint64_t add_node(int64_t def_guid, double dx, double dy, uint32_t seed, double z = bs::kDefaultSpawn.z) {
        const content::GatherDefRow *def = g().def(def_guid);
        spawns::InstanceParams p;
        p.keep_captured_position = true;
        p.server_id = kServerId;
        p.seed = 0x6A12DE0ull ^ seed;
        std::string why;
        auto node = spawns::instantiate(*donor, [this] { return guids.next(); }, p, why);
        if (!node || !def) return 0;
        patch_gather_node(*node, def_guid, def->interaction_guid);
        node->x = kCx + dx;
        node->y = kCy + dy;
        node->z = z;
        node->npc.source = world::Source::Runtime;
        const uint64_t id = node->id;
        return world->registry().add(std::move(*node)) ? id : 0;
    }
    uint64_t component_of(uint64_t node) {
        const world::WorldActor *a = world->registry().find(node);
        return a ? a->npc.interact_component : 0;
    }

    Player &add_player(svc::Character rec) {
        players.push_back(std::make_unique<Player>(*this, std::move(rec)));
        players.back()->join();
        return *players.back();
    }
    void step(milliseconds d = milliseconds(20)) {
        t += d;
        for (auto &p : players) {
            if (p->s) p->step_session();
        }
    }
    void run(milliseconds total) {
        for (milliseconds done{0}; done < total; done += milliseconds(20)) step();
    }
    void settle() {
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            size_t before = 0;
            for (auto &p : players) before += p->got.size();
            step();
            size_t after = 0;
            bool busy = false;
            for (auto &p : players) {
                after += p->got.size();
                busy = busy || (p->s && (p->s->join_pending() != 0 || p->s->replies_pending() != 0));
            }
            quiet = (after == before && !busy) ? quiet + 1 : 0;
        }
    }
};

inline void Player::join() {
    s = PlayerSession::join(conn, *rig.world, record, identity(), world_time_at(rig.t), error);
    if (!s) {
        std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
        std::exit(2);
    }
    pawn_ch = s->channel_of(bs::Role::Pawn);
    pc_ch = s->channel_of(bs::Role::PlayerController);
}
inline void Player::flush() {
    for (auto &pkt : conn.flush(rig.t)) {
        const auto r = client.receive(pkt);
        for (const auto &d : r.delivered) got.push_back(d);
    }
}
inline void Player::client_sends(std::vector<fake::Bunch> bunches) {
    const auto pkt = client.packet(std::move(bunches));
    const std::vector<InBunch> in = conn.receive(pkt, rig.t);
    s->take_acks(conn);
    for (const InBunch &b : in) s->on_bunch(conn, b, rig.t);
    s->end_of_datagram(conn);
}
inline void Player::step_session() {
    s->tick(conn, rig.t);
    flush();
    s->take_acks(conn);
    client_sends({});
}
inline void Player::request(const BunchWriter &rpc, uint32_t channel) {
    client_sends({client.actor(channel, rpc, true)});
    rig.settle();
}
inline void Player::send_now(const BunchWriter &rpc, uint32_t channel) {
    client_sends({client.actor(channel, rpc, true)});
    rig.run(milliseconds(60));
}

inline BunchWriter actor_rpc(const schema::ClassSchema &cls, const char *name, const repl::RpcParams &params) {
    BunchWriter out;
    const schema::RpcSchema *rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 && !params.values.empty()) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    if (failed) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

inline BunchWriter interrupt_channel() {
    return actor_rpc(class_named("PlayerPawn_C"), "ServerInterruptChannel", {});
}

inline std::ostringstream g_log;
inline size_t count_in_log(const char *what) {
    const std::string text = g_log.str();
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

inline BunchWriter item_used(int storage, int slot, const std::string &guid_hex) {
    const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema *rpc = pawn.rpc("ServerItemUsed");
    repl::RpcParams p;
    const auto g = items::guid_of(guid_hex);
    const repl::GuidValue gv = g ? *g : repl::GuidValue{};
    repl::set_rpc_param(*rpc, p, "Storage", repl::Value::u64(static_cast<uint64_t>(storage)));
    repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.A", repl::Value::i64(static_cast<int32_t>(gv.a)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.B", repl::Value::i64(static_cast<int32_t>(gv.b)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.C", repl::Value::i64(static_cast<int32_t>(gv.c)));
    repl::set_rpc_param(*rpc, p, "ExpectedItem.D", repl::Value::i64(static_cast<int32_t>(gv.d)));
    repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    return actor_rpc(pawn, "ServerItemUsed", p);
}

inline void read_channeling_block(size_t idx, const schema::ClassSchema &info, const fake::Delivered &d,
                                  const repl::BlockView &b, std::vector<std::pair<size_t, repl::ChannelingData>> &out) {
    if (b.header.is_actor || !b.header.has_rep_layout) return;
    size_t pos = b.payload_pos;
    const auto st = repl::read_rep_block(info, d.content.data(), b.payload_pos + b.payload_bits, pos);
    const repl::Value *v = st ? st->get(info, "ChannelingData") : nullptr;
    if (v && v->get_if<repl::ChannelingData>()) out.emplace_back(idx, *v->get_if<repl::ChannelingData>());
}

inline std::vector<std::pair<size_t, repl::ChannelingData>> channeling_seen(const Player &p, size_t from) {
    std::vector<std::pair<size_t, repl::ChannelingData>> out;
    const schema::ClassSchema &info = class_named("UCharacterSecondaryInfo");
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pawn_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto &b : *blocks) read_channeling_block(i, info, p.got[i], b, out);
    }
    return out;
}

inline void append_chain_rpc_names(size_t idx, const fake::Delivered &d, const repl::BlockView &b,
                                   std::vector<std::pair<size_t, std::string>> &out) {
    if (!b.header.is_actor || b.header.has_rep_layout) return;
    size_t pos = b.payload_pos;
    auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), d.content.data(),
                                       b.payload_pos + b.payload_bits, pos);
    if (!calls) return;
    for (const auto &c : *calls) out.emplace_back(idx, c.rpc ? c.rpc->name : "?");
}

inline std::vector<std::pair<size_t, std::string>> pc_chain_calls(const Player &p, size_t from) {
    std::vector<std::pair<size_t, std::string>> out;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto &b : *blocks) append_chain_rpc_names(i, p.got[i], b, out);
    }
    return out;
}

inline std::vector<std::string> pc_call_names_since(const Player &p, size_t from) {
    std::vector<std::string> out;
    for (const auto &[chain, name] : pc_chain_calls(p, from)) out.push_back(name);
    return out;
}

inline void play(Player &p) {
    p.rig.settle();
    p.request(actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), p.pawn_ch);
    p.rig.run(seconds(2));
    p.rig.settle();
}

inline bool run_until(Rig &rig, const std::function<bool()> &done, milliseconds limit = seconds(30)) {
    for (milliseconds t{0}; t < limit; t += milliseconds(20)) {
        if (done()) return true;
        rig.step();
    }
    return done();
}

struct FoliageSeen {
    size_t chain = 0;
    float duration = 0;
    double start_time = 0;
    int64_t instance = 0;
    std::string owner, component;
    uint64_t state = 0;
    int64_t tool_record = 0;
};

inline std::string name_text(const repl::Value *v) {
    const repl::NameValue *n = v ? v->get_if<repl::NameValue>() : nullptr;
    return n ? repl::fname_text(*n) : std::string();
}

inline void read_foliage_seen_block(size_t chain_idx, const fake::Delivered &d, const repl::BlockView &b,
                                    const emit::FIntrepidNetworkGUID &base_info, std::vector<FoliageSeen> &out) {
    if (b.header.is_actor || !b.header.has_rep_layout || !(b.header.object == base_info)) return;
    const schema::ClassSchema &info = class_named("UPlayerInfo");
    size_t pos = b.payload_pos;
    const auto st = repl::read_rep_block(info, d.content.data(), b.payload_pos + b.payload_bits, pos);
    if (!st) return;
    const repl::Value *dur = st->get(info, "FoliageData.Duration");
    if (!dur) return;
    FoliageSeen f;
    f.chain = chain_idx;
    f.duration = dur->get_if<float>() ? *dur->get_if<float>() : -1.0f;
    const repl::Value *t = st->get(info, "FoliageData.ServerStartTime");
    f.start_time = (t && t->get_if<double>()) ? *t->get_if<double>() : -1.0;
    const repl::Value *idx = st->get(info, "FoliageData.OriginalInstanceIndex");
    f.instance = (idx && idx->get_if<int64_t>()) ? *idx->get_if<int64_t>() : -2;
    f.owner = name_text(st->get(info, "FoliageData.FoliageName"));
    f.component = name_text(st->get(info, "FoliageData.FoliageComponentUUID"));
    const repl::Value *s = st->get(info, "FoliageData.FoliageState");
    f.state = (s && s->get_if<uint64_t>()) ? *s->get_if<uint64_t>() : 99;
    const repl::Value *tool = st->get(info, "FoliageData.ToolRecordId");
    f.tool_record = (tool && tool->get_if<int64_t>()) ? *tool->get_if<int64_t>() : -1;
    out.push_back(std::move(f));
}

inline std::vector<FoliageSeen> foliage_data_seen(const Player &p, size_t from) {
    std::vector<FoliageSeen> out;
    const bs::SessionGuids &g = p.s->bootstrap().setup().guids;
    for (size_t i = from; i < p.got.size(); ++i) {
        if (p.got[i].channel != p.pawn_ch || p.got[i].open || p.got[i].close) continue;
        const auto blocks = repl::read_content_blocks(p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto &b : *blocks) read_foliage_seen_block(i, p.got[i], b, g.base_character_info, out);
    }
    return out;
}

inline size_t new_actor_prefix_bits(const emit::BunchWriter &content) {
    aoc::protocol::wire::PacketReader r(content.data(), (content.bit_pos() + 7) / 8, content.bit_pos());
    r.set_pos(384);
    if (r.read_bit()) {
        if (r.read_bit()) {
            if (!repl::read_quantized_vector(r)) return 0;
        } else {
            r.read_bits(64);
            r.read_bits(64);
            r.read_bits(64);
        }
    }
    r.read_bit();
    r.read_bit();
    r.read_bit();
    return r.pos();
}

inline BunchWriter foliage_start(const char *owner, const char *component, uint32_t index, uint64_t state) {
    const schema::ClassSchema &pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema *rpc = pc.rpc("ServerStartInteractionWithFoliage");
    repl::RpcParams p;
    if (!rpc) return BunchWriter();
    if (index != 0) repl::set_rpc_param(*rpc, p, "OriginalInstanceIndex", repl::Value::i64(index));
    repl::set_rpc_param(*rpc, p, "FoliageName", repl::Value(repl::fname_parse(owner)));
    repl::set_rpc_param(*rpc, p, "FoliageComponentUUID", repl::Value(repl::fname_parse(component)));
    repl::set_rpc_param(*rpc, p, "FoliageState", repl::Value::u64(state));
    return actor_rpc(pc, "ServerStartInteractionWithFoliage", p);
}

struct FRig {
    Rig rig;
    bool open(double regrow_mult = 0.01, bool all_cells = true) {
        if (!rig.open(g_path)) return false;
        std::string error;
        foliage::Config cfg;
        cfg.map_path = g_tsv;
        cfg.regrow_mult = regrow_mult;
        cfg.all_cells = all_cells;
        if (!rig.world->start_foliage(cfg, error)) {
            std::printf("  [FAIL] foliage does not start: %s\n", error.c_str());
            return false;
        }
        return true;
    }
    foliage::Foliage &f() { return *rig.world->foliage(); }
    uint32_t oak_row() { return *f().map().find(fid::kOwner, fid::kFismc0, 0); }
};

inline Player &join_at_the_oak(FRig &fr, const char *id_hex, const char *name) {
    Player &p = fr.rig.add_player(record_at(id_hex, name, fid::kStandX, fid::kStandY, fid::kStandZ));
    play(p);
    return p;
}

inline bool init_foliage_session_env() {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    const char *path = db_path();
    if (!path) {
        std::printf("[skip] test_worldd_foliage_session: no content.db (set "
                    "AOC_CONTENT_DB, or build "
                    "dist/content/content.db)\n");
        return false;
    }
#ifdef FOLIAGE_TSV_PATH
    if (fs::exists(FOLIAGE_TSV_PATH)) g_tsv = FOLIAGE_TSV_PATH;
#endif
    if (const char *env = std::getenv("AOC_FOLIAGE_MAP"); env && *env) g_tsv = env;
    if (!g_tsv) {
        std::printf("[skip] test_worldd_foliage_session: no "
                    "dist/content/region_foliage.tsv\n");
        return false;
    }
    g_path = path;
    std::printf("content.db: %s\nmap: %s\n", path, g_tsv);
    return true;
}

void run_foliage_unnamed_call_test();
void run_foliage_session_wire_tests();
