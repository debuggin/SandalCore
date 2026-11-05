// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/constants.h"
#include "net/tick_loop.h"
#include "protocol/schema/generated/index.h"
#include "session/actor_view.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace aoc::worldd;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::FIntrepidNetworkGUID;
using std::chrono::milliseconds;
using std::chrono::seconds;

inline int g_pass = 0;
inline int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline const conn::OuterHeader kOuter{0x500c7696u, 0, 5};
inline const conn::CustomField kCf{0x38, 0x21, 0xc7, 0x0d, 0x43, 0x5d};
inline const Clock::time_point t0{seconds(1000)};

inline ConnectionConfig server_config() {
    ConnectionConfig c;
    c.outer = kOuter;
    c.custom_field = kCf;
    c.server_initial_seq = 14265;
    c.client_initial_seq = 15870;
    return c;
}

inline ConnectionConfig client_config() {
    ConnectionConfig c = server_config();
    std::swap(c.server_initial_seq, c.client_initial_seq);
    return c;
}

struct Seen {
    uint32_t channel;
    bool     open, close, reliable;
    uint32_t reason;
    size_t   bits;
};

struct Pair {
    Connection s{server_config(), t0};
    Connection cl{client_config(), t0};
    Clock::time_point t = t0;
    std::vector<Seen> seen;
    std::vector<AckEvent> events;
    size_t packets = 0, bytes = 0;

    void step(ActorView* view = nullptr, milliseconds dt = milliseconds(60)) {
        t += dt;
        auto pkts = s.flush(t);
        packets += pkts.size();
        for (const auto& p : pkts) {
            bytes += p.size();
            for (auto& b : cl.receive(p, t)) {
                if (b.channel == 0) continue;
                seen.push_back({b.channel, b.open, b.close, b.reliable, b.close_reason, b.bits});
            }
        }
        t += dt;
        for (const auto& p : cl.flush(t)) s.receive(p, t);
        for (const AckEvent& e : s.take_ack_events()) {
            events.push_back(e);
            if (view) view->on_ack(e);
        }
    }
};

inline const schema::ClassSchema& schema_named(const char* name) {
    const schema::ClassSchema* s = schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

inline FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 60, uint32_t rnd = 0x6ee87ee4) {
    FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = server;
    g.Randomizer = rnd;
    return g;
}

inline repl::ExportLink link(uint64_t id, const char* name, bool no_load, uint32_t checksum) {
    repl::ExportLink l;
    l.guid = guid(id, 0, 0);
    l.name = name;
    l.no_load = no_load;
    l.has_checksum = true;
    l.checksum = checksum;
    return l;
}

inline void set_value(repl::ObjectState& o, const char* path, repl::Value v) {
    if (!o.state.set(*o.schema, path, std::move(v))) {
        std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
        g_fail++;
    }
}

inline world::WorldActor make_npc(uint64_t id, double x, double y, const char* archetype = "Wolf") {
    world::WorldActor w;
    w.id = id;
    w.kind = world::ActorKind::Npc;
    w.x = x;
    w.y = y;
    w.z = 12000;
    repl::Actor& a = w.actor;
    a.guid = guid(id);
    a.obj.schema = &schema_named("ANPCCharacter");
    set_value(a.obj, "AuthServerIDReplicated", repl::Value::u64(60));
    repl::RepMovement m;
    m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), 1200000};
    m.rotation = repl::Rot{0, 64, 0};
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    set_value(a.obj, "ReplicatedMovement", repl::Value(m));
    set_value(a.obj, "RemoteRole", repl::Value::u64(1));
    set_value(a.obj, "Owner", repl::Value(guid(id + 2)));
    set_value(a.obj, "Role", repl::Value::u64(4));
    set_value(a.obj, "Controller", repl::Value(guid(id + 2)));
    set_value(a.obj, "ReplicatedMovementMode", repl::Value::u64(10));
    set_value(a.obj, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{0}));
    const std::string path = std::string("/Game/DesignAssets/Population/NPC/Beast/") + archetype + "/" + archetype + ".Default__" + archetype + "_C";
    a.open.archetype = {link(0xA000 + std::hash<std::string>{}(archetype) % 1000 * 2 + 1, (std::string("Default__") + archetype + "_C").c_str(), false, 0x1234),
                        link(0xB000 + std::hash<std::string>{}(archetype) % 1000 * 2 + 1, path.substr(0, path.rfind('.')).c_str(), false, 0)};
    a.open.level = aoc::worldd::bootstrap::persistent_level_export();
    a.open.spawn.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 10), static_cast<int64_t>(y * 10), 120000};
    a.open.spawn.rotation = repl::Rot{0, 11000, 0};
    repl::Subobject info;
    info.guid = guid(id + 6);
    info.name = "BaseCharacterInfo";
    info.checksum = 0xaff9746f;
    info.obj.schema = &schema_named("UNPCInfo");
    set_value(info.obj, "CharacterGuid.A", repl::Value::i64(static_cast<int32_t>(id)));
    set_value(info.obj, "CreatureOwnerName", repl::Value::str(std::string("A ") + archetype));
    set_value(info.obj, "Level", repl::Value::i64(14));
    a.subobjects.push_back(std::move(info));
    repl::Subobject combat;
    combat.guid = guid(id + 8);
    combat.name = "CombatInfo";
    combat.checksum = 0xb9f1a7bc;
    combat.obj.schema = &schema_named("UNPCCombatInfo");
    set_value(combat.obj, "bIsDead", repl::Value::boolean(false));
    set_value(combat.obj, "EliteRank", repl::Value::i64(0));
    a.subobjects.push_back(std::move(combat));
    w.npc.name = std::string("A ") + archetype;
    w.npc.level = 14;
    w.npc.archetype = path;
    return w;
}

