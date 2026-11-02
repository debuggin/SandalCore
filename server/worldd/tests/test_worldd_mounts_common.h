// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "bootstrap/constants.h"
#include "protocol/schema/generated/index.h"
#include "session/mounts.h"
#include "session/player_world.h"
#include "spawns/mount_spawn.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

inline int g_pass = 0;
inline int g_fail = 0;
inline aoc::worldd::spawns::MountBook* g_book = nullptr;

#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

inline uint64_t g_next = 0x2000000;

inline aoc::protocol::emit::FIntrepidNetworkGUID mint() {
    aoc::protocol::emit::FIntrepidNetworkGUID g;
    g.ObjectId = g_next;
    g_next += 2;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}

inline std::string handle_path_or_id(const aoc::protocol::schema::ClassSchema& s, uint16_t h) {
    const aoc::protocol::schema::RepCommand* cmd = nullptr;
    for (const aoc::protocol::schema::RepCommand& c : s.commands) {
        if (c.handle == h) cmd = &c;
    }
    return cmd ? cmd->path : "#" + std::to_string(h);
}

inline void add_fast_array_diffs(
    const aoc::protocol::repl::ObjectState& a,
    const aoc::protocol::repl::ObjectState& b,
    std::set<std::string>& out) {
    for (const auto& [field, arr] : a.arrays.all()) {
        const aoc::protocol::repl::FastArrayState* other = b.arrays.find(field);
        std::vector<aoc::protocol::repl::ElementState> x, y;
        for (const auto& it : arr.items()) x.push_back(it.values);
        if (other) {
            for (const auto& it : other->items()) y.push_back(it.values);
        }
        if (x != y) out.insert(std::string("[") + a.schema->fields[field].name + "]");
    }
    for (const auto& [field, arr] : b.arrays.all()) {
        if (!a.arrays.find(field) && !arr.items().empty()) {
            out.insert(std::string("[") + b.schema->fields[field].name + "]");
        }
    }
}

inline std::set<std::string> differing(
    const aoc::protocol::repl::ObjectState& a,
    const aoc::protocol::repl::ObjectState& b) {
    std::set<std::string> out;
    std::set<uint16_t> handles;
    for (const auto& [h, v] : a.state.values()) handles.insert(h);
    for (const auto& [h, v] : b.state.values()) handles.insert(h);
    for (const uint16_t h : handles) {
        const aoc::protocol::repl::Value* x = a.state.get(h);
        const aoc::protocol::repl::Value* y = b.state.get(h);
        if (x && y && *x == *y) continue;
        out.insert(handle_path_or_id(*a.schema, h));
    }
    add_fast_array_diffs(a, b, out);
    return out;
}

inline std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const std::string& x : s) out += (out.empty() ? "" : ", ") + x;
    return out.empty() ? "nothing" : out;
}

inline int64_t int_of(const aoc::protocol::repl::Value* v) {
    if (!v) return 0;
    if (const auto* i = v->get_if<int64_t>()) return *i;
    if (const auto* u = v->get_if<uint64_t>()) return static_cast<int64_t>(*u);
    return 0;
}

