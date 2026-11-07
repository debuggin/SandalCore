// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  spawns/captured_spawns.cpp
//
//  See captured_spawns.h.
// ============================================================================
#include "spawns/captured_spawns.h"

#include "bootstrap/constants.h"
#include "content/content_db.h"
#include "crafting/station.h"
#include "protocol/repl/block_reader.h"
#include "protocol/schema/generated/index.h"
#include "spawns/area_spawns.h"
#include "spawns/mount_donors.h"
#include "spawns/spawn_util.h"
#include "world/interaction_state.h"

namespace aoc::worldd::spawns {

namespace gen = ::aoc::protocol::schema::gen;
using protocol::emit::FIntrepidNetworkGUID;
using util::angle16;
using util::angle8;
using util::erase_prefix;
using util::int_of;
using util::set_path;
using util::splitmix;

int OpenTemplate::find(const std::string& cls) const {
    for (size_t i = 1; i < blocks.size(); ++i) {
        if (blocks[i].cls == cls) return static_cast<int>(i);
    }
    return -1;
}

namespace {

template <class Link, class Chain>
repl::ExportChain chain_of(const Link* links, const Chain& c) {
    repl::ExportChain out;
    for (uint32_t i = 0; i < c.count; ++i) {
        const Link& l = links[c.first + i];
        repl::ExportLink link;
        link.guid.ObjectId = l.id;
        link.guid.ServerId = l.server_id;
        link.guid.Randomizer = l.randomizer;
        link.name = l.name;
        link.no_load = (l.flags & 2) != 0;
        link.has_checksum = (l.flags & 4) != 0;
        link.checksum = l.checksum;
        out.push_back(std::move(link));
    }
    return out;
}

template <class Vec>
repl::QuantVec quant_of(const Vec& v) {
    repl::QuantVec q;
    switch (v.mode) {
        case 0: q.mode = repl::QuantVec::Mode::Scaled; break;
        case 1: q.mode = repl::QuantVec::Mode::Unscaled; break;
        case 2: q.mode = repl::QuantVec::Mode::RawFloat; break;
        default: q.mode = repl::QuantVec::Mode::RawDouble; break;
    }
    q.x = v.x;
    q.y = v.y;
    q.z = v.z;
    return q;
}
// == C | O | A | C:Y:B ==

/// The location of a SerializeNewActor vector in cm.
template <class Vec>
bool location_cm(const Vec& v, double& x, double& y, double& z) {
    if (!v.present) return false;
    switch (v.mode) {
        case 0: x = v.x / 10.0; y = v.y / 10.0; z = v.z / 10.0; return true;
        case 1: x = static_cast<double>(v.x); y = static_cast<double>(v.y); z = static_cast<double>(v.z); return true;
        case 2: {
            const auto f = [](int64_t bits) { return static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(bits))); };
            x = f(v.x); y = f(v.y); z = f(v.z);
            return true;
        }
        default: {
            const auto d = [](int64_t bits) { return std::bit_cast<double>(static_cast<uint64_t>(bits)); };
            x = d(v.x); y = d(v.y); z = d(v.z);
            return true;
        }
    }
}

template <class Block, class Payload, class Chain, class Link>
struct TableRefs {
    const Block*   blocks = nullptr;
    const Payload* payloads = nullptr;
    const Chain*   chains = nullptr;
    const Link*    links = nullptr;
};

