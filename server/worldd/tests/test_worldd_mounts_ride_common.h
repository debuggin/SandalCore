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

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

inline int g_pass = 0, g_fail = 0;
inline float g_pawn_ts = 0;

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

inline aoc::worldd::bootstrap::LoginIdentity identity() { return {kMachine, kMachine}; }
inline double world_time_at(aoc::worldd::Clock::time_point t) { return std::chrono::duration<double>(t - t0).count(); }

struct RecordingSink : aoc::worldd::PositionSink {
    std::vector<std::vector<aoc::worldd::svc::InventoryItem>> inventories;
    std::vector<aoc::worldd::svc::Position> positions;
    void save(const std::string&, const aoc::worldd::svc::Position& p) override { positions.push_back(p); }
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

    Fixture() {
        dir = content_fixture::make_temp_dir("worldd_mounts_ride_");
        const std::filesystem::path db = dir / "content.db";
        if (!content_fixture::make_db(db)) std::exit(2);
        std::string error;
        content = aoc::worldd::content::ContentDb::open(db.string(), error);
        if (!content) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            std::exit(2);
        }
        real = aoc::worldd::content::ContentDb::open(real_db_path(), error);
        if (!real) {
            std::printf("  [FAIL] the real content.db does not open (%s): %s\n", real_db_path().c_str(), error.c_str());
            std::exit(2);
        }
        world = std::make_unique<aoc::worldd::PlayerWorld>(&*content, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId, guids);
        auto book = aoc::worldd::spawns::MountBook::load(*real, error);
        if (!book) {
            std::printf("  [FAIL] the mounts do not load: %s\n", error.c_str());
            std::exit(2);
        }
        std::string speeds_error;
        if (!book->load_speeds(MOUNT_SPEEDS_PATH, speeds_error)) {
            std::printf("  [FAIL] the mount speeds do not load: %s\n", speeds_error.c_str());
            std::exit(2);
        }
        world->set_mounts(std::move(*book));
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        world.reset();
        content.reset();
        real.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

inline aoc::protocol::emit::BunchWriter actor_rpc(const aoc::protocol::schema::ClassSchema& cls, const char* name, const aoc::protocol::repl::RpcParams& params) {
    aoc::protocol::emit::BunchWriter out;
    const aoc::protocol::schema::RpcSchema* rpc = cls.rpc(name);
    if (!rpc) return out;
    aoc::protocol::emit::BunchWriter field;
    bool failed = false;
    if (aoc::protocol::repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 && !params.values.empty()) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    if (failed) return out;
    aoc::protocol::repl::BlockHeader header;
    aoc::protocol::repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

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

    explicit Harness(aoc::worldd::svc::Character r, const std::function<void(aoc::worldd::PlayerWorld&)>& setup = nullptr) : record(std::move(r)) {
        fx.world->set_position_sink(&sink);
        if (setup) setup(*fx.world);
        s = aoc::worldd::PlayerSession::join(conn, *fx.world, record, identity(), world_time_at(t), error);
        if (!s) {
            std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
            std::exit(2);
        }
        pawn_ch = s->channel_of(aoc::worldd::bootstrap::Role::Pawn);
        pc_ch = s->channel_of(aoc::worldd::bootstrap::Role::PlayerController);
    }

    void flush() {
        for (auto& pkt : conn.flush(t)) {
            const auto r = client.receive(pkt);
            for (const auto& d : r.delivered) {
                got.push_back(d);
                got_at.push_back(t);
            }
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
        fx.world->tick_world(world_time_at(t));
        s->tick(conn, t);
        flush();
        s->take_acks(conn);
        client_sends({});
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
    void play() {
        g_pawn_ts = 0;
        settle();
        client_sends({client.actor(pawn_ch, actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}), true)});
        settle();
        run(std::chrono::milliseconds(400));
    }
    double since(aoc::worldd::Clock::time_point from) const { return std::chrono::duration<double>(t - from).count(); }
    const aoc::worldd::world::WorldActor* mount() const { return s->mounts() ? s->mounts()->mount() : nullptr; }
    uint32_t mount_ch() const {
        const aoc::worldd::world::WorldActor* m = mount();
        return m && s->actor_view() ? s->actor_view()->channel_of(m->id) : 0;
    }
};

inline aoc::protocol::emit::BunchWriter item_used(int storage, int slot, const std::string& guid_hex) {
    const aoc::protocol::schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const aoc::protocol::schema::RpcSchema* rpc = pawn.rpc("ServerItemUsed");
    aoc::protocol::repl::RpcParams p;
    const auto g = aoc::worldd::items::guid_of(guid_hex);
    const aoc::protocol::repl::GuidValue gv = g ? *g : aoc::protocol::repl::GuidValue{};
    aoc::protocol::repl::set_rpc_param(*rpc, p, "Storage", aoc::protocol::repl::Value::u64(static_cast<uint64_t>(storage)));
    aoc::protocol::repl::set_rpc_param(*rpc, p, "Slot", aoc::protocol::repl::Value::i64(slot));
    aoc::protocol::repl::set_rpc_param(*rpc, p, "ExpectedItem.A", aoc::protocol::repl::Value::i64(static_cast<int32_t>(gv.a)));
    aoc::protocol::repl::set_rpc_param(*rpc, p, "ExpectedItem.B", aoc::protocol::repl::Value::i64(static_cast<int32_t>(gv.b)));
    aoc::protocol::repl::set_rpc_param(*rpc, p, "ExpectedItem.C", aoc::protocol::repl::Value::i64(static_cast<int32_t>(gv.c)));
    aoc::protocol::repl::set_rpc_param(*rpc, p, "ExpectedItem.D", aoc::protocol::repl::Value::i64(static_cast<int32_t>(gv.d)));
    aoc::protocol::repl::set_rpc_param(*rpc, p, "RequestedEquipSlot", aoc::protocol::repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
    return actor_rpc(pawn, "ServerItemUsed", p);
}

struct TriggerItemArg {
    int slot = -1;
    const aoc::worldd::svc::InventoryItem* item = nullptr;
    uint8_t location = 3;
    int32_t animation_index = 0;
};

inline aoc::protocol::emit::BunchWriter trigger_on(Harness& h, const aoc::protocol::emit::FIntrepidNetworkGUID& component, int64_t ability, int32_t client_instance, TriggerItemArg item_arg = {}) {
    const aoc::protocol::schema::ClassSchema& cls = class_named("UAoCAbilityComponent");
    const aoc::protocol::schema::RpcSchema* rpc = cls.rpc("ServerTriggerAbility");
    aoc::protocol::repl::RpcParams p;
    bool ok = rpc && aoc::protocol::repl::set_rpc_param(*rpc, p, "AbilityGuid", aoc::protocol::repl::Value::i64(ability));
    if (ok && client_instance != 0) ok = aoc::protocol::repl::set_rpc_param(*rpc, p, "ClientInstanceId", aoc::protocol::repl::Value::i64(client_instance));
    if (ok && item_arg.animation_index != 0) ok = aoc::protocol::repl::set_rpc_param(*rpc, p, "AnimationIndex", aoc::protocol::repl::Value::i64(item_arg.animation_index));
    ok = ok && aoc::protocol::repl::set_rpc_param(*rpc, p, "InputVector", aoc::protocol::repl::Value(aoc::protocol::repl::Vec2d{1, 0}));
    if (ok && item_arg.slot >= 0) {
        const auto inst = item_arg.item ? aoc::worldd::items::to_instance(*item_arg.item) : aoc::protocol::repl::ItemInstance{};
        ok = aoc::protocol::repl::set_rpc_param(*rpc, p, "ItemData.Slot", aoc::protocol::repl::Value::i64(item_arg.slot)) &&
             aoc::protocol::repl::set_rpc_param(*rpc, p, "ItemData.ItemInstance", aoc::protocol::repl::Value(inst)) &&
             aoc::protocol::repl::set_rpc_param(*rpc, p, "ItemData.Location", aoc::protocol::repl::Value::u64(item_arg.location));
    }
    if (!ok) {
        std::printf("  [FAIL] the trigger's parameters do not fit\n");
        return aoc::protocol::emit::BunchWriter();
    }
    aoc::protocol::emit::BunchWriter out;
    aoc::protocol::emit::BunchWriter field;
    bool failed = false;
    if (aoc::protocol::repl::write_rpc_field(cls, *rpc, p, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] ServerTriggerAbility does not write\n");
        return out;
    }
    aoc::protocol::repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = component;
    aoc::protocol::repl::write_content_block(out, header, field, /*server_form=*/false);
    (void)h;
    return out;
}

inline aoc::protocol::emit::BunchWriter summon_trigger(Harness& h, int32_t client_instance, int slot, const aoc::worldd::svc::InventoryItem* item, uint8_t location = 3) {
    return trigger_on(h, h.s->bootstrap().setup().guids.ability, 108436548376185ll, client_instance, TriggerItemArg{slot, item, location, 0});
}

inline aoc::protocol::emit::BunchWriter desummon_rpc() { return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerDesummonMount", {}); }
inline aoc::protocol::emit::BunchWriter dismount_rpc() { return actor_rpc(class_named("AoCPlayerControllerBP_C"), "ServerDismountInteraction", {}); }

inline aoc::protocol::emit::BunchWriter possession_ack(const aoc::protocol::emit::FIntrepidNetworkGUID& actor) {
    const aoc::protocol::schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
    aoc::protocol::repl::RpcParams p;
    aoc::protocol::repl::set_rpc_param(*pc.rpc("ServerAcknowledgePossession"), p, "P", aoc::protocol::repl::Value(actor));
    return actor_rpc(pc, "ServerAcknowledgePossession", p);
}

inline aoc::protocol::emit::FIntrepidNetworkGUID component_of(const aoc::worldd::world::WorldActor& m, uint64_t object_id) {
    aoc::protocol::emit::FIntrepidNetworkGUID g = m.actor.guid;
    g.ObjectId = object_id;
    return g;
}

inline aoc::protocol::emit::BunchWriter interact_with(const aoc::protocol::emit::FIntrepidNetworkGUID& component) {
    const aoc::protocol::schema::ClassSchema& pawn = class_named("PlayerPawn_C");
    const aoc::protocol::schema::RpcSchema* rpc = pawn.rpc("ServerInteractWithInteractableComponent");
    aoc::protocol::repl::InteractRequest r;
    r.interactable = component;
    r.counter = 1;
    r.zone = 2;
    const auto params = aoc::protocol::repl::interact_params(*rpc, r);
    return params ? actor_rpc(pawn, "ServerInteractWithInteractableComponent", *params) : aoc::protocol::emit::BunchWriter();
}

inline aoc::protocol::repl::NetworkMove move_at(float ts, double x, double y, double z, uint16_t yaw = 0) {
    aoc::protocol::repl::NetworkMove m;
    m.time_stamp = ts;
    m.acceleration = aoc::protocol::repl::quantize_vector(10, 0, 0, 0);
    m.location = aoc::protocol::repl::quantize_vector(100, x, y, z);
    m.control_rotation.yaw = yaw;
    m.movement_mode = aoc::protocol::repl::kMoveWalking;
    return m;
}

inline aoc::protocol::repl::NetworkMove falling_move_at(float ts, double x, double y, double z, uint16_t yaw = 0) {
    aoc::protocol::repl::NetworkMove m = move_at(ts, x, y, z, yaw);
    m.movement_mode = aoc::protocol::repl::kMoveFalling;
    return m;
}

inline aoc::protocol::emit::BunchWriter client_move(const char* cls_name, const aoc::protocol::repl::NetworkMove& m) {
    const aoc::protocol::schema::ClassSchema& cls = class_named(cls_name);
    const aoc::protocol::schema::NetField* f = cls.field("ServerMovePacked");
    aoc::protocol::emit::BunchWriter out;
    if (!f) return out;
    aoc::protocol::repl::MovePacked p;
    p.new_move = m;
    aoc::protocol::emit::BunchWriter params;
    if (!aoc::protocol::repl::write_move_packed(params, p)) return out;
    aoc::protocol::emit::BunchWriter field;
    if (aoc::protocol::repl::write_field(cls, f->index, params, field) == 0) return out;
    aoc::protocol::repl::BlockHeader header;
    aoc::protocol::repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

inline void move_pawn(Harness& h, const aoc::protocol::repl::NetworkMove& m) { h.client_sends({h.client.actor(h.pawn_ch, client_move("PlayerPawn_C", m), false)}); }
inline void move_mount(Harness& h, const aoc::protocol::repl::NetworkMove& m) {
    const uint32_t ch = h.s->mount_channel();
    h.client_sends({h.client.actor(ch ? ch : 999, client_move("ANPCCharacter", m), false)});
}

inline void summon_with(Harness& h, int slot, int32_t client_instance = 7) {
    const aoc::worldd::svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    const std::string guid = item ? item->instance_guid : std::string();
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, guid), true), h.client.actor(h.pawn_ch, summon_trigger(h, client_instance, slot, item), true)});
}