inline const aoc::protocol::repl::Subobject* sub_named(
    const aoc::protocol::repl::Actor& a,
    const char* cls) {
    for (const aoc::protocol::repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

inline aoc::protocol::repl::GuidValue guid_of(
    const aoc::protocol::repl::ObjectState& o,
    const char* prefix) {
    aoc::protocol::repl::GuidValue g;
    const std::string p = prefix;
    g.a = static_cast<uint32_t>(int_of(o.state.get(*o.schema, p + ".A")));
    g.b = static_cast<uint32_t>(int_of(o.state.get(*o.schema, p + ".B")));
    g.c = static_cast<uint32_t>(int_of(o.state.get(*o.schema, p + ".C")));
    g.d = static_cast<uint32_t>(int_of(o.state.get(*o.schema, p + ".D")));
    return g;
}

struct DonorValues {
    aoc::worldd::spawns::MountParams p;
    double health = 0;
};

inline double donor_max_health_of(const aoc::worldd::spawns::ParsedBlock& stats) {
    const auto* nf = stats.state.schema->field("StatRepInt32EveryoneProxy");
    const aoc::protocol::repl::FastArrayState* arr = nf ? stats.state.arrays.find(nf->index) : nullptr;
    if (!arr) return 0.0;
    double health = 0.0;
    for (const aoc::protocol::repl::FastArrayItem& it : arr->items()) {
        const auto f = it.values.values.find(1);
        const auto* t = f == it.values.values.end() ? nullptr : f->second.get_if<aoc::protocol::repl::StatInt32Inst>();
        if (t && t->record == aoc::worldd::content::kGuidMaxHealth) health = std::bit_cast<float>(t->value);
    }
    return health;
}

inline void read_donor_actor_transform(
    const aoc::worldd::spawns::OpenTemplate& d,
    const aoc::protocol::repl::ObjectState& actor,
    aoc::worldd::spawns::MountParams& p) {
    if (const aoc::protocol::repl::Value* v = actor.state.get(*actor.schema, "Instigator")) {
        if (const auto* g = v->get_if<aoc::protocol::emit::FIntrepidNetworkGUID>()) p.owner_pawn = *g;
    }
    if (const aoc::protocol::repl::Value* v = actor.state.get(*actor.schema, "ReplicatedServerLastTransformUpdateTimeStamp")) {
        if (const float* f = v->get_if<float>()) p.world_time = *f;
    }
    if (const aoc::protocol::repl::Value* v = actor.state.get(*actor.schema, "ReplicatedMovement")) {
        if (const auto* m = v->get_if<aoc::protocol::repl::RepMovement>()) {
            const aoc::protocol::repl::Vec3d loc = aoc::protocol::repl::dequantize_location(actor.opts, m->location);
            p.x = loc.x;
            p.y = loc.y;
            p.z = loc.z;
            p.yaw_degrees = d.spawn.rotation ? d.spawn.rotation->yaw * 360.0 / 65536.0 : 0;
        }
    }
}

inline std::optional<DonorValues> values_of(const aoc::worldd::spawns::OpenTemplate& d) {
    DonorValues out;
    aoc::worldd::spawns::MountParams& p = out.p;
    const aoc::worldd::spawns::ParsedBlock* info = nullptr;
    const aoc::worldd::spawns::ParsedBlock* second = nullptr;
    const aoc::worldd::spawns::ParsedBlock* mount = nullptr;
    const aoc::worldd::spawns::ParsedBlock* stats = nullptr;
    for (const aoc::worldd::spawns::ParsedBlock& b : d.blocks) {
        if (b.cls == "UNPCInfo") info = &b;
        if (b.cls == "UNPCSecondaryInfo") second = &b;
        if (b.cls == "UMountComponent") mount = &b;
        if (b.cls == "UAoCStatsComponent") stats = &b;
    }
    if (!info || !second || !mount || !stats) return std::nullopt;
    p.owner_guid = guid_of(second->state, "CreatureOwnerGuid");
    const aoc::protocol::repl::Value* n = info->state.state.get(*info->state.schema, "CreatureOwnerName");
    if (n && n->get_if<std::string>()) p.owner_name = *n->get_if<std::string>();
    p.server_id = static_cast<uint32_t>(int_of(info->state.state.get(*info->state.schema, "ServerId")));
    p.item_guid = guid_of(mount->state, "ItemInstanceGuid");
    p.mount_guid = guid_of(info->state, "CharacterGuid");
    if (const aoc::protocol::repl::Value* v = mount->state.state.get(*mount->state.schema, "DissolveTimer")) {
        if (const float* f = v->get_if<float>()) p.dissolve_timer = *f;
    }
    read_donor_actor_transform(d, d.actor_block().state, p);
    out.health = donor_max_health_of(*stats);
    return out;
}

inline bool same_chain(const aoc::protocol::repl::ExportChain& a, const aoc::protocol::repl::ExportChain& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].guid.ObjectId != b[i].guid.ObjectId || a[i].name != b[i].name ||
            a[i].checksum != b[i].checksum || a[i].has_checksum != b[i].has_checksum) {
            return false;
        }
    }
    return true;
}