/// Reads one open of a generated table (spawns/area_spawns.h's `captured` or spawns/mount_donors.h's: the two have the same layout) into a template.
/// False, with `error`, when a class has no schema or a payload does not read to its last bit.
template <class Open, class Block, class Payload, class Chain, class Link>
bool read_open(const Open& o, const TableRefs<Block, Payload, Chain, Link>& refs, OpenTemplate& t, std::string& error) {
    t.category = o.category;
    t.archetype_path = o.archetype;
    t.archetype = chain_of(refs.links, refs.chains[o.archetype_chain]);
    t.capture = o.capture;
    t.channel = o.channel;
    t.packet = o.packet;
    if (o.location.present) t.spawn.location = quant_of(o.location);
    if (o.rotation.present) t.spawn.rotation = repl::Rot{o.rotation.pitch, o.rotation.yaw, o.rotation.roll};
    if (o.scale.present) t.spawn.scale = quant_of(o.scale);
    location_cm(o.location, t.x, t.y, t.z);
    for (uint32_t b = 0; b < o.blocks; ++b) {
        const Block& blk = refs.blocks[o.first_block + b];
        const Payload& payload = refs.payloads[blk.payload];
        ParsedBlock pb;
        pb.kind = blk.kind;
        pb.cls = blk.cls;
        pb.schema = gen::find_class_schema(blk.cls);
        if (!pb.schema) {
            error = std::string("the open of ") + o.archetype + " (capture " + std::to_string(o.capture) + ", packet " + std::to_string(o.packet) + ") needs the class " +
                    blk.cls + ", which has no schema";
            return false;
        }
        std::string why;
        auto state = repl::load_object_state_hex(*pb.schema, payload.hex, payload.bits, blk.rep, &why);
        if (!state) {
            error = std::string("the ") + blk.cls + " block of the open of " + o.archetype + " (capture " + std::to_string(o.capture) + ", packet " + std::to_string(o.packet) +
                    ") does not load: " + why;
            return false;
        }
        pb.state = std::move(*state);
        pb.name = blk.name;
        pb.checksum = blk.checksum;
        if (blk.kind == 2) pb.class_chain = chain_of(refs.links, refs.chains[blk.class_chain]);
        t.blocks.push_back(std::move(pb));
    }
    if (t.blocks.empty() || t.blocks.front().kind != 0) {
        error = std::string("the open of ") + o.archetype + " has no actor block first";
        return false;
    }
    return true;
}

}  // namespace

std::optional<CapturedData> CapturedData::load(std::string& error) {
    CapturedData out;
    out.opens_.reserve(std::size(captured::kOpens));
    const TableRefs<captured::Block, captured::Payload, captured::Chain, captured::Link> refs{
        captured::kBlocks, captured::kPayloads, captured::kChains, captured::kLinks};
    for (size_t i = 0; i < std::size(captured::kOpens); ++i) {
        OpenTemplate t;
        if (!read_open(captured::kOpens[i], refs, t, error)) return std::nullopt;
        out.opens_.push_back(std::move(t));
    }
    for (const captured::Link& l : captured::kLinks) out.static_ids_.insert(l.id);
    for (const captured::Template& t : captured::kTemplates) out.by_path_[t.archetype] = t.open;
    for (const captured::Spawn& s : captured::kSpawns) {
        if (s.open >= out.opens_.size()) {
            error = "a captured spawn names an open that is none";
            return std::nullopt;
        }
        out.spawns_.push_back({&out.opens_[s.open], s.x, s.y, s.z});
    }
    return out;
}

const OpenTemplate* CapturedData::template_for(const std::string& archetype_path) const {
    const auto it = by_path_.find(archetype_path);
    return it == by_path_.end() ? nullptr : &opens_[it->second];
}

std::set<std::string> CapturedData::npc_leaves() const {
    std::set<std::string> out;
    for (const auto& [path, open] : by_path_) {
        if (opens_[open].category != "npc") continue;
        std::string leaf = path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1);
        if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
        if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
        out.insert(leaf);
    }
    return out;
}
// CoA.C:Y:B

std::optional<MountDonors> MountDonors::load(std::string& error) {
    MountDonors out;
    out.opens_.reserve(std::size(mount_donors::kOpens));
    const TableRefs<mount_donors::Block, mount_donors::Payload, mount_donors::Chain, mount_donors::Link> refs{
        mount_donors::kBlocks, mount_donors::kPayloads, mount_donors::kChains, mount_donors::kLinks};
    for (const mount_donors::Open& o : mount_donors::kOpens) {
        OpenTemplate t;
        if (!read_open(o, refs, t, error)) return std::nullopt;
        out.opens_.push_back(std::move(t));
    }
    for (const mount_donors::Donor& d : mount_donors::kDonors) {
        if (d.open >= out.opens_.size()) {
            error = "a mount donor names an open that is none";
            return std::nullopt;
        }
        out.by_kind_[d.kind] = d.open;
    }
    for (const mount_donors::Link& l : mount_donors::kLinks) out.static_ids_.insert(l.id);
    return out;
}

