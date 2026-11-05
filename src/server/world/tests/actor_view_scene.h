// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "combat/npc_ability_replicator.h"
#include "test_world_actor_view_common.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace view_scene {

struct Rng {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    }
    int64_t below(int64_t n) { return static_cast<int64_t>(next()) % n; }
    bool chance(int percent) { return below(100) < percent; }
};

inline std::optional<repl::Value> scalar_value(const schema::RepCommand& c, int64_t pick) {
    using T = schema::PropType;
    switch (c.type) {
        case T::Bool: return repl::Value::boolean((pick & 1) != 0);
        case T::Int8:
        case T::Int16:
        case T::Int32:
        case T::Int64: {
            const int64_t room = c.width_bits == 0 || c.width_bits >= 4 ? 4 : (int64_t{1} << (c.width_bits - 1));
            return repl::Value::i64(pick % room);
        }
        case T::UInt8:
        case T::UInt16:
        case T::UInt32:
        case T::UInt64:
        case T::Enum: {
            const int64_t room = c.width_bits == 0 || c.width_bits >= 3 ? 4 : (int64_t{1} << c.width_bits);
            return repl::Value::u64(static_cast<uint64_t>(pick % room));
        }
        case T::Float: return repl::Value::f32(0.5f * static_cast<float>(pick));
        case T::Double: return repl::Value::f64(0.5 * static_cast<double>(pick));
        case T::Name: return pick == 0 ? repl::Value(repl::NameValue::hardcoded_name(0)) : repl::Value(repl::NameValue::plain("n" + std::to_string(pick)));
        case T::Str: return repl::Value::str(std::string(static_cast<size_t>(pick), 'a'));
        case T::Object: {
            FIntrepidNetworkGUID g;
            if (pick != 0) g = guid(0x7000 + 2 * static_cast<uint64_t>(pick));
            return repl::Value(g);
        }
        default: return std::nullopt;
    }
}

inline std::optional<repl::Value> array_value(const schema::ClassSchema& sc, const schema::RepCommand& c, Rng& rng) {
    if (c.elem_end <= c.elem_begin || c.elem_end > sc.element_commands.size()) return std::nullopt;
    const auto elems = sc.element_commands.subspan(c.elem_begin, c.elem_end - c.elem_begin);
    repl::ArrayValue a;
    const int n = static_cast<int>(rng.below(4));
    for (int i = 0; i < n; ++i) {
        repl::ElementState es;
        for (size_t k = 0; k < elems.size(); ++k) {
            if (!rng.chance(55)) continue;
            if (auto v = scalar_value(elems[k], rng.below(4))) es.values[static_cast<uint16_t>(k + 1)] = std::move(*v);
        }
        a.elements.push_back(std::move(es));
    }
    return repl::Value(std::move(a));
}

inline std::optional<repl::Value> any_value(const schema::ClassSchema& sc, const schema::RepCommand& c, Rng& rng) {
    if (c.flags & schema::kRepCustomDelta) return std::nullopt;
    if (c.type == schema::PropType::DynamicArray) return array_value(sc, c, rng);
    return scalar_value(c, rng.below(4));
}

inline repl::ObjectState& object_of(repl::Actor& a, size_t i) { return i == 0 ? a.obj : a.subobjects[i - 1].obj; }

inline world::WorldActor make_real_npc(uint64_t id, double x, double y, Rng& rng) {
    world::WorldActor w = make_npc(id, x, y, "Wolf");
    const auto add = [&](const char* schema_name, const char* name, uint32_t checksum, uint64_t offset) {
        repl::Subobject s;
        s.guid = guid(id + offset);
        s.name = name;
        s.checksum = checksum;
        s.obj.schema = &schema_named(schema_name);
        w.actor.subobjects.push_back(std::move(s));
    };
    add("UAlignmentComponent", "AlignmentComponent", 1069384348u, 10);
    add("UAoCAbilityComponent", "AbilityComponent", 2657332828u, 12);
    add("UInteractableComponent", "InteractableComponent", 2170799233u, 14);
    for (size_t i = 0; i <= w.actor.subobjects.size(); ++i) {
        repl::ObjectState& o = object_of(w.actor, i);
        for (const schema::RepCommand& c : o.schema->commands) {
            if (c.type == schema::PropType::NetSerializeStruct || !rng.chance(50)) continue;
            if (const auto v = any_value(*o.schema, c, rng)) o.state.set(*o.schema, c.handle, *v);
        }
    }
    return w;
}

