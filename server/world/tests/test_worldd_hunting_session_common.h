// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "nav_fixture.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"

#include <sstream>

namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace fs = std::filesystem;
namespace cf = content_fixture;
namespace fi = content_fixture::item;

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg)                                  \
    do {                                                  \
        if (cond) {                                       \
            std::printf("  [ok ] %s\n", msg);             \
            g_pass++;                                     \
        } else {                                          \
            std::printf("  [FAIL] %s\n", msg);            \
            g_fail++;                                     \
        }                                                 \
    } while (0)

inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(5000)};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const aoc::worldd::svc::ItemContainer Bag = aoc::worldd::svc::ItemContainer::Backpack;
inline const aoc::worldd::svc::ItemContainer Worn = aoc::worldd::svc::ItemContainer::Equipment;
inline const aoc::worldd::svc::ItemContainer R0 = aoc::worldd::svc::ItemContainer::Resource0;
inline const char* g_path = nullptr;
inline std::ostringstream g_log;

namespace id {
inline constexpr int64_t kMining = 108752769996425ll;
inline constexpr int64_t kLumberjacking = 108752769996426ll;
inline constexpr int64_t kHerbalism = 108752770061963ll;
inline constexpr int64_t kLtHerbalist = 109460724995707ll;
inline constexpr int64_t kT1Pickaxe = 6064629375589548039ll;
inline constexpr int64_t kT3Pickaxe = 6064629376467927053ll;
inline constexpr int64_t kDaffodil = 6064632138835886135ll;
inline constexpr int64_t kMoonbell = 6064631483095515243ll;
inline constexpr int64_t kRividium = 6064631669497200654ll;
inline constexpr int64_t kHalcyonite = 6064631483054948432ll;
inline constexpr int64_t kLimestone = 6064631483017789494ll;
inline constexpr int64_t kOakLarge2 = 6064632138726178834ll;
inline constexpr int64_t kDaffodilItem = 6064629184431849472ll;
}  // namespace id

namespace hid {
inline constexpr int64_t kHunting = 108752770061965ll;
inline constexpr int64_t kBow = 6064630140303376483ll;
inline constexpr int64_t kCoal = 109162169652923ll;
}  // namespace hid

inline const char* db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
#ifdef GATHER_CONTENT_DB_PATH
    if (fs::exists(GATHER_CONTENT_DB_PATH)) return GATHER_CONTENT_DB_PATH;
#endif
    return nullptr;
}