const OpenTemplate* MountDonors::donor(const std::string& kind) const {
    const auto it = by_kind_.find(kind);
    return it == by_kind_.end() ? nullptr : &opens_[it->second];
}

double CapturedData::area_center_x() const { return captured::kAreaCenterX; }
double CapturedData::area_center_y() const { return captured::kAreaCenterY; }
double CapturedData::area_radius_cm() const { return captured::kAreaRadiusCm; }

// ---- instantiate ------------------------------------------------------------------------------------------------

namespace {

repl::Subobject* find_subobject(repl::Actor& a, const char* cls) {
    for (repl::Subobject& s : a.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == cls) return &s;
    }
    return nullptr;
}

bool apply_actor_transform(world::WorldActor& w, const InstanceParams& p, std::string& error) {
    repl::Actor& a = w.actor;
    repl::ObjectState& actor = a.obj;
    repl::RepMovement m;
    if (const repl::Value* v = actor.state.get(7)) {
        if (const auto* old = v->get_if<repl::RepMovement>()) m = *old;
    }
    m.simulated_physic_sleep = false;
    m.rep_physics = false;
    m.location = repl::quantize_location(actor.opts, p.x, p.y, p.z);
    m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
    m.acceleration.reset();
    m.rotation = repl::Rot{0, actor.opts.rep_movement_short_rotation ? angle16(p.yaw_degrees) : angle8(p.yaw_degrees), 0};
    if (!set_path(actor, "ReplicatedMovement", repl::Value(m), error)) return false;
    a.open.spawn.location = repl::quantize_vector(10, p.x, p.y, p.z);
    a.open.spawn.rotation = repl::Rot{0, angle16(p.yaw_degrees), 0};
    a.open.spawn.velocity.reset();
    w.x = p.x;
    w.y = p.y;
    w.z = p.z;
    w.yaw_degrees = p.yaw_degrees;
    return true;
}

bool instantiate_subobjects(const OpenTemplate& open, const Minter& mint, const InstanceParams& p, world::WorldActor& w, std::string& error) {
    const bool prop = w.kind == world::ActorKind::Prop;
    for (size_t i = 1; i < open.blocks.size(); ++i) {
        const ParsedBlock& b = open.blocks[i];
        repl::Subobject s;
        s.guid = mint();
        s.obj = b.state;
        if (b.kind == 1) {
            s.name = b.name;
            s.checksum = b.checksum;
        } else {
            s.dynamic_class = b.class_chain;
        }
        if (b.cls == "UInteractableComponent" && prop && s.obj.schema && p.component_def != 0) {
            const repl::Value* data = s.obj.state.get(*s.obj.schema, "InteractionInputData");
            const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
            if (!s.obj.state.set(*s.obj.schema, "ComponentDefId.Guid", repl::Value::i64(p.component_def)) || !arr || arr->elements.empty()) {
                error = "the interactable component of the template does not hold a definition and an input";
                return false;
            }
            repl::ArrayValue copy = *arr;
            copy.elements[0].values[6] = repl::Value::i64(p.interact_def);
            if (!s.obj.state.set(*s.obj.schema, "InteractionInputData", repl::Value(std::move(copy)))) {
                error = "the interactable component of the template refuses its input";
                return false;
            }
        }
        if (b.cls == "UInteractableComponent") {
            w.npc.interact_component = s.guid.ObjectId;
            if (b.schema && b.schema->command("InteractionInputData")) {
                const repl::Value* data = b.state.state.get(*b.schema, "InteractionInputData");
                const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
                if (arr && !arr->elements.empty()) {
                    const auto g = arr->elements[0].values.find(6);
                    if (g != arr->elements[0].values.end()) {
                        if (const int64_t* id = g->second.get_if<int64_t>()) w.npc.interact_def = *id;
                    }
                }
            }
        }
        w.actor.subobjects.push_back(std::move(s));
    }
    return true;
}

