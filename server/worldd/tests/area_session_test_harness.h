// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#pragma once

#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "protocol/repl/storage_rpc.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"
#include "world/interaction_state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace fs = std::filesystem;
namespace cf = content_fixture;
namespace fi = content_fixture::item;
namespace dl = aoc::worldd::dialogue;
using emit::BunchWriter;
using std::chrono::milliseconds;
using std::chrono::seconds;
using svc::ItemContainer;

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const Clock::time_point t0{seconds(5000)};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const ItemContainer Bag = ItemContainer::Backpack;
inline const ItemContainer Worn = ItemContainer::Equipment;
inline constexpr double kCx = bootstrap::kDefaultSpawn.x;
inline constexpr double kCy = bootstrap::kDefaultSpawn.y;

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

inline svc::InventoryItem make_item(ItemContainer c, int slot, int64_t record, uint32_t n, int32_t quantity = 1) {
    svc::InventoryItem it;
    it.container = c;
    it.slot = slot;
    it.item_record_id = record;
    it.instance_guid = guid_n(n);
    it.quantity = quantity;
    it.quality = 1;
    it.durability = 11;
    it.created_at_ms = 1'771'299'474'000ll;
    return it;
}

inline std::vector<svc::InventoryItem> kit() {
    const std::pair<int, int64_t> worn[] = {
        {1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter},
        {16, fi::kFocus}, {17, fi::kBook}, {23, fi::kBag}, {26, fi::kPickaxe}, {29, fi::kAxe},
        {33, fi::kSickle}, {35, fi::kFishingRod}, {38, fi::kBow}};
    std::vector<svc::InventoryItem> out;
    for (const auto& [slot, record] : worn) out.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    out.push_back(make_item(Bag, 0, fi::kSword2H, 200));
    out.push_back(make_item(Bag, 1, fi::kPotion, 201, 5));
    return out;
}

inline svc::Character cyb_record(std::vector<svc::InventoryItem> items = kit()) {
    svc::Character c;
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
    c.items = std::move(items);
    return c;
}