inline std::map<int64_t, aoc::protocol::repl::StatInt32Inst> stat_items_of(
    const aoc::protocol::repl::ObjectState& o) {
    std::map<int64_t, aoc::protocol::repl::StatInt32Inst> m;
    const aoc::protocol::schema::NetField* nf = o.schema->field("StatRepInt32EveryoneProxy");
    const aoc::protocol::repl::FastArrayState* arr = nf ? o.arrays.find(nf->index) : nullptr;
    if (!arr) return m;
    for (const aoc::protocol::repl::FastArrayItem& it : arr->items()) {
        const auto f1 = it.values.values.find(1);
        if (f1 == it.values.values.end()) continue;
        if (const auto* t = f1->second.get_if<aoc::protocol::repl::StatInt32Inst>()) m[t->record] = *t;
    }
    return m;
}

inline std::set<int64_t> stat_records_differing(
    const aoc::protocol::repl::ObjectState& a,
    const aoc::protocol::repl::ObjectState& b) {
    std::set<int64_t> out;
    const auto x = stat_items_of(a);
    const auto y = stat_items_of(b);
    for (const auto& [record, t] : x) {
        const auto it = y.find(record);
        if (it == y.end() || it->second.value != t.value) out.insert(record);
    }
    for (const auto& [record, t] : y) {
        if (!x.count(record)) out.insert(record);
    }
    return out;
}

inline bool is_road_effect_id(int64_t id, const aoc::worldd::content::MountData& data) {
    for (const aoc::worldd::content::MountEffectRow& e : data.effects) {
        if (e.kind == "road" && e.effect == id) return true;
    }
    return false;
}

inline bool item_has_road_effect(
    const aoc::protocol::repl::FastArrayItem& it,
    const aoc::worldd::content::MountData& data) {
    for (const auto& [h, v] : it.values.values) {
        const auto* i = v.get_if<int64_t>();
        if (i && is_road_effect_id(*i, data)) return true;
    }
    return false;
}

inline size_t road_effects_in(
    const aoc::protocol::repl::ObjectState& o,
    const aoc::worldd::content::MountData& data,
    size_t* all) {
    size_t roads = 0;
    size_t n = 0;
    const aoc::protocol::schema::NetField* nf = o.schema->field("CurrentEffects");
    const aoc::protocol::repl::FastArrayState* arr = nf ? o.arrays.find(nf->index) : nullptr;
    if (!arr) {
        if (all) *all = 0;
        return 0;
    }
    for (const aoc::protocol::repl::FastArrayItem& it : arr->items()) {
        ++n;
        if (item_has_road_effect(it, data)) ++roads;
    }
    if (all) *all = n;
    return roads;
}

inline bool writes_open(aoc::protocol::repl::Actor& a) {
    aoc::protocol::repl::ActorShadow shadow;
    aoc::protocol::repl::ExportAcks acks;
    bool failed = false;
    const auto e = aoc::protocol::repl::open_actor(a, shadow, acks, &failed);
    return e.has_value() && !failed && e->payload.bit_pos() > 0;
}

inline std::string db_path() {
    const char* env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : MOUNT_CONTENT_DB_PATH;
}

inline const aoc::worldd::content::MountRow* row_named(
    const aoc::worldd::spawns::MountBook& book,
    const char* item_name) {
    for (const aoc::worldd::content::MountRow& r : book.data().mounts) {
        if (r.item_name == item_name) return &r;
    }
    return nullptr;
}