bool instantiate_npc_health(repl::Subobject& stats, std::string& error) {
    repl::FastArrayState* arr = stats.obj.arrays.get(*stats.obj.schema, "StatRepInt32EveryoneProxy");
    if (!arr) return true;
    const auto stat_of = [](const repl::FastArrayItem& it) -> const repl::StatInt32Inst* {
        const auto f = it.values.values.find(1);
        return f == it.values.values.end() ? nullptr : f->second.get_if<repl::StatInt32Inst>();
    };
    uint32_t max_bits = 0;
    bool have_max = false;
    for (const repl::FastArrayItem& it : arr->items()) {
        if (const repl::StatInt32Inst* t = stat_of(it); t && t->record == content::kGuidMaxHealth) {
            max_bits = t->value;
            have_max = true;
        }
    }
    if (!have_max) return true;
    std::vector<std::pair<int32_t, repl::ElementState>> changes;
    for (const repl::FastArrayItem& it : arr->items()) {
        const repl::StatInt32Inst* t = stat_of(it);
        if (!t || t->record != content::kGuidHealth || t->value == max_bits) continue;
        repl::StatInt32Inst full = *t;
        full.value = max_bits;
        repl::ElementState values = it.values;
        values.values[1] = repl::Value(full);
        changes.emplace_back(it.id, std::move(values));
    }
    for (auto& [id, values] : changes) {
        if (!arr->change(id, std::move(values))) {
            error = "Health cannot be set to its maximum";
            return false;
        }
    }
    return true;
}

bool instantiate_npc_subobjects(world::WorldActor& w, const InstanceParams& p, std::string& error) {
    repl::Actor& a = w.actor;
    uint32_t old_guid[4] = {0, 0, 0, 0}, fresh[4];
    uint64_t seed = p.seed ? p.seed : w.id;
    for (uint32_t& f : fresh) f = static_cast<uint32_t>(splitmix(seed) >> 16);
    if (repl::Subobject* info = find_subobject(a, "UNPCInfo")) {
        const char* words[4] = {"CharacterGuid.A", "CharacterGuid.B", "CharacterGuid.C", "CharacterGuid.D"};
        for (int i = 0; i < 4; ++i) {
            old_guid[i] = static_cast<uint32_t>(int_of(info->obj.state.get(*info->obj.schema, words[i])));
            if (!set_path(info->obj, words[i], repl::Value::i64(static_cast<int32_t>(fresh[i])), error)) return false;
        }
        if (p.level > 0 && !set_path(info->obj, "Level", repl::Value::i64(p.level), error)) return false;
        if (!p.name.empty() && !set_path(info->obj, "CharacterName", repl::Value::str(p.name), error)) return false;
        if (info->obj.state.get(*info->obj.schema, "ServerId") && !set_path(info->obj, "ServerId", repl::Value::i64(static_cast<int32_t>(p.server_id)), error)) return false;
        if (const repl::Value* n = info->obj.state.get(*info->obj.schema, "CharacterName")) {
            if (const auto* str = n->get_if<std::string>()) w.npc.name = *str;
        }
        w.npc.level = static_cast<int>(int_of(info->obj.state.get(*info->obj.schema, "Level")));
    }
    if (repl::Subobject* al = find_subobject(a, "UAlignmentComponent")) {
        for (const char* path : {"Alignments", "InheritedAlignments"}) {
            const repl::Value* v = al->obj.state.get(*al->obj.schema, path);
            const auto* c = v ? v->get_if<repl::AlignmentContainer>() : nullptr;
            if (!c) continue;
            repl::AlignmentContainer fixed = *c;
            for (auto& inst : fixed.instances) {
                for (repl::GuidValue& g : inst.guids) {
                    if (g.a == old_guid[0] && g.b == old_guid[1] && g.c == old_guid[2] && g.d == old_guid[3] && (old_guid[0] | old_guid[1] | old_guid[2] | old_guid[3])) {
                        g.a = fresh[0]; g.b = fresh[1]; g.c = fresh[2]; g.d = fresh[3];
                    }
                }
            }
            if (!set_path(al->obj, path, repl::Value(std::move(fixed)), error)) return false;
        }
    }
    if (repl::Subobject* second = find_subobject(a, "UNPCSecondaryInfo")) {
        if (const repl::Subobject* look = find_subobject(a, "UNPCAppearanceComponent")) {
            if (second->obj.schema->command("AppearanceComponent") && !set_path(second->obj, "AppearanceComponent", repl::Value(look->guid), error)) return false;
        }
        if (p.clear_population && (!set_path(second->obj, "PopInstanceId", repl::Value::i64(0), error) || !set_path(second->obj, "PopAssetId", repl::Value::i64(0), error))) return false;
        if (p.pop_instance && !set_path(second->obj, "PopInstanceId", repl::Value::i64(p.pop_instance), error)) return false;
        if (p.pop_asset && !set_path(second->obj, "PopAssetId", repl::Value::i64(p.pop_asset), error)) return false;
    }
    if (repl::Subobject* combat = find_subobject(a, "UNPCCombatInfo")) {
        if (!set_path(combat->obj, "bIsDead", repl::Value::boolean(false), error)) return false;
        if (combat->obj.state.get(*combat->obj.schema, "bInCombat") && !set_path(combat->obj, "bInCombat", repl::Value::boolean(false), error)) return false;
    }
    if (repl::Subobject* stats = find_subobject(a, "UAoCStatsComponent"); stats && p.full_health) {
        if (!instantiate_npc_health(*stats, error)) return false;
    }
    return true;
}