inline const schema::ClassSchema& class_named(const char* name) {
    const schema::ClassSchema* c = gen::find_class_schema(name);
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

inline aoc::worldd::svc::InventoryItem make_item(
    aoc::worldd::svc::ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
    aoc::worldd::svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = 1;
    it.durability = (quantity == 20) ? 0.0f : 11.0f;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

inline aoc::worldd::svc::InventoryItem make_tool_item(int slot, int64_t record, uint32_t n, float durability = 150.0f) {
    aoc::worldd::svc::InventoryItem it = make_item(Worn, slot, record, n, 1);
    it.durability = durability;
    return it;
}

inline aoc::worldd::svc::Character record_of(const char* id_hex, const char* name, bool tools = true) {
    aoc::worldd::svc::Character c;
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
    const std::pair<int, int64_t> worn[] = {
        {1, fi::kHelmet}, {2, fi::kChest},   {5, fi::kLegs},  {6, fi::kFeet},
        {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}};
    for (const auto& [slot, record] : worn) {
        c.items.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    }
    if (tools) {
        c.items.push_back(make_tool_item(26, id::kT1Pickaxe, 126));
        c.items.push_back(make_tool_item(29, 6064629375589613576ll, 129));
        c.items.push_back(make_tool_item(33, 6064629375589482502ll, 133));
        c.items.push_back(make_tool_item(38, 6064630140303376483ll, 138));
    }
    return c;
}

inline bs::LoginIdentity identity() { return {kMachine, kMachine}; }
inline double world_time_at(aoc::worldd::Clock::time_point t) {
    return std::chrono::duration<double>(t - t0).count();
}

struct RecordingSink : aoc::worldd::PositionSink {
    struct Progress {
        std::string id;
        aoc::worldd::svc::Progress progress;
    };
    std::vector<std::pair<std::string, std::vector<aoc::worldd::svc::InventoryItem>>> inventories;
    std::vector<Progress> progress;
    void save(const std::string&, const aoc::worldd::svc::Position&) override {}
    void save_inventory(const std::string& id, std::vector<aoc::worldd::svc::InventoryItem> items) override {
        inventories.emplace_back(id, std::move(items));
    }
    void save_progress(const std::string& id, aoc::worldd::svc::Progress p) override {
        progress.push_back({id, std::move(p)});
    }
};

inline constexpr double kCx = bs::kDefaultSpawn.x;
inline constexpr double kCy = bs::kDefaultSpawn.y;

struct Rig;

struct Player {
    Rig& rig;
    aoc::worldd::svc::Character record;
    aoc::worldd::Connection conn{
        aoc::worldd::ConnectionConfig{
            conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870},
        t0};
    fake::Client client{
        conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<aoc::worldd::PlayerSession> s;
    std::vector<fake::Delivered> got;
    uint32_t pawn_ch = 0;
    uint32_t pc_ch = 0;
    std::string error;

    Player(Rig& r, aoc::worldd::svc::Character rec) : rig(r), record(std::move(rec)) {}
    void join();
    void flush();
    void client_sends(std::vector<fake::Bunch> bunches);
    void step_session();
    void request(const emit::BunchWriter& rpc, uint32_t channel);
    void send_now(const emit::BunchWriter& rpc, uint32_t channel);
};

struct Rig {
    std::optional<aoc::worldd::content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;
    std::optional<aoc::worldd::spawns::CapturedData> captured;
    std::optional<aoc::worldd::spawns::GenericNpcs> generic;
    aoc::worldd::content::PopulationData pop;
    RecordingSink sink;
    aoc::worldd::Clock::time_point t = t0;
    std::vector<std::unique_ptr<Player>> players;
    uint64_t seed = 1;

    bool open_spawns(std::string& error) {
        auto c = aoc::worldd::spawns::CapturedData::load(error);
        if (!c) {
            std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
            return false;
        }
        captured = std::move(*c);
        auto p = db->population_data(error);
        if (!p) {
            std::printf("  [FAIL] the population does not read: %s\n", error.c_str());
            return false;
        }
        pop = std::move(*p);
        aoc::worldd::gather::Config cfg;
        if (!world->start_gathering(cfg, error)) {
            std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
            return false;
        }
        aoc::worldd::spawns::GenericConfig gc;
        for (const auto& [def, kind] : g().hunting()) gc.hunting_defs.insert(def);
        generic.emplace(*captured, &world->blueprints(), gc);
        return world->combat() != nullptr && world->aftermath() != nullptr;
    }

    bool open(const char* path) {
        std::string error;
        db = aoc::worldd::content::ContentDb::open(path, error);
        if (!db) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            return false;
        }
        world = std::make_unique<aoc::worldd::PlayerWorld>(&*db, "", bs::GameStateMode::Tier2, kServerId, guids);
        world->set_position_sink(&sink);
        if (!world->ready()) {
            std::printf("  [FAIL] the world is not ready: %s\n", world->content_problem().c_str());
            return false;
        }
        if (!world->load_blueprints(NPC_MOTION_PATH, error)) {
            std::printf("  [FAIL] the Blueprint table: %s\n", error.c_str());
            return false;
        }
        auto nav = aoc::worldd::nav::AreaNav::from_file(aoc::worldd::navfix::build_file(), &error);
        if (!nav || !world->adopt_ground(std::move(nav), error)) {
            std::printf("  [FAIL] the ground: %s\n", error.c_str());
            return false;
        }
        return open_spawns(error);
    }

    aoc::worldd::gather::Gathering& g() { return *world->gathering(); }
    aoc::worldd::gather::NodeBoard& board() { return g().board(); }
    double now() const { return world_time_at(t); }

    const aoc::worldd::content::NpcAssetRow* asset(const std::string& name) const {
        for (const auto& [guid, a] : pop.assets) {
            if (a.name == name) return &a;
        }
        return nullptr;
    }

    uint64_t spawn(const std::string& name, double dx, double dy, int level = 5) {
        const aoc::worldd::content::NpcAssetRow* a = asset(name);
        const aoc::worldd::spawns::OpenTemplate* tpl = a ? generic->template_for(*a) : nullptr;
        if (!tpl) return 0;
        aoc::worldd::spawns::InstanceParams p;
        p.x = kCx + dx;
        p.y = kCy + dy;
        p.z = bs::kDefaultSpawn.z;
        p.level = level;
        p.server_id = kServerId;
        p.seed = 0x4F0D5ull + seed++;
        std::string why;
        auto w = aoc::worldd::spawns::instantiate(*tpl, [this] { return world->mint_guid(); }, p, why);
        if (!w) return 0;
        w->npc.name = a->name;
        w->npc.archetype = a->archetype_path;
        w->npc.level = level;
        w->npc.home_x = p.x;
        w->npc.home_y = p.y;
        w->npc.home_z = p.z;
        w->npc.aggro_radius_cm = 1;
        const uint64_t id = w->id;
        if (!world->registry().add(std::move(*w))) return 0;
        aoc::worldd::npc::NpcMotionSetup setup;
        setup.ambient = aoc::worldd::npc::Ambient::Stationary;
        setup.foot_offset = 88.0;
        world->npcs()->add(id, setup);
        return id;
    }

    uint64_t component_of(uint64_t npc) {
        const aoc::worldd::world::WorldActor* a = world->registry().find(npc);
        return a ? a->npc.interact_component : 0;
    }

    void kill(uint64_t npc, uint64_t by_pawn) {
        aoc::worldd::combat::Resolved k;
        k.mods.push_back({1, aoc::worldd::content::kGuidHealth, -1.0e9});
        world->combat()->apply_hit_to_npc(npc, by_pawn, k, now());
    }

    Player& add_player(aoc::worldd::svc::Character rec) {
        players.push_back(std::make_unique<Player>(*this, std::move(rec)));
        players.back()->join();
        return *players.back();
    }

    void step(std::chrono::milliseconds d = std::chrono::milliseconds(20)) {
        t += d;
        world->tick_world(now());
        for (auto& p : players) {
            if (p->s) p->step_session();
        }
    }

    void run(std::chrono::milliseconds total) {
        for (std::chrono::milliseconds done{0}; done < total; done += std::chrono::milliseconds(20)) step();
    }

    size_t total_got() const {
        size_t n = 0;
        for (const auto& p : players) n += p->got.size();
        return n;
    }

    bool any_busy() const {
        for (const auto& p : players) {
            if (p->s && (p->s->join_pending() != 0 || p->s->replies_pending() != 0)) return true;
        }
        return false;
    }

    void settle() {
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            const size_t before = total_got();
            step();
            quiet = (total_got() == before && !any_busy()) ? quiet + 1 : 0;
        }
    }
};

inline void Player::join() {
    s = aoc::worldd::PlayerSession::join(conn, *rig.world, record, identity(), world_time_at(rig.t), error);
    if (!s) {
        std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
        std::exit(2);
    }
    pawn_ch = s->channel_of(bs::Role::Pawn);
    pc_ch = s->channel_of(bs::Role::PlayerController);
}

inline void Player::flush() {
    for (auto& pkt : conn.flush(rig.t)) {
        const auto r = client.receive(pkt);
        for (const auto& d : r.delivered) got.push_back(d);
    }
}

inline void Player::client_sends(std::vector<fake::Bunch> bunches) {
    const auto pkt = client.packet(std::move(bunches));
    const std::vector<aoc::worldd::InBunch> in = conn.receive(pkt, rig.t);
    s->take_acks(conn);
    for (const aoc::worldd::InBunch& b : in) s->on_bunch(conn, b, rig.t);
    s->end_of_datagram(conn);
}

inline void Player::step_session() {
    s->tick(conn, rig.t);
    flush();
    s->take_acks(conn);
    client_sends({});
}

inline void Player::request(const emit::BunchWriter& rpc, uint32_t channel) {
    client_sends({client.actor(channel, rpc, true)});
    rig.settle();
}

inline void Player::send_now(const emit::BunchWriter& rpc, uint32_t channel) {
    client_sends({client.actor(channel, rpc, true)});
    rig.run(std::chrono::milliseconds(60));
}

inline emit::BunchWriter actor_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    emit::BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    emit::BunchWriter field;
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

inline emit::BunchWriter interact(uint64_t component_object_id) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest r;
    r.interactable.ObjectId = component_object_id;
    r.interactable.ServerId = kServerId;
    r.interactable.Randomizer = kRandomizer;
    r.counter = 1;
    const auto params = repl::interact_params(*rpc, r);
    return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : emit::BunchWriter();
}

