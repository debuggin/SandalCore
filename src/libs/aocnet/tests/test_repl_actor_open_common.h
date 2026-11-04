// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "capture_aoc_codecs.h"
#include "protocol/repl/replicator.h"
#include "protocol/schema/generated/index.h"
#include "protocol/wire/packet_reader.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <vector>

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline const aoc::protocol::schema::ClassSchema& schema_named(const char* name) {
    const aoc::protocol::schema::ClassSchema* s = aoc::protocol::schema::gen::find_class_schema(name);
    if (!s) { std::printf("  [FAIL] no schema %s\n", name); std::exit(2); }
    return *s;
}

inline const aoc::protocol::schema::ClassSchema& component_schema(const char* name) {
    const aoc::protocol::schema::ClassSchema* s = aoc::protocol::schema::gen::find_component_schema(name);
    if (!s) { std::printf("  [FAIL] no component schema %s\n", name); std::exit(2); }
    return *s;
}

inline aoc::protocol::emit::FIntrepidNetworkGUID guid(uint64_t id, uint32_t server = 0, uint32_t rnd = 0) {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId = id;
    g.ServerId = server;
    g.Randomizer = rnd;
    return g;
}

inline aoc::protocol::emit::FIntrepidNetworkGUID session_guid(uint64_t id) {
    return guid(id, 60, 0x6ee87ee4);
}

inline void set(aoc::protocol::repl::ObjectState& o, const char* path, aoc::protocol::repl::Value v) {
    if (!o.state.set(*o.schema, path, std::move(v))) {
        std::printf("  [FAIL] %s.%s refused\n", o.schema->name, path);
        g_fail++;
    }
}

inline aoc::protocol::repl::ExportLink link(uint64_t id, const char* name, bool no_load, uint32_t checksum) {
    aoc::protocol::repl::ExportLink l;
    l.guid = guid(id);
    l.name = name;
    l.no_load = no_load;
    l.has_checksum = true;
    l.checksum = checksum;
    return l;
}

inline aoc::protocol::repl::ExportChain level_chain() {
    return {link(16442478405498561049ull, "PersistentLevel", true, 0),
            link(16604466839667550161ull, "Verra_World_Master", true, 0),
            link(12923414834320654503ull, "/Game/Levels/Verra_World_Master/Verra_World_Master", true, 0)};
}

inline aoc::protocol::repl::RepState state_of(const capture_blocks::Block& b) {
    const aoc::protocol::schema::ClassSchema& s = schema_named(b.class_name);
    const auto bytes = capture_blocks::bytes_of(b.hex);
    size_t pos = 0;
    auto st = aoc::protocol::repl::read_rep_block(s, bytes.data(), b.bits, pos);
    if (!st) { std::printf("  [FAIL] %s does not read\n", b.name); g_fail++; return {}; }
    return *st;
}

inline aoc::protocol::repl::Subobject component(uint64_t id, const char* name, uint32_t checksum) {
    aoc::protocol::repl::Subobject s;
    s.guid = session_guid(id);
    s.name = name;
    s.checksum = checksum;
    s.obj.schema = &component_schema(name);
    return s;
}

inline aoc::protocol::repl::ExportChain gameplay_class(uint64_t id, const char* name, uint32_t checksum) {
    return {link(id, name, false, checksum), link(158953490572197689ull, "/Script/GameSystemsPlugin", false, 0)};
}

inline aoc::protocol::repl::Value object(uint64_t id) {
    return aoc::protocol::repl::Value(session_guid(id));
}

inline aoc::protocol::repl::QuantVec session_location() {
    return aoc::protocol::repl::QuantVec{aoc::protocol::repl::QuantVec::Mode::Scaled, -7776645, 6165787, 159520};
}