bool instantiate_npc(const OpenTemplate& open, const FIntrepidNetworkGUID& controller, const InstanceParams& p, world::WorldActor& w, std::string& error) {
    repl::Actor& a = w.actor;
    repl::ObjectState& actor = a.obj;
    if (!p.keep_captured_position) {
        if (!apply_actor_transform(w, p, error)) return false;
    } else {
        w.x = open.x;
        w.y = open.y;
        w.z = open.z;
        w.yaw_degrees = open.spawn.rotation ? open.spawn.rotation->yaw * 360.0 / 65536.0 : 0;
        if (const repl::Value* v = actor.state.get(7)) {
            if (const auto* old = v->get_if<repl::RepMovement>()) {
                repl::RepMovement m = *old;
                m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
                m.angular_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 0, 0, 0};
                m.acceleration.reset();
                if (!set_path(actor, "ReplicatedMovement", repl::Value(m), error)) return false;
            }
        }
        a.open.spawn.velocity.reset();
    }
    if (!set_path(actor, "Owner", repl::Value(controller), error) || !set_path(actor, "Controller", repl::Value(controller), error) ||
        !set_path(actor, "ReplicatedServerLastTransformUpdateTimeStamp", repl::Value::f32(static_cast<float>(p.world_time)), error)) {
        return false;
    }
    if (const schema::RepCommand* c = actor.schema->command("Instigator")) actor.state.erase(c->handle);
    if (actor.state.get(*actor.schema, "CurrentMovementInput") && !set_path(actor, "CurrentMovementInput", repl::Value(repl::PlayerMovementInput{0}), error)) {
        return false;
    }
    return instantiate_npc_subobjects(w, p, error);
}