inline void desummon_with(Harness& h, int slot) {
    const aoc::worldd::svc::InventoryItem* item = h.s->inventory().at(Bag, slot);
    h.client_sends({h.client.actor(h.pawn_ch, item_used(3, slot, item ? item->instance_guid : std::string()), true), h.client.actor(h.pc_ch, desummon_rpc(), true)});
}

inline void get_on(Harness& h) {
    const aoc::worldd::world::WorldActor* m = h.mount();
    if (!m) {
        std::printf("  [FAIL] (no mount to get on)\n");
        return;
    }
    h.client_sends({h.client.actor(h.pawn_ch, interact_with(component_of(*m, m->npc.interact_component)), true)});
}

inline void get_off(Harness& h) { h.client_sends({h.client.actor(h.pc_ch, dismount_rpc(), true)}); }

inline uint16_t yaw16_of(double degrees) { return static_cast<uint16_t>(static_cast<int>(std::lround(degrees * 65536.0 / 360.0)) & 0xFFFF); }

inline void walk_pawn_to(Harness& h, double x, double y, double speed = 600.0) {
    const aoc::worldd::GoodPosition g = h.s->movement().good();
    const double dx = x - g.x, dy = y - g.y, dist = std::hypot(dx, dy);
    const int steps = std::max(1, static_cast<int>(std::ceil(dist / (speed * 0.2))));
    for (int i = 1; i <= steps; ++i) {
        g_pawn_ts += 0.2f;
        move_pawn(h, move_at(g_pawn_ts, g.x + dx * i / steps, g.y + dy * i / steps, g.z, yaw16_of(g.yaw_degrees)));
        h.step(std::chrono::milliseconds(200));
    }
}