inline bs::LoginIdentity identity() { return {kMachine, kMachine}; }
inline double world_time_at(Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

struct RecordingSink : PositionSink {
    std::vector<std::vector<svc::InventoryItem>> inventories;
    void save(const std::string&, const svc::Position&) override {}
    void save_inventory(const std::string&, std::vector<svc::InventoryItem> items) override { inventories.push_back(std::move(items)); }
    void save_progress(const std::string&, svc::Progress) override {}
};

struct SpawnSpec {
    double x = 0.0;
    double y = 0.0;
    bool gm_vendor = false;
    int64_t vendor_def = 0;
    const char* name = nullptr;
    int64_t dialogue_root = 0;
};

struct Fixture {
    fs::path dir;
    std::optional<content::ContentDb> content;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<PlayerWorld> world;
    std::optional<spawns::CapturedData> captured;

    Fixture() {
        dir = cf::make_temp_dir("worldd_area_session_");
        const fs::path db = dir / "content.db";
        if (!cf::make_db(db)) std::exit(2);
        std::string error;
        content = content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        world = std::make_unique<PlayerWorld>(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
        captured = spawns::CapturedData::load(error);
        if (!captured) {
            std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
            std::exit(2);
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    spawns::Minter minter() { return [this] { return guids.next(); }; }

    const spawns::OpenTemplate* npc_template(const char* leaf = nullptr) const {
        for (const spawns::OpenTemplate& t : captured->opens()) {
            if (t.category != "npc" || captured->template_for(t.archetype_path) != &t) continue;
            if (leaf && t.archetype_path.find(leaf) == std::string::npos) continue;
            if (!leaf && t.find("UInteractableComponent") > 0) continue;
            return &t;
        }
        return nullptr;
    }

    const spawns::OpenTemplate* gatherable_template() const {
        for (const spawns::OpenTemplate& t : captured->opens()) {
            if (t.category == "gatherable") return &t;
        }
        return nullptr;
    }

    uint64_t add_spec(const spawns::OpenTemplate& t, const SpawnSpec& spec) {
        spawns::InstanceParams p;
        p.x = spec.x;
        p.y = spec.y;
        p.z = bootstrap::kDefaultSpawn.z;
        p.server_id = kServerId;
        p.keep_captured_position = t.category == "gatherable";
        if (spec.name) p.name = spec.name;
        p.seed = guids.next().ObjectId;
        std::string error;
        auto w = spawns::instantiate(t, minter(), p, error);
        if (!w) {
            std::printf("  [FAIL] instantiate: %s\n", error.c_str());
            std::exit(2);
        }
        if (t.category == "gatherable") {
            w->x = spec.x;
            w->y = spec.y;
        }
        w->npc.is_gm_vendor = spec.gm_vendor;
        w->npc.vendor_guid = spec.vendor_def;
        w->npc.dialogue_root = spec.dialogue_root;
        if (spec.gm_vendor || spec.dialogue_root != 0) world::allow_inputs(w->actor);
        if (spec.name) w->npc.name = spec.name;
        const uint64_t id = w->id;
        if (!world->registry().add(std::move(*w))) {
            std::printf("  [FAIL] the registry refuses an actor\n");
            std::exit(2);
        }
        return id;
    }

    uint64_t add(const spawns::OpenTemplate& t, double x, double y) {
        return add_spec(t, {x, y, false, 0, nullptr, 0});
    }

    uint64_t add_vendor(const spawns::OpenTemplate& t, double x, double y, int64_t vendor_def, const char* name) {
        return add_spec(t, {x, y, true, vendor_def, name, 0});
    }

    uint64_t add_talker(const spawns::OpenTemplate& t, double x, double y, const char* name, int64_t dialogue_root) {
        return add_spec(t, {x, y, false, 0, name, dialogue_root});
    }
};

inline BunchWriter actor_rpc(const schema::ClassSchema& cls, const char* name, const repl::RpcParams& params) {
    BunchWriter out;
    const schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 && !params.values.empty()) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    if (failed) return out;
    repl::BlockHeader header;
    repl::write_content_block(out, header, field, false);
    return out;
}

struct Harness {
    Fixture fx;
    RecordingSink sink;
    Connection conn{ConnectionConfig{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870}, t0};
    fake::Client client{conn::OuterHeader{0x500c7696u, 0, 5}, conn::CustomField{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d}, 14265, 15870};
    std::unique_ptr<PlayerSession> s;
    Clock::time_point t = t0;
    std::vector<fake::Delivered> got;
    size_t packets = 0, bytes = 0;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;
    double tick_us_total = 0, tick_us_max = 0;
    size_t ticks = 0;
    std::function<void(Clock::time_point)> before_tick;
    svc::Character record;

    explicit Harness(svc::Character r = cyb_record()) : record(std::move(r)) { fx.world->set_position_sink(&sink); }
    virtual ~Harness() = default;

    void start() {
        s = PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(bs::Role::Pawn);
        pc_ch = s->channel_of(bs::Role::PlayerController);
    }

    void flush() {
        for (auto& pkt : conn.flush(t)) {
            bytes += pkt.size();
            const auto r = client.receive(pkt);
            ++packets;
            for (const auto& d : r.delivered) got.push_back(d);
        }
    }

    void client_sends(std::vector<fake::Bunch> bunches) {
        for (const auto& pkt : client.packets(std::move(bunches))) {
            const std::vector<InBunch> in = conn.receive(pkt, t);
            s->take_acks(conn);
            for (const InBunch& b : in) s->on_bunch(conn, b, t);
            s->end_of_datagram(conn);
        }
    }

    void step(milliseconds d = milliseconds(20)) {
        t += d;
        if (before_tick) before_tick(t);
        const auto a = std::chrono::steady_clock::now();
        s->tick(conn, t);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
        tick_us_total += us;
        tick_us_max = std::max(tick_us_max, us);
        ++ticks;
        flush();
        s->take_acks(conn);
        client_sends({});
    }

    void run(milliseconds total) {
        for (milliseconds done{0}; done < total; done += milliseconds(20)) step();
    }

    void settle() {
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }

    void client_request(const BunchWriter& rpc, uint32_t channel) {
        client_sends({client.actor(channel, rpc, true)});
        settle();
    }

    void request_phase_in() {
        client_request(actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), pawn_ch);
    }

    void play() {
        settle();
        request_phase_in();
    }
};

using TestHarness = Harness;

inline BunchWriter interact(uint64_t component_object_id, uint32_t server_id = kServerId, uint32_t randomizer = kRandomizer) {
    const schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    repl::InteractRequest r;
    r.interactable.ObjectId = component_object_id;
    r.interactable.ServerId = server_id;
    r.interactable.Randomizer = randomizer;
    r.counter = 1;
    const auto params = repl::interact_params(*rpc, r);
    return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : BunchWriter();
}

inline void append_pc_block_calls(const fake::Delivered& d, std::vector<repl::RpcCall>& out) {
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return;
    for (const auto& b : *blocks) {
        if (!b.header.is_actor || b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), d.content.data(), b.payload_pos + b.payload_bits, p);
        if (calls) out.insert(out.end(), std::make_move_iterator(calls->begin()), std::make_move_iterator(calls->end()));
    }
}

inline std::vector<repl::RpcCall> pc_calls_since(const Harness& h, size_t from) {
    std::vector<repl::RpcCall> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch || h.got[i].open || h.got[i].close) continue;
        append_pc_block_calls(h.got[i], out);
    }
    return out;
}