bool instantiate_shrine(const InstanceParams& p, world::WorldActor& w, std::string& error) {
    if (!apply_actor_transform(w, p, error)) return false;
    repl::ObjectState& actor = w.actor.obj;
    uint64_t seed = p.seed ? p.seed : w.id;
    const char* const word_paths[4] = {"SpawnerGuid.A", "SpawnerGuid.B", "SpawnerGuid.C", "SpawnerGuid.D"};
    for (const char* path : word_paths) {
        if (!set_path(actor, path, repl::Value::i64(static_cast<int32_t>(splitmix(seed) >> 16)), error)) return false;
    }
    if (p.attune_record != 0 && !set_path(actor, "AttuneRecordId.Guid", repl::Value::i64(p.attune_record), error)) return false;
    w.shrine.place = p.attune_record;
    w.shrine.seed = p.seed;
    w.npc.name = "Emberspring";
    w.npc.source = world::Source::Data;
    return true;
}

bool instantiate_prop(const InstanceParams& p, world::WorldActor& w, std::string& error) {
    if (!apply_actor_transform(w, p, error)) return false;
    repl::Actor& a = w.actor;
    repl::ObjectState& actor = a.obj;
    uint64_t seed = p.seed ? p.seed : w.id;
    const char* const word_paths[4] = {"InteractableBaseGuid.A", "InteractableBaseGuid.B", "InteractableBaseGuid.C", "InteractableBaseGuid.D"};
    for (const char* path : word_paths) {
        if (!set_path(actor, path, repl::Value::i64(static_cast<int32_t>(splitmix(seed) >> 16)), error)) return false;
    }
    if (!set_path(actor, "InteractableActorDefId.Guid", repl::Value::i64(p.actor_def), error)) return false;
    if (repl::Subobject* ic = find_subobject(a, "UInteractableComponent")) {
        if (!set_path(actor, "InteractableComponent", repl::Value(ic->guid), error)) return false;
    }
    for (const char* field : {"SynchronizedMontageComponent", "StatsComponent", "Instigators"}) {
        if (const schema::RepCommand* c = actor.schema->command(field)) actor.state.erase(c->handle);
    }
    if (!world::allow_inputs(a)) {
        error = "the quest object has no interactable component to allow its input on";
        return false;
    }
    w.prop.component_def = p.component_def;
    w.prop.interact_def = p.interact_def;
    w.prop.actor_def = p.actor_def;
    w.prop.instance = p.pop_instance;
    w.npc.name = "quest object";
    w.npc.source = world::Source::Data;
    return true;
}

bool instantiate_station(const OpenTemplate& open, const InstanceParams& p, world::WorldActor& w, std::string& error) {
    if (!apply_actor_transform(w, p, error)) return false;
    repl::ObjectState& actor = w.actor.obj;
    w.station.seed = p.seed;
    const std::array<int32_t, 4> words = crafting::station_guid_words(p.seed ? p.seed : w.id);
    const char* const word_paths[4] = {"StationGuid.A", "StationGuid.B", "StationGuid.C", "StationGuid.D"};
    for (size_t i = 0; i < 4; ++i) {
        if (!set_path(actor, word_paths[i], repl::Value::i64(words[i]), error)) return false;
    }
    const size_t dot = open.archetype_path.rfind('.');
    std::string leaf = open.archetype_path.substr(dot == std::string::npos ? 0 : dot + 1);
    if (leaf.rfind("Default__", 0) == 0) leaf = leaf.substr(9);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    w.station.name = leaf;
    w.station.processing = leaf.rfind("ProcessingStation_", 0) == 0;
    w.npc.name = leaf;
    w.npc.source = world::Source::Captured;
    return true;
}