inline world::WorldActor make_state_actor(uint64_t id, double x, double y, size_t components, size_t items_each) {
    world::WorldActor w;
    w.id = id;
    w.kind = world::ActorKind::Foliage;
    w.x = x;
    w.y = y;
    w.z = 12000;
    w.snapshot_only = true;
    w.keep_fast_shadow = true;
    w.dormancy_delay = milliseconds(265);
    repl::Actor& a = w.actor;
    a.guid = guid(id);
    a.obj.schema = &schema_named("AIntrepidStateIFA");
    set_value(a.obj, "AuthServerIDReplicated", repl::Value::u64(60));
    repl::RepMovement m;
    m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 100), static_cast<int64_t>(y * 100), 1200000};
    m.rotation = repl::Rot{0, 0, 0};
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    set_value(a.obj, "ReplicatedMovement", repl::Value(m));
    set_value(a.obj, "RemoteRole", repl::Value::u64(1));
    set_value(a.obj, "Role", repl::Value::u64(4));
    set_value(a.obj, "FoliageName", repl::Value(repl::NameValue::plain("InstancedFoliageActor_25600_-40_19", 1)));
    a.open.archetype = aoc::worldd::bootstrap::foliage_state_actor_archetype_export();
    a.open.level = aoc::worldd::bootstrap::persistent_level_export();
    a.open.spawn.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, static_cast<int64_t>(x * 10), static_cast<int64_t>(y * 10), 120000};
    for (size_t c = 0; c < components; ++c) {
        repl::Subobject sub;
        sub.guid = guid(id + 2 + 2 * c);
        sub.dynamic_class = aoc::worldd::bootstrap::foliage_state_component_class_export();
        sub.obj.schema = &schema_named("UIntrepidFoliageStateComponent");
        set_value(sub.obj, "FoliageComponentUUID", repl::Value(repl::NameValue::plain("IntrepidGatherableFISMC", static_cast<int32_t>(c + 1))));
        set_value(sub.obj, "bInitialized", repl::Value::boolean(true));
        repl::FastArrayState* fa = sub.obj.arrays.get(*sub.obj.schema, "IndexedStates");
        for (size_t i = 0; i < items_each; ++i) {
            repl::ElementState es;
            es.values[1] = repl::Value::u64(3);
            es.values[2] = repl::Value::u64(i);
            fa->add(std::move(es));
        }
        a.subobjects.push_back(std::move(sub));
    }
    return w;
}

inline size_t open_bits(const std::vector<Seen>& s) {
    for (const Seen& x : s) {
        if (x.open) return x.bits;
    }
    return 0;
}

inline ActorView::Config view_config() { return ActorView::Config{}; }

inline std::function<bool(uint32_t)> reserved_3_to_6() { return [](uint32_t i) { return i >= 3 && i <= 6; }; }

inline void run(ActorView& v, Pair& p, double x, double y, int steps = 1) {
    for (int i = 0; i < steps; ++i) {
        v.tick(p.s, x, y, p.t);
        p.step(&v, milliseconds(65));
    }
}

inline std::vector<Seen> take(Pair& p) {
    std::vector<Seen> out;
    out.swap(p.seen);
    return out;
}

void run_actor_view_players_tests();
void run_actor_view_shared_tests();
void run_actor_view_threads_tests();
