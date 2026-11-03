// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "capture_mount_session.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "protocol/repl/block_reader.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

inline int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline const std::string kCyb = "bbec5c86eb101fe5f38c87f0fa22e7d0";
inline const std::string kMachine = "DESKTOP-EXAMPLE1-US-0123456789ABCDEF0123456789ABCDEF";
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(5000)};
inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const aoc::worldd::svc::ItemContainer Bag = aoc::worldd::svc::ItemContainer::Backpack;
inline const aoc::worldd::svc::ItemContainer Worn = aoc::worldd::svc::ItemContainer::Equipment;

inline const aoc::protocol::schema::ClassSchema& class_named(const char* name) {
    const aoc::protocol::schema::ClassSchema* c = aoc::protocol::schema::gen::find_class_schema(name);
    if (!c) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
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

inline constexpr int kMountSlot = 5, kOtherMountSlot = 6, kLockedSlot = 7, kAquaticSlot = 8, kFlyingSlot = 9, kPotionSlot = 1, kSwordSlot = 0;
inline int64_t g_mount_record = 0, g_other_mount_record = 0, g_aquatic_record = 0, g_flying_record = 0;

inline std::vector<aoc::worldd::svc::InventoryItem> kit() {
    namespace fi = content_fixture::item;
    const std::pair<int, int64_t> worn[] = {{1, fi::kHelmet}, {2, fi::kChest}, {5, fi::kLegs}, {6, fi::kFeet}, {15, fi::kScepter}, {16, fi::kFocus}, {17, fi::kBook},
                                             {23, fi::kBag}, {26, fi::kPickaxe}, {29, fi::kAxe}, {33, fi::kSickle}, {35, fi::kFishingRod}, {38, fi::kBow}};
    std::vector<aoc::worldd::svc::InventoryItem> out;
    for (const auto& [slot, record] : worn) out.push_back(make_item(Worn, slot, record, 100 + static_cast<uint32_t>(slot)));
    out.push_back(make_item(Bag, kSwordSlot, fi::kSword2H, 200));
    out.push_back(make_item(Bag, kPotionSlot, fi::kPotion, 201, 5));
    aoc::worldd::svc::InventoryItem a = make_item(Bag, kMountSlot, g_mount_record, 300);
    a.persisted_health = 3572;
    a.persisted_mana = 1;
    out.push_back(a);
    out.push_back(make_item(Bag, kOtherMountSlot, g_other_mount_record, 301));
    aoc::worldd::svc::InventoryItem locked = make_item(Bag, kLockedSlot, g_mount_record, 302);
    locked.lock_expiration = 4'000'000'000ll;
    out.push_back(locked);
    out.push_back(make_item(Bag, kAquaticSlot, g_aquatic_record, 303));
    out.push_back(make_item(Bag, kFlyingSlot, g_flying_record, 304));
    return out;
}

inline aoc::worldd::svc::Character cyb_record(std::vector<aoc::worldd::svc::InventoryItem> items = kit()) {
    aoc::worldd::svc::Character c;
    c.id = kCyb; c.account_id = 3; c.name = "cyb"; c.race_id = 0; c.gender_id = 0;
    c.class_id = 2; c.archetype_id = 17747; c.class_name = "Bard";
    c.appearance_json = cyb::kAppearanceJsonb; c.level = 1; c.items = std::move(items);
    return c;
}

inline aoc::worldd::bootstrap::LoginIdentity identity() { return {kMachine, kMachine}; }
inline double world_time_at(aoc::worldd::Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

struct RecordingSink : aoc::worldd::PositionSink {
    std::vector<std::vector<aoc::worldd::svc::InventoryItem>> inventories;
    void save(const std::string&, const aoc::worldd::svc::Position&) override {}
    void save_inventory(const std::string&, std::vector<aoc::worldd::svc::InventoryItem> items) override { inventories.push_back(std::move(items)); }
    void save_progress(const std::string&, aoc::worldd::svc::Progress) override {}
};

inline std::string real_db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : MOUNT_CONTENT_DB_PATH;
}

struct Fixture {
    std::filesystem::path dir;
    std::optional<aoc::worldd::content::ContentDb> content;
    std::optional<aoc::worldd::content::ContentDb> real;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;

    explicit Fixture(bool real_world = false) {
        namespace cf = content_fixture;
        dir = cf::make_temp_dir("worldd_mounts_session_");
        const std::filesystem::path db = dir / "content.db";
        if (!cf::make_db(db)) std::exit(2);
        std::string error;
        content = aoc::worldd::content::ContentDb::open(db.string(), error);
        if (!content) { std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str()); std::exit(2); }
        real = aoc::worldd::content::ContentDb::open(real_db_path(), error);
        if (!real) { std::printf("  [FAIL] the real content.db does not open (%s): %s\n", real_db_path().c_str(), error.c_str()); std::exit(2); }
        world = std::make_unique<aoc::worldd::PlayerWorld>(real_world ? &*real : &*content, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId, guids);
        auto book = aoc::worldd::spawns::MountBook::load(*real, error);
        if (!book) { std::printf("  [FAIL] the mounts do not load: %s\n", error.c_str()); std::exit(2); }
        for (const aoc::worldd::content::MountRow& r : book->data().mounts) {
            if (r.item_name == "Mount_Grounded_TerrorBird_0") g_mount_record = r.item;
            if (r.item_name == "Mount_Grounded_Horse_0") g_other_mount_record = r.item;
            if (r.kind == "aquatic" && !g_aquatic_record) g_aquatic_record = r.item;
            if (r.kind == "flying" && !g_flying_record) g_flying_record = r.item;
        }
        if (!g_mount_record || !g_other_mount_record) { std::printf("  [FAIL] the data has no terror bird or horse\n"); std::exit(2); }
        world->set_mounts(std::move(*book));
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset(); content.reset(); real.reset();
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
    std::vector<aoc::worldd::Clock::time_point> got_at;
    std::string error;
    uint32_t pawn_ch = 0, pc_ch = 0;
    aoc::worldd::svc::Character record;

    explicit Harness(aoc::worldd::svc::Character r, bool real_world = false) : fx(real_world), record(std::move(r)) {
        fx.world->set_position_sink(&sink);
        s = aoc::worldd::PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) { std::printf("  [FAIL] the session does not join: %s\n", error.c_str()); std::exit(2); }
        pawn_ch = s->channel_of(aoc::worldd::bootstrap::Role::Pawn);
        pc_ch = s->channel_of(aoc::worldd::bootstrap::Role::PlayerController);
    }

    void flush() {
        for (auto& pkt : conn.flush(t))
            for (const auto& d : client.receive(pkt).delivered) { got.push_back(d); got_at.push_back(t); }
    }
    void client_sends(std::vector<fake::Bunch> bunches) {
        const auto pkt = client.packet(std::move(bunches));
        const std::vector<aoc::worldd::InBunch> in = conn.receive(pkt, t);
        s->take_acks(conn);
        for (const aoc::worldd::InBunch& b : in) s->on_bunch(conn, b, t);
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
        for (int quiet = 0, i = 0; quiet < 5 && i < 600; ++i) {
            const size_t before = got.size();
            step();
            quiet = (got.size() == before && s->join_pending() == 0 && s->replies_pending() == 0) ? quiet + 1 : 0;
        }
    }
    inline void play();
    double since(aoc::worldd::Clock::time_point from) const { return std::chrono::duration<double>(t - from).count(); }
};