inline emit::BunchWriter interrupt_channel() {
    return actor_rpc(class_named("PlayerPawn_C"), "ServerInterruptChannel", {});
}

inline size_t count_in_log(const char* what) {
    const std::string text = g_log.str();
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

inline emit::BunchWriter item_used(int storage, int slot, const std::string& guid_hex) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed");
    repl::RpcParams p;
    const auto g = aoc::worldd::items::guid_of(guid_hex);
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

struct CorpseSeen {
    std::vector<uint32_t> statuses;
    std::vector<std::vector<std::optional<bool>>> can_input;
    std::vector<uint64_t> last_states;
    size_t chains = 0;
};

std::vector<std::pair<size_t, repl::ChannelingData>> channeling_seen(const Player& p, size_t from);
std::vector<std::pair<size_t, std::string>> pc_chain_calls(const Player& p, size_t from);
std::vector<std::string> pc_call_names_since(const Player& p, size_t from);
size_t count_names(const std::vector<std::string>& v, const char* name);
CorpseSeen corpse_seen(const Player& p, uint64_t comp_id, uint32_t channel, size_t from);

inline void play(Player& p) {
    p.rig.settle();
    p.request(actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), p.pawn_ch);
    p.rig.run(std::chrono::seconds(2));
    p.rig.settle();
}