inline void walk_up_to_mount(Harness& h) {
    const aoc::worldd::world::WorldActor* m = h.mount();
    if (!m) return;
    const aoc::worldd::GoodPosition g = h.s->movement().good();
    const double dx = m->x - g.x, dy = m->y - g.y, dist = std::hypot(dx, dy);
    if (dist <= 250) return;
    walk_pawn_to(h, g.x + dx * (dist - 250) / dist, g.y + dy * (dist - 250) / dist);
}

inline bool ride_on(Harness& h, int slot = kMountSlot) {
    summon_with(h, slot);
    h.run(std::chrono::milliseconds(1900));
    if (!h.mount() || h.s->mounts()->phase() != aoc::worldd::MountController::Phase::Out) return false;
    walk_up_to_mount(h);
    get_on(h);
    h.run(std::chrono::milliseconds(200));
    return h.s->mounted();
}

inline std::vector<aoc::protocol::repl::RpcCall> calls_of(const fake::Delivered& d, const char* cls) {
    std::vector<aoc::protocol::repl::RpcCall> out;
    const auto blocks = aoc::protocol::repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return out;
    for (const auto& b : *blocks) {
        if (b.header.has_rep_layout) continue;
        size_t p = b.payload_pos;
        auto calls = aoc::protocol::repl::read_rpc_fields(class_named(cls), d.content.data(), b.payload_pos + b.payload_bits, p);
        if (calls) out.insert(out.end(), std::make_move_iterator(calls->begin()), std::make_move_iterator(calls->end()));
    }
    return out;
}