inline aoc::protocol::emit::BunchWriter actor_rpc(const aoc::protocol::schema::ClassSchema& cls, const char* name, const aoc::protocol::repl::RpcParams& params) {
    namespace repl = aoc::protocol::repl;
    aoc::protocol::emit::BunchWriter out, field;
    const aoc::protocol::schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
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

inline void Harness::play() {
    settle();
    client_sends({client.actor(pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
    settle();
    run(std::chrono::milliseconds(400));
}

inline aoc::protocol::emit::BunchWriter item_used(int storage, int slot, const std::string& guid_hex) {
    namespace repl = aoc::protocol::repl;
    const aoc::protocol::schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const aoc::protocol::schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed");
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

inline aoc::protocol::emit::BunchWriter summon_trigger(Harness& h, int32_t client_instance, int slot, const aoc::worldd::svc::InventoryItem* item, uint8_t location = 3) {
    namespace repl = aoc::protocol::repl;
    const aoc::protocol::schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const aoc::protocol::schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    repl::RpcParams p;
    bool ok = rpc && repl::set_rpc_param(*rpc, p, "AbilityGuid", repl::Value::i64(108436548376185ll));
    if (ok && client_instance != 0) ok = repl::set_rpc_param(*rpc, p, "ClientInstanceId", repl::Value::i64(client_instance));
    ok = ok && repl::set_rpc_param(*rpc, p, "InputVector", repl::Value(repl::Vec2d{1, 0}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Slot", repl::Value::i64(slot));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", repl::Value(item ? aoc::worldd::items::to_instance(*item) : repl::ItemInstance{}));
    ok = ok && repl::set_rpc_param(*rpc, p, "ItemData.Location", repl::Value::u64(location));
    if (!ok) { std::printf("  [FAIL] the trigger's parameters do not fit\n"); return aoc::protocol::emit::BunchWriter(); }
    aoc::protocol::emit::BunchWriter out, field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, p, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] ServerTriggerAbility does not write\n");
        return out;
    }
    repl::BlockHeader header;
    header.is_actor = false; header.has_rep_layout = false; header.object = h.s->bootstrap().setup().guids.ability;
    repl::write_content_block(out, header, field, false);
    return out;
}

inline aoc::protocol::emit::BunchWriter desummon_rpc() { return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerDesummonMount", {}); }

inline void summon_with(Harness& h, int slot, int32_t client_instance = 7) {
    const aoc::worldd::svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    const std::string guid = item ? item->instance_guid : std::string();
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true), h.client.actor(h.pawn_ch, summon_trigger(h, client_instance, slot, item), true)});
}

inline void desummon_with(Harness& h, int slot) {
    const aoc::worldd::svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, item ? item->instance_guid : std::string()), true), h.client.actor(h.pc_ch, desummon_rpc(), true)});
}