inline size_t count_calls(const std::vector<repl::RpcCall>& calls, const char* name) {
    return static_cast<size_t>(std::count_if(calls.begin(), calls.end(), [&](const repl::RpcCall& c) { return c.rpc && std::string(c.rpc->name) == name; }));
}

struct ActorChannels {
    std::set<uint32_t> opened, closed;
    size_t open_chains = 0, close_chains = 0, update_chains = 0;
};

inline ActorChannels actor_channels(const Harness& h, size_t from = 0) {
    ActorChannels a;
    for (size_t i = from; i < h.got.size(); ++i) {
        const fake::Delivered& d = h.got[i];
        if (d.channel == h.pc_ch || d.channel == h.pawn_ch || d.channel == h.s->channel_of(bs::Role::GameState) || d.channel == h.s->channel_of(bs::Role::PlayerState)) continue;
        if (d.open) {
            a.opened.insert(d.channel);
            ++a.open_chains;
        } else if (d.close) {
            a.closed.insert(d.channel);
            ++a.close_chains;
        } else {
            ++a.update_chains;
        }
    }
    return a;
}

inline size_t bag_items(const Harness& h) { return h.s->inventory().count(Bag); }

inline BunchWriter dialogue_choice(const repl::DialogueResponse& response) {
    const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    const schema::RpcSchema* rpc = pc.rpc("ServerSelectDialogueChoice");
    const auto params = repl::dialogue_response_params(*rpc, response);
    return params ? actor_rpc(pc, "ServerSelectDialogueChoice", *params) : BunchWriter();
}

struct DlgBits {
    std::vector<bool> b;
    size_t size() const { return b.size(); }
    bool operator==(const DlgBits& o) const { return b == o.b; }
    DlgBits slice(size_t at, size_t n) const {
        DlgBits r;
        r.b.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
        return r;
    }
    void erase(size_t at, size_t n) {
        b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
    }
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out((b.size() + 7) / 8 + 1, 0);
        for (size_t i = 0; i < b.size(); ++i) out[i >> 3] |= static_cast<uint8_t>(b[i] ? 1 : 0) << (i & 7);
        return out;
    }
    static DlgBits of_hex(const std::string& hex, size_t n) {
        DlgBits r;
        for (size_t i = 0; i < n; ++i) {
            const size_t at = (i >> 3) * 2;
            if (at >= hex.size()) break;
            const auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            const int byte = nib(hex[at]) << 4 | nib(hex[at + 1]);
            r.b.push_back(((byte >> (i & 7)) & 1) != 0);
        }
        return r;
    }
    static DlgBits of_writer(const BunchWriter& w) {
        DlgBits r;
        const auto& bytes = w.bytes();
        for (size_t i = 0; i < w.bit_pos(); ++i) r.b.push_back(((bytes[i >> 3] >> (i & 7)) & 1) != 0);
        return r;
    }
};

inline std::string dialogue_db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    if (env && *env) return env;
    for (const char* p : {"dist/content/content.db", "../content/content.db", "../../dist/content/content.db"}) {
        if (fs::exists(p)) return p;
    }
    return {};
}

inline std::optional<dl::Book> dialogue_book(std::string& why) {
    const std::string path = dialogue_db_path();
    if (path.empty()) {
        why = "no content.db (dist/content/content.db or AOC_CONTENT_DB)";
        return std::nullopt;
    }
    auto db = content::ContentDb::open(path, why);
    if (!db) return std::nullopt;
    auto data = db->dialogue_data(why);
    if (!data) return std::nullopt;
    return dl::Book(std::move(*data));
}

inline int64_t root_with_top(const dl::Book& book, int64_t top) {
    for (const auto& [root, t] : book.data().roots) {
        if (t == top) return root;
    }
    return 0;
}

inline repl::DialogueResponse choice_of(int64_t node, int32_t index) {
    repl::DialogueResponse r;
    r.response_type = static_cast<uint8_t>(repl::DialogueResponseType::Choice);
    r.node = node;
    r.node_type_id = repl::kDialogueNodeTypeId;
    r.choice_index = index;
    return r;
}