inline void make_interactable(world::WorldActor& w, int64_t definition) {
    for (repl::Subobject& s : w.actor.subobjects) {
        if (std::string(s.obj.schema->name) != "UInteractableComponent") continue;
        repl::ArrayValue inputs;
        repl::ElementState es;
        es.values[6] = repl::Value::i64(definition);
        inputs.elements.push_back(std::move(es));
        s.obj.state.set(*s.obj.schema, "InteractionInputData", repl::Value(std::move(inputs)));
    }
}

}  // namespace view_scene

namespace redo_checks {

using namespace view_scene;

struct Conn {
    repl::ActorShadow shadow;
    uint32_t          sent_version = 0;
    uint32_t          epoch = 0;
    bool              open = false;
};

struct Scene {
    world::ActorRegistry reg;
    uint64_t             id = 4000;
    Rng                  rng;
    combat::NpcCastState cast;
    struct Erased {
        size_t      object;
        uint16_t    handle;
        repl::Value value;
    };
    std::vector<Erased>                             erased;
    std::vector<std::pair<size_t, repl::Subobject>> removed;
    bool                                            shape_changes = true;
    size_t ops = 0, sets = 0, arrays = 0, casts = 0, erases = 0, resets = 0, removals = 0, readds = 0, moves = 0;
};

inline void mutate_value_or_movement(Scene& sc, int roll) {
    Rng& rng = sc.rng;
    const world::Change kind = world::Change::State;
    if (roll < 380 || (sc.shape_changes && roll >= 860)) {
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            repl::ObjectState& o = object_of(w.actor, static_cast<size_t>(rng.below(static_cast<int64_t>(w.actor.subobjects.size()) + 1)));
            const auto& cmds = o.schema->commands;
            const schema::RepCommand& c = cmds[static_cast<size_t>(rng.below(static_cast<int64_t>(cmds.size())))];
            if (c.type == schema::PropType::NetSerializeStruct || c.type == schema::PropType::DynamicArray || (c.flags & schema::kRepCustomDelta)) return;
            std::optional<repl::Value> v;
            if (const repl::Value* cur = o.state.get(c.handle); cur && rng.chance(40)) v = *cur;
            else v = scalar_value(c, rng.chance(25) ? 0 : rng.below(4));
            if (v && o.state.set(*o.schema, c.handle, std::move(*v))) ++sc.sets;
        });
    } else if (roll < 480) {
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            repl::ObjectState& o = object_of(w.actor, static_cast<size_t>(rng.below(static_cast<int64_t>(w.actor.subobjects.size()) + 1)));
            std::vector<const schema::RepCommand*> arrays;
            for (const schema::RepCommand& c : o.schema->commands) {
                if (c.type == schema::PropType::DynamicArray && !(c.flags & schema::kRepCustomDelta)) arrays.push_back(&c);
            }
            if (arrays.empty()) return;
            const schema::RepCommand& c = *arrays[static_cast<size_t>(rng.below(static_cast<int64_t>(arrays.size())))];
            if (auto v = array_value(*o.schema, c, rng); v && o.state.set(*o.schema, c.handle, std::move(*v))) ++sc.arrays;
        });
    } else if (roll < 580) {
        combat::NpcCast c;
        c.ability = 1000 + rng.below(3);
        c.montage = 0x9000 + 2 * static_cast<uint64_t>(rng.below(3));
        const repl::ExportChain montage = {link(c.montage, "Montage", false, 0x55)};
        if (!sc.cast.playing) combat::start_npc_cast(sc.reg, sc.id, sc.cast, c, montage);
        else combat::end_npc_cast(sc.reg, sc.id, sc.cast);
        ++sc.casts;
    } else {
        sc.reg.modify(sc.id, world::Change::Movement, [&](world::WorldActor& w) {
            repl::RepMovement m;
            m.location = repl::QuantVec{repl::QuantVec::Mode::Scaled, rng.below(100000), rng.below(100000), 1200000};
            m.rotation = repl::Rot{0, static_cast<uint16_t>(rng.below(200)), 0};
            m.linear_velocity = repl::QuantVec{repl::QuantVec::Mode::Scaled, 100, 0, 0};
            w.actor.obj.state.set(*w.actor.obj.schema, "ReplicatedMovement", repl::Value(m));
        });
        ++sc.moves;
    }
}