struct Saw {
    double ability_item = -1, bar = -1, open = -1, summoning_off = -1, flags = -1, close = -1, item_end = -1, callout = -1;
    uint32_t mount_channel = 0;
    size_t opens = 0, closes = 0, updates_on_mount = 0;
};

inline bool is_player_channel(const Harness& h, uint32_t ch) {
    namespace bs = aoc::worldd::bootstrap;
    return ch == h.pc_ch || ch == h.pawn_ch || ch == h.s->channel_of(bs::Role::GameState) || ch == h.s->channel_of(bs::Role::PlayerState);
}

inline std::vector<aoc::protocol::repl::RpcCall> calls_of(const Harness& h, const fake::Delivered& d, const char* cls) {
    namespace repl = aoc::protocol::repl;
    std::vector<repl::RpcCall> out;
    const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return out;
    for (const auto& b : *blocks) {
        if (b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        if (auto calls = repl::read_rpc_fields(class_named(cls), d.content.data(), b.payload_pos + b.payload_bits, p))
            for (auto& c : *calls) out.push_back(std::move(c));
    }
    (void)h;
    return out;
}

struct AbilityItem {
    int64_t ability = 0, instance = 0, animation_counter = 0;
    uint64_t montage = 0;
};

inline std::vector<AbilityItem> ability_items(const Harness& h) {
    namespace repl = aoc::protocol::repl;
    std::vector<AbilityItem> out;
    const repl::Actor& pawn = h.s->bootstrap().pawn();
    const repl::Subobject* ab = pawn.find(h.s->bootstrap().setup().guids.ability);
    if (!ab) return out;
    const repl::FastArrayState* arr = ab->obj.arrays.find(ab->obj.schema->field("ReplicatedActiveAbilities")->index);
    if (!arr) return out;
    for (const repl::FastArrayItem& it : arr->items()) {
        AbilityItem a;
        const auto& v = it.values.values;
        if (const auto f = v.find(1); f != v.end() && f->second.get_if<int64_t>()) a.ability = *f->second.get_if<int64_t>();
        if (const auto f = v.find(2); f != v.end() && f->second.get_if<int64_t>()) a.instance = *f->second.get_if<int64_t>();
        if (const auto f = v.find(11); f != v.end() && f->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>())
            a.montage = f->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()->ObjectId;
        if (const auto f = v.find(15); f != v.end() && f->second.get_if<int64_t>()) a.animation_counter = *f->second.get_if<int64_t>();
        out.push_back(a);
    }
    return out;
}

inline size_t summon_items(const Harness& h, bool with_montage = false) {
    size_t n = 0;
    for (const AbilityItem& a : ability_items(h))
        if (a.ability == 108436548376185ll && (!with_montage || a.montage != 0)) ++n;
    return n;
}

inline size_t mount_count(const Harness& h) { return h.fx.world->registry().count(aoc::worldd::world::ActorKind::Mount); }

inline void watch_delivered(const Harness& h, const fake::Delivered& d, double now, Saw& saw) {
    if (d.channel == h.pc_ch && !d.open && !d.close) {
        for (const aoc::protocol::repl::RpcCall& c : calls_of(h, d, "AoCPlayerControllerBP_C"))
            if (c.rpc && std::string(c.rpc->name) == "ClientClearActionbarsForInstigators" && saw.bar < 0) saw.bar = now;
    }
    if (is_player_channel(h, d.channel)) return;
    if (d.open) {
        ++saw.opens;
        if (saw.open < 0) { saw.open = now; saw.mount_channel = d.channel; }
    } else if (d.close) {
        ++saw.closes;
        if (saw.close < 0) saw.close = now;
    } else if (d.channel == saw.mount_channel) {
        ++saw.updates_on_mount;
    }
}

inline void watch_mount_subobjects(const aoc::worldd::world::WorldActor& m, double now, Saw& saw) {
    for (const aoc::protocol::repl::Subobject& sub : m.actor.subobjects) {
        if (!sub.obj.schema || std::string(sub.obj.schema->name) != "UMountComponent") continue;
        const aoc::protocol::repl::Value* v = sub.obj.state.get(*sub.obj.schema, "bIsSummoning");
        if (saw.open >= 0 && saw.summoning_off < 0 && v && v->get_if<bool>() && !*v->get_if<bool>()) saw.summoning_off = now;
        const aoc::protocol::repl::Value* f = sub.obj.state.get(*sub.obj.schema, "bIsDesummoning");
        if (saw.flags < 0 && f && f->get_if<bool>() && *f->get_if<bool>()) saw.flags = now;
    }
}

inline Saw watch(Harness& h, aoc::worldd::Clock::time_point from, std::chrono::milliseconds ms, Saw saw = {}) {
    for (std::chrono::milliseconds done{0}; done < ms; done += std::chrono::milliseconds(20)) {
        const size_t before = h.got.size();
        h.step();
        const double now = h.since(from);
        for (size_t i = before; i < h.got.size(); ++i) watch_delivered(h, h.got[i], now, saw);
        if (saw.ability_item < 0 && summon_items(h, true) > 0) saw.ability_item = now;
        if (saw.ability_item >= 0 && saw.item_end < 0 && summon_items(h, true) == 0 && summon_items(h) > 0) saw.item_end = now;
        if (saw.callout < 0 && h.s->bootstrap().callouts() > 0) saw.callout = now;
        if (const aoc::worldd::world::WorldActor* m = h.s->mounts() ? h.s->mounts()->mount() : nullptr)
            watch_mount_subobjects(*m, now, saw);
    }
    return saw;
}

inline bool within(double got, double want, double tol) { return got >= 0 && std::fabs(got - want) <= tol; }

void run_mounts_session_goldens_tests();