struct PcBlock {
    DlgBits bits;
    repl::RepState state;
    std::vector<uint16_t> handles;
};

inline void append_pc_rep_blocks(const fake::Delivered& d, std::vector<PcBlock>& out) {
    const uint8_t* data = d.content.data();
    const auto blocks = repl::read_content_blocks(data, d.content.bit_pos(), 0);
    if (!blocks) return;
    for (const auto& b : *blocks) {
        if (!b.header.is_actor || !b.header.has_rep_layout) continue;
        size_t pos = b.payload_pos;
        const auto st = repl::read_rep_block(class_named("AoCPlayerControllerBP_C"), data, b.payload_pos + b.payload_bits, pos);
        if (!st) continue;
        PcBlock p;
        for (size_t k = 0; k < b.payload_bits; ++k) {
            const size_t at = b.payload_pos + k;
            p.bits.b.push_back(((data[at >> 3] >> (at & 7)) & 1) != 0);
        }
        p.state = *st;
        for (const auto& [handle, v] : st->values()) {
            if (handle >= 141 && handle <= 170) p.handles.push_back(handle);
        }
        if (!p.handles.empty()) out.push_back(std::move(p));
    }
}

inline std::vector<PcBlock> pc_blocks_since(const Harness& h, size_t from) {
    std::vector<PcBlock> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel != h.pc_ch || h.got[i].open || h.got[i].close) continue;
        append_pc_rep_blocks(h.got[i], out);
    }
    return out;
}

struct ClientDialogue {
    int64_t previous_node = 0, previous_choice_node = 0, current_node = 0, shown_node = 0;
    int64_t previous_choice_index = 0;
    bool greeting = false;
    emit::FIntrepidNetworkGUID source;
    struct Line { int64_t node = 0, type_id = 0, index = 0, kind = 0; };
    std::vector<Line> lines;

    static int64_t elem_int(const repl::ElementState& es, uint16_t k) {
        const auto it = es.values.find(k);
        if (it == es.values.end()) return INT64_MIN;
        if (const int64_t* x = it->second.get_if<int64_t>()) return *x;
        if (const uint64_t* u = it->second.get_if<uint64_t>()) return static_cast<int64_t>(*u);
        return INT64_MIN;
    }

    static void apply_line_element(const repl::ElementState& es, Line& line) {
        if (elem_int(es, 1) != INT64_MIN) line.node = elem_int(es, 1);
        if (elem_int(es, 2) != INT64_MIN) line.type_id = elem_int(es, 2);
        if (elem_int(es, 4) != INT64_MIN) line.index = elem_int(es, 4);
        if (elem_int(es, 5) != INT64_MIN) line.kind = elem_int(es, 5);
    }

    void apply(const repl::RepState& s) {
        const auto num = [&](uint16_t h, int64_t& into) {
            const repl::Value* v = s.get(h);
            const int64_t* i = v ? v->get_if<int64_t>() : nullptr;
            if (i) into = *i;
        };
        num(143, previous_node);
        num(146, previous_choice_node);
        num(149, previous_choice_index);
        num(151, current_node);
        num(166, shown_node);
        const repl::Value* sv = s.get(156);
        if (sv && sv->get_if<emit::FIntrepidNetworkGUID>()) source = *sv->get_if<emit::FIntrepidNetworkGUID>();
        const repl::Value* gv = s.get(170);
        if (gv && gv->get_if<bool>()) greeting = *gv->get_if<bool>();
        const repl::Value* lv = s.get(154);
        const auto* a = lv ? lv->get_if<repl::ArrayValue>() : nullptr;
        if (!a) return;
        lines.resize(a->elements.size());
        for (size_t i = 0; i < a->elements.size(); ++i) apply_line_element(a->elements[i], lines[i]);
    }
};

struct DialogueHarness : Harness {
    uint64_t talker = 0;
    int64_t root = 0;
    explicit DialogueHarness(dl::Book book, int64_t root_guid) : Harness(), root(root_guid) {
        fx.world->set_dialogue(std::move(book));
        const spawns::OpenTemplate* vendor_t = fx.captured->template_for(spawns::kGmVendorArchetype);
        if (!vendor_t) {
            std::printf("  [FAIL] no vendor template\n");
            std::exit(2);
        }
        talker = fx.add_talker(*vendor_t, kCx + 800, kCy + 300, "Talker", root);
        start();
    }
};

void run_area_session_suite();
void run_dialogue_session_suite();
void run_quest_session_suite_a();
void run_quest_session_suite_b();