using States = std::map<uint64_t, aoc::protocol::repl::ObjectState>;
inline States states_of(const fake::Delivered& d, const aoc::protocol::repl::Actor& actor) {
    States out;
    const auto blocks = aoc::protocol::repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
    if (!blocks) return out;
    for (const aoc::protocol::repl::BlockView& b : *blocks) {
        const aoc::protocol::schema::ClassSchema* cls = nullptr;
        uint64_t id = 0;
        if (b.header.is_actor) {
            cls = actor.obj.schema;
        } else {
            const aoc::protocol::repl::Subobject* sub = actor.find(b.header.object);
            cls = sub ? sub->obj.schema : nullptr;
            id = b.header.object.ObjectId;
        }
        if (!cls) continue;
        aoc::protocol::emit::BunchWriter w;
        w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
        auto st = aoc::protocol::repl::load_object_state(*cls, w.data(), b.payload_bits, b.header.has_rep_layout);
        if (st) out.emplace(id, std::move(*st));
    }
    return out;
}

inline std::optional<int64_t> int_of(const aoc::protocol::repl::ObjectState& o, const char* path) {
    const aoc::protocol::repl::Value* v = o.schema ? o.state.get(*o.schema, path) : nullptr;
    if (!v) return std::nullopt;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    if (const auto* b = v->get_if<bool>()) return *b ? 1 : 0;
    return std::nullopt;
}