inline const aoc::worldd::content::MountRow* first_of_kind(
    const aoc::worldd::spawns::MountBook& book,
    const char* kind) {
    for (const aoc::worldd::content::MountRow& r : book.data().mounts) {
        if (r.kind == kind && !r.mule) return &r;
    }
    return nullptr;
}

inline aoc::worldd::spawns::MountParams plain_params() {
    aoc::worldd::spawns::MountParams p;
    p.player_id = 3;
    p.owner_guid = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    p.owner_name = "Mounty";
    p.owner_pawn = mint();
    p.owner_level = 12;
    p.server_id = 60;
    p.world_time = 1234.5;
    p.item_guid = {0xA1u, 0xA2u, 0xA3u, 0xA4u};
    p.x = -777000;
    p.y = 616000;
    p.z = 5000;
    p.yaw_degrees = 90;
    return p;
}

inline std::map<int64_t, double> stat_values(const aoc::protocol::repl::Subobject* stats) {
    std::map<int64_t, double> out;
    if (!stats) return out;
    const aoc::protocol::repl::FastArrayState* arr =
        stats->obj.arrays.find(stats->obj.schema->field("StatRepInt32EveryoneProxy")->index);
    if (!arr) return out;
    for (const aoc::protocol::repl::FastArrayItem& it : arr->items()) {
        const auto f = it.values.values.find(1);
        const auto* t = f == it.values.values.end() ? nullptr : f->second.get_if<aoc::protocol::repl::StatInt32Inst>();
        if (t) out[t->record] = std::bit_cast<float>(t->value);
    }
    return out;
}

inline const aoc::protocol::repl::AlignmentContainer* alignments_in(
    const aoc::protocol::repl::ObjectState& o,
    const char* field) {
    const aoc::protocol::repl::Value* v = o.state.get(*o.schema, field);
    return v ? v->get_if<aoc::protocol::repl::AlignmentContainer>() : nullptr;
}

inline const aoc::protocol::repl::AlignmentContainer::Instance* instance_of_key(
    const aoc::protocol::repl::AlignmentContainer& c,
    uint64_t key) {
    for (const auto& i : c.instances) {
        if (i.key == key) return &i;
    }
    return nullptr;
}

inline std::vector<uint64_t> keys_of(const aoc::protocol::repl::AlignmentContainer& c) {
    std::vector<uint64_t> out;
    for (const auto& i : c.instances) out.push_back(i.key);
    return out;
}

inline bool flag_off(const aoc::protocol::repl::ObjectState& o, const char* path) {
    const aoc::protocol::repl::Value* v = o.state.get(*o.schema, path);
    if (!v) return true;
    if (const auto* b = v->get_if<bool>()) return !*b;
    return int_of(v) == 0;
}

inline const aoc::worldd::content::MountRow* row_of_blueprint(
    const aoc::worldd::spawns::MountBook& book,
    const char* blueprint) {
    for (const aoc::worldd::content::MountRow& r : book.data().mounts) {
        if (r.blueprint == blueprint) return &r;
    }
    return nullptr;
}

inline std::optional<aoc::worldd::spawns::MountMade> make_plain(
    const aoc::worldd::spawns::MountBook& book,
    const aoc::worldd::content::MountRow& row,
    const aoc::worldd::spawns::MountParams& p,
    std::string& error) {
    uint64_t next = 0x6000000;
    const auto minter = [&]() {
        aoc::protocol::emit::FIntrepidNetworkGUID g;
        g.ObjectId = next;
        next += 2;
        g.ServerId = 60;
        g.Randomizer = 0x6ee87ee4u;
        return g;
    };
    return book.make(row, p, minter, error);
}

inline bool near_to(double got, double want, double tol = 0.0125) {
    return got >= 0 && std::fabs(got - want) <= tol;
}

void run_mounts_part2();
void run_mounts_part3();