inline void mutate_shape_change(Scene& sc, int roll) {
    Rng& rng = sc.rng;
    const world::Change kind = world::Change::State;
    if (roll < 780 || !sc.shape_changes) {
        sc.reg.modify(sc.id, kind, [](world::WorldActor&) {});
    } else if (roll < 810) {
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            const size_t oi = static_cast<size_t>(rng.below(static_cast<int64_t>(w.actor.subobjects.size()) + 1));
            repl::ObjectState& o = object_of(w.actor, oi);
            if (o.state.empty()) return;
            auto it = o.state.values().begin();
            std::advance(it, rng.below(static_cast<int64_t>(o.state.values().size())));
            if (it->second.get_if<repl::ArrayValue>()) return;
            const uint16_t handle = it->first;
            repl::Value value = it->second;
            if (o.state.erase(handle)) {
                sc.erased.push_back({oi, handle, std::move(value)});
                ++sc.erases;
            }
        });
    } else if (roll < 840) {
        if (sc.erased.empty()) return;
        const size_t k = static_cast<size_t>(rng.below(static_cast<int64_t>(sc.erased.size())));
        const Scene::Erased e = sc.erased[k];
        sc.erased.erase(sc.erased.begin() + static_cast<std::ptrdiff_t>(k));
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            if (e.object > w.actor.subobjects.size()) return;
            repl::ObjectState& o = object_of(w.actor, e.object);
            repl::Value v = e.value;
            const schema::RepCommand* c = rng.chance(30) ? o.schema->command(e.handle) : nullptr;
            if (c) {
                if (auto other = scalar_value(*c, rng.below(4))) v = std::move(*other);
            }
            if (o.state.set(*o.schema, e.handle, std::move(v))) ++sc.resets;
        });
    } else if (roll < 850) {
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            if (w.actor.subobjects.size() <= 3) return;
            const size_t i = 3 + static_cast<size_t>(rng.below(static_cast<int64_t>(w.actor.subobjects.size()) - 3));
            sc.removed.emplace_back(i, std::move(w.actor.subobjects[i]));
            w.actor.subobjects.erase(w.actor.subobjects.begin() + static_cast<std::ptrdiff_t>(i));
            ++sc.removals;
        });
    } else {
        if (sc.removed.empty()) return;
        const size_t k = static_cast<size_t>(rng.below(static_cast<int64_t>(sc.removed.size())));
        auto back = std::move(sc.removed[k]);
        sc.removed.erase(sc.removed.begin() + static_cast<std::ptrdiff_t>(k));
        sc.reg.modify(sc.id, kind, [&](world::WorldActor& w) {
            const size_t at = rng.chance(50) ? w.actor.subobjects.size() : std::min(back.first, w.actor.subobjects.size());
            w.actor.subobjects.insert(w.actor.subobjects.begin() + static_cast<std::ptrdiff_t>(at), std::move(back.second));
            ++sc.readds;
        });
    }
}

inline void mutate(Scene& sc) {
    ++sc.ops;
    const int roll = static_cast<int>(sc.rng.below(1000));
    if (roll < 680 || (sc.shape_changes && roll >= 860)) mutate_value_or_movement(sc, roll);
    else mutate_shape_change(sc, roll);
}

struct PairResult {
    bool compared = false;
    bool same = false;
    bool redo_self = false;
};