inline std::optional<aoc::protocol::emit::FIntrepidNetworkGUID> object_of(const aoc::protocol::repl::ObjectState& o, const char* path) {
    const aoc::protocol::repl::Value* v = o.schema ? o.state.get(*o.schema, path) : nullptr;
    if (!v) return std::nullopt;
    if (const auto* g = v->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) return *g;
    return std::nullopt;
}

inline std::vector<const fake::Delivered*> chains_on(const Harness& h, uint32_t channel, size_t from) {
    std::vector<const fake::Delivered*> out;
    for (size_t i = from; i < h.got.size(); ++i) {
        if (h.got[i].channel == channel && !h.got[i].open && !h.got[i].close) out.push_back(&h.got[i]);
    }
    return out;
}

inline size_t acks_on(const Harness& h, uint32_t channel, size_t from, const char* cls = "ANPCCharacter") {
    size_t n = 0;
    for (const fake::Delivered* d : chains_on(h, channel, from)) {
        for (const aoc::protocol::repl::RpcCall& c : calls_of(*d, cls)) n += c.rpc && std::string(c.rpc->name) == "ClientMoveResponsePacked";
    }
    return n;
}

inline size_t corrections_on(const Harness& h, uint32_t channel, size_t from, const char* cls = "ANPCCharacter") {
    size_t n = 0;
    for (const fake::Delivered* d : chains_on(h, channel, from)) {
        for (const aoc::protocol::repl::RpcCall& c : calls_of(*d, cls)) n += c.rpc && std::string(c.rpc->name) == "ClientAdjustPosition";
    }
    return n;
}

inline size_t mount_count(const Harness& h) { return h.fx.world->registry().count(aoc::worldd::world::ActorKind::Mount); }
inline bool within(double got, double want, double tol) { return got >= 0 && std::fabs(got - want) <= tol; }

struct Rider {
    double x = 0, y = 0, z = 0;
    float  ts = 0;
    uint16_t yaw = 0;
};

inline Rider start_riding(Harness& h) {
    Rider r;
    const aoc::worldd::GoodPosition g = h.s->movement().good();
    r.x = g.x;
    r.y = g.y;
    r.z = g.z;
    r.yaw = yaw16_of(g.yaw_degrees);
    return r;
}

inline void ride_step_mode(Harness& h, Rider& r, aoc::protocol::repl::Vec3d delta, uint8_t mode, uint16_t pitch = 0) {
    r.ts += 0.1f;
    r.x += delta.x;
    r.y += delta.y;
    r.z += delta.z;
    aoc::protocol::repl::NetworkMove m = move_at(r.ts, r.x, r.y, r.z, r.yaw);
    m.movement_mode = mode;
    m.control_rotation.pitch = pitch;
    move_mount(h, m);
    h.step(std::chrono::milliseconds(100));
}

inline void ride_step(Harness& h, Rider& r, double dx, double dy, double dz = 0) {
    ride_step_mode(h, r, {dx, dy, dz}, aoc::protocol::repl::kMoveWalking, 0);
}

inline aoc::protocol::emit::FIntrepidNetworkGUID ability_component_of(const aoc::worldd::world::WorldActor& m) {
    const aoc::protocol::repl::Subobject* a = aoc::worldd::spawns::sub_of(m, "UAoCAbilityComponent");
    return a ? a->guid : aoc::protocol::emit::FIntrepidNetworkGUID{};
}

inline std::vector<aoc::protocol::repl::ElementState> items_of(const aoc::worldd::world::WorldActor& m, const char* cls, const char* field) {
    std::vector<aoc::protocol::repl::ElementState> out;
    const aoc::protocol::repl::Subobject* s = aoc::worldd::spawns::sub_of(m, cls);
    const aoc::protocol::schema::NetField* f = s ? s->obj.schema->field(field) : nullptr;
    const aoc::protocol::repl::FastArrayState* a = f ? s->obj.arrays.find(f->index) : nullptr;
    if (a) {
        for (const aoc::protocol::repl::FastArrayItem& it : a->items()) out.push_back(it.values);
    }
    return out;
}

inline std::optional<double> mult_of(const aoc::worldd::world::WorldActor& m) {
    return aoc::worldd::spawns::stat_value(m, aoc::worldd::spawns::kStatMoveSpeedMult);
}

void run_mounts_ride_part2();
void run_mounts_ride_part3();
void run_mounts_ride_part4();