inline bool run_until(
    Rig& rig, const std::function<bool()>& done, std::chrono::milliseconds limit = std::chrono::seconds(30)) {
    for (std::chrono::milliseconds t{0}; t < limit; t += std::chrono::milliseconds(20)) {
        if (done()) return true;
        rig.step();
    }
    return done();
}

inline aoc::worldd::svc::Character record_here(const char* id_hex, const char* name) {
    aoc::worldd::svc::Character c = record_of(id_hex, name);
    c.has_position = true;
    c.position.x = kCx;
    c.position.y = kCy;
    c.position.z = bs::kDefaultSpawn.z;
    return c;
}

inline uint64_t pawn_id(const Player& p) { return p.s->bootstrap().setup().guids.pawn.ObjectId; }

struct HuntRig {
    Rig rig;
    Player* p = nullptr;
    uint64_t wolf = 0;

    bool open(const char* kind = "Hunting_Wolf") {
        if (!rig.open(g_path)) return false;
        p = &rig.add_player(record_here("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
        wolf = rig.spawn(kind, 150, 0);
        if (!wolf) {
            std::printf("  [FAIL] no %s could be made\n", kind);
            return false;
        }
        play(*p);
        return true;
    }

    void interact_with(Player& who, uint64_t npc) {
        who.send_now(interact(rig.component_of(npc)), who.pawn_ch);
    }

    void kill_it(uint64_t npc) {
        rig.kill(npc, pawn_id(*p));
        rig.run(std::chrono::milliseconds(500));
    }

    void settle() { rig.run(std::chrono::milliseconds(500)); }
};

inline bool dead_for_clients(const aoc::worldd::world::WorldActor* a) {
    if (!a) return false;
    for (const repl::Subobject& s : a->actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UNPCCombatInfo") continue;
        const repl::Value* v = s.obj.state.get(*s.obj.schema, "bIsDead");
        const bool* b = v ? v->get_if<bool>() : nullptr;
        return b && *b;
    }
    return false;
}

void run_hunting_session_part2();