inline aoc::protocol::repl::Actor make_pc() {
    namespace repl = aoc::protocol::repl;
    repl::Actor a;
    a.guid = session_guid(10341530);
    a.obj.schema = &schema_named("AoCPlayerControllerBP_C");
    set(a.obj, "AuthServerIDReplicated", repl::Value::u64(0));
    set(a.obj, "RemoteRole", repl::Value::u64(2));
    set(a.obj, "Role", repl::Value::u64(4));
    set(a.obj, "PlayerState", object(10341528));
    set(a.obj, "SpawnLocation", repl::Value(repl::Vec3d{-777664.5, 616578.6875, 15952.0498046875}));
    set(a.obj, "CaravanLaunchNode", repl::Value::i64(0));
    set(a.obj, "SummonCooldownTimer", repl::Value::f32(1593462.375f));
    a.open.archetype = {link(3503756484819958835ull, "Default__AoCPlayerControllerBP_C", false, 0x6b62891c),
                        link(4074085207143396457ull, "/Game/ThirdPersonCPP/Blueprints/AoCPlayerControllerBP", false, 0)};
    a.open.level = level_chain();
    a.open.spawn.location = session_location();
    a.open.player_controller = repl::PlayerControllerOpen{0, 0};
    repl::Subobject gm;
    gm.guid = session_guid(10341532);
    gm.dynamic_class = gameplay_class(485698175673737329ull, "GlobalGMCommands", 0xcaaaee3e);
    a.subobjects.push_back(std::move(gm));
    return a;
}

inline aoc::protocol::repl::Actor make_pawn() {
    namespace repl = aoc::protocol::repl;
    repl::Actor a;
    a.guid = session_guid(10341538);
    a.obj.schema = &schema_named("PlayerPawn_C");
    set(a.obj, "AuthServerIDReplicated", repl::Value::u64(0));
    repl::RepMovement m;
    m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, -77766450, 61657869, 1595205};
    m.rotation = repl::Rot{0, 65, 0};
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    set(a.obj, "ReplicatedMovement", repl::Value(m));
    set(a.obj, "RemoteRole", repl::Value::u64(1));
    set(a.obj, "Owner", object(10341530));
    set(a.obj, "Role", repl::Value::u64(4));
    a.open.archetype = {link(9327572450991073355ull, "Default__PlayerPawn_C", false, 0x2cdc5b6),
                        link(9839620482321143831ull, "/Game/ThirdPersonCPP/Blueprints/PlayerPawn", false, 0)};
    a.open.level = level_chain();
    a.open.spawn.location = session_location();
    a.open.spawn.rotation = repl::Rot{0, 16550, 0};

    repl::Subobject base = component(10341540, "BaseCharacterInfo", 0xaff9746f);
    set(base.obj, "CharacterGuid.A", repl::Value::i64(static_cast<int32_t>(3635609113u)));
    set(base.obj, "CharacterGuid.B", repl::Value::i64(static_cast<int32_t>(2516274681u)));
    set(base.obj, "CharacterGuid.C", repl::Value::i64(static_cast<int32_t>(2602449240u)));
    set(base.obj, "CharacterGuid.D", repl::Value::i64(static_cast<int32_t>(1814768596u)));
    set(base.obj, "CharacterName", repl::Value::str("RandomChar"));
    set(base.obj, "CurrentBreathingTime", repl::Value::f32(30.0f));
    repl::Subobject combat = component(10341542, "CombatInfo", 0xb9f1a7bc);
    set(combat.obj, "bIsDead", repl::Value::boolean(false));
    set(combat.obj, "PrimaryClassId.Guid", repl::Value::i64(108640158570105ll));
    a.subobjects.push_back(std::move(base));
    a.subobjects.push_back(std::move(combat));
    a.subobjects.push_back(component(10341544, "OwnerInfo", 0xf05c630d));
    a.subobjects.push_back(component(10341546, "BackpackComponent", 0x24e4d927));
    a.subobjects.push_back(component(10341548, "EquipmentComponent", 0x24e4d927));
    a.subobjects.push_back(component(10341550, "QuestStorageComponent", 0x24e4d927));
    a.subobjects.push_back(component(10341552, "RewardStorageComponent", 0x24e4d927));
    repl::Subobject appearance = component(10341554, "Character Appearance", 0x864dde37);
    appearance.obj.state = state_of(capture_blocks::kCustomizationPawn78);
    a.subobjects.push_back(std::move(appearance));
    return a;
}

inline uint32_t export_count(const aoc::protocol::emit::BunchWriter& b) {
    aoc::protocol::wire::PacketReader r(b.data(), b.byte_size(), b.bit_pos());
    r.read_bit();
    return r.read_uint32();
}

void run_repl_actor_open_shadow();