inline PairResult compare_pair(const world::WorldActor& w, const repl::ActorShadow& sa, const repl::ActorShadow& sb) {
    PairResult r;
    const repl::ExportAcks acks{};
    repl::ActorShadow ca = sa, cb = sb;
    repl::ShadowUndo ua, ub;
    std::vector<repl::ContentBlock> ba, bb;
    if (!repl::collect_update_blocks(w.actor, ca, ba, &ua) || !repl::collect_update_blocks(w.actor, cb, bb, &ub)) return r;
    const auto ea = ba.empty() ? std::nullopt : repl::assemble_blocks(w.actor, ba, acks);
    const auto eb = bb.empty() ? std::nullopt : repl::assemble_blocks(w.actor, bb, acks);
    if (ba.empty() != !ea.has_value() || bb.empty() != !eb.has_value()) return r;
    r.compared = true;
    const repl::ShadowRedo redo = repl::make_redo(ua, ca);
    repl::ActorShadow own = sa;
    redo.apply(own);
    r.redo_self = repl::shadows_equal(own, ca);
    repl::ActorShadow via = sb;
    redo.apply(via);
    const bool same_bytes = ea.has_value() == eb.has_value() && (!ea || repl::same_emission(*ea, *eb));
    const bool same_rec = repl::same_record(repl::sent_record(ua, ca), repl::sent_record(ub, cb));
    r.same = same_bytes && same_rec && repl::shadows_equal(via, cb);
    return r;
}

struct Tally {
    size_t pairs = 0;
    size_t same = 0, differ = 0;
    size_t ok_pairs = 0, ok_differ = 0;
    size_t private_pairs = 0, private_differ = 0;
    size_t updates = 0, redo_wrong = 0;
    size_t opens = 0;
    size_t unbuildable = 0;
};

inline void compare_scene_conns(const world::WorldActor& a, const std::vector<Conn>& conns, Tally& t) {
    for (size_t i = 0; i < conns.size(); ++i) {
        for (size_t j = i + 1; j < conns.size(); ++j) {
            const Conn& x = conns[i];
            const Conn& y = conns[j];
            if (!x.open || !y.open || x.sent_version != y.sent_version || x.sent_version == a.version) continue;
            const PairResult r = compare_pair(a, x.shadow, y.shadow);
            if (!r.compared) {
                ++t.unbuildable;
                continue;
            }
            ++t.pairs;
            (r.same ? t.same : t.differ) += 1;
            const bool ok = x.epoch == a.shape_epoch && y.epoch == a.shape_epoch;
            if (ok) {
                ++t.ok_pairs;
                t.ok_differ += r.same ? 0 : 1;
            } else {
                ++t.private_pairs;
                t.private_differ += r.same ? 0 : 1;
            }
            if (!r.redo_self) ++t.redo_wrong;
        }
    }
}

inline void open_scene_conn(const world::WorldActor& a, Conn& c, Tally& t) {
    const repl::ExportAcks acks{};
    c.shadow = repl::ActorShadow{};
    bool failed = false;
    if (repl::open_actor(a.actor, c.shadow, acks, &failed) && !failed) {
        c.sent_version = a.version;
        c.epoch = a.shape_epoch;
        c.open = true;
        ++t.opens;
    } else {
        c.open = false;
    }
}

inline Tally run_scene(uint64_t seed, int steps, bool shape_changes, Scene& sc) {
    sc.rng = Rng{seed};
    sc.shape_changes = shape_changes;
    Tally t;
    sc.reg.add(make_real_npc(sc.id, 3000, 3000, sc.rng));
    std::vector<Conn> conns(4);
    const repl::ExportAcks acks{};
    open_scene_conn(*sc.reg.find(sc.id), conns[0], t);
    for (int step = 0; step < steps; ++step) {
        mutate(sc);
        const world::WorldActor* a = sc.reg.find(sc.id);
        compare_scene_conns(*a, conns, t);
        for (size_t i = 0; i < conns.size(); ++i) {
            Conn& c = conns[i];
            if (!c.open ? (step >= 3 * static_cast<int>(i) && sc.rng.chance(60)) : sc.rng.chance(i == 0 ? 2 : 4)) {
                open_scene_conn(*a, c, t);
                continue;
            }
            if (!c.open || !sc.rng.chance(i == 0 ? 85 : 65)) continue;
            repl::ShadowUndo undo;
            std::vector<repl::ContentBlock> blocks;
            if (!repl::collect_update_blocks(a->actor, c.shadow, blocks, &undo)) {
                ++t.unbuildable;
                continue;
            }
            if (!blocks.empty() && !repl::assemble_blocks(a->actor, blocks, acks)) {
                undo.rollback(c.shadow);
                ++t.unbuildable;
                continue;
            }
            c.sent_version = a->version;
            ++t.updates;
        }
    }
    return t;
}

}  // namespace redo_checks