bool instantiate_gatherable(const OpenTemplate& open, const InstanceParams& p, world::WorldActor& w, std::string& error) {
    repl::Actor& a = w.actor;
    repl::ObjectState& actor = a.obj;
    if (!p.keep_captured_position) {
        if (!apply_actor_transform(w, p, error)) return false;
    } else {
        w.x = open.x;
        w.y = open.y;
        w.z = open.z;
    }
    if (p.actor_def != 0 && !set_path(actor, "GatherableActorDef.Guid", repl::Value::i64(p.actor_def), error)) return false;
    if (p.interact_def != 0) {
        if (repl::Subobject* ic = find_subobject(a, "UInteractableComponent"); ic && ic->obj.schema) {
            const repl::Value* data = ic->obj.state.get(*ic->obj.schema, "InteractionInputData");
            const auto* arr = data ? data->get_if<repl::ArrayValue>() : nullptr;
            if (arr && !arr->elements.empty() && arr->elements[0].values.count(6)) {
                repl::ArrayValue copy = *arr;
                copy.elements[0].values[6] = repl::Value::i64(p.interact_def);
                if (!ic->obj.state.set(*ic->obj.schema, "InteractionInputData", repl::Value(std::move(copy)))) {
                    error = "the gatherable interactable component refuses its input";
                    return false;
                }
                w.npc.interact_def = p.interact_def;
            }
        }
    }
    uint64_t seed = p.seed ? p.seed : w.id;
    const char* words[4] = {"GatherableGuid.A", "GatherableGuid.B", "GatherableGuid.C", "GatherableGuid.D"};
    for (const char* path : words) {
        if (!set_path(actor, path, repl::Value::i64(static_cast<int32_t>(splitmix(seed) >> 16)), error)) return false;
    }
    if (!p.name.empty()) {
        w.label = p.name;
        w.npc.name = p.name;
    }
    if (p.level > 0) w.npc.level = p.level;
    w.npc.source = p.keep_captured_position ? world::Source::Captured : world::Source::Data;
    return true;
}

}  // namespace

int64_t gatherable_def_guid(const OpenTemplate& open) noexcept {
    if (open.blocks.empty() || !open.blocks.front().schema) return 0;
    const ParsedBlock& b = open.blocks.front();
    return int_of(b.state.state.get(*b.schema, "GatherableActorDef.Guid"));
}

std::optional<world::WorldActor> instantiate(const OpenTemplate& open, const Minter& mint, const InstanceParams& p, std::string& error) {
    world::WorldActor w;
    const bool npc = open.category == "npc";
    const bool station = open.category == "station";
    const bool shrine = open.category == "shrine";
    const bool prop = open.category == "prop";
    w.kind = npc ? world::ActorKind::Npc : (station ? world::ActorKind::Station : (shrine ? world::ActorKind::Shrine : (prop ? world::ActorKind::Prop : world::ActorKind::Gatherable)));
    repl::Actor& a = w.actor;
    a.guid = mint();
    const FIntrepidNetworkGUID controller = (station || shrine || prop) ? FIntrepidNetworkGUID{} : mint();
    const FIntrepidNetworkGUID cylinder = (station || shrine || prop) ? FIntrepidNetworkGUID{} : mint();
    a.obj = open.actor_block().state;
    a.open.archetype = open.archetype;
    a.open.level = bootstrap::persistent_level_export();
    a.open.spawn = open.spawn;
    w.id = a.guid.ObjectId;
    w.npc.archetype = open.archetype_path;
    w.npc.controller = npc ? controller.ObjectId : 0;
    w.npc.collision_cylinder = (station || shrine || prop) ? 0 : cylinder.ObjectId;

    repl::ObjectState& actor = a.obj;
    if (!set_path(actor, "AuthServerIDReplicated", repl::Value::u64(bootstrap::kAuthServerIdReplicated), error)) return std::nullopt;
    for (const char* prefix : {"AttachmentReplication.", "ReplicatedBasedMovement.", "RepRootMotion."}) erase_prefix(actor, prefix);
    if (!instantiate_subobjects(open, mint, p, w, error)) return std::nullopt;

    const bool ok = npc ? instantiate_npc(open, controller, p, w, error)
                        : (shrine ? instantiate_shrine(p, w, error)
                                  : (prop ? instantiate_prop(p, w, error)
                                          : (station ? instantiate_station(open, p, w, error)
                                                     : instantiate_gatherable(open, p, w, error))));
    return ok ? std::optional<world::WorldActor>(std::move(w)) : std::nullopt;
}

}  // namespace aoc::worldd::spawns
