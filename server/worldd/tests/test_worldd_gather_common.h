// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "capture_gather.h"
#include "gather/rules.h"
#include "protocol/repl/block_reader.h"
#include "session/player_world.h"
#include "test_support.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

inline int g_pass = 0;
inline int g_fail = 0;

#undef CHECK
#define CHECK(cond, msg)                       \
    do {                                       \
        if (cond) {                            \
            std::printf("  [ok ] %s\n", msg);  \
            g_pass++;                          \
        } else {                               \
            std::printf("  [FAIL] %s\n", msg); \
            g_fail++;                          \
        }                                      \
    } while (0)

inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(5000)};

namespace id {
inline constexpr int64_t kMining = 108752769996425ll;
inline constexpr int64_t kLumberjacking = 108752769996426ll;
inline constexpr int64_t kHerbalism = 108752770061963ll;
inline constexpr int64_t kT1Pickaxe = 6064629375589548039ll;
inline constexpr int64_t kT2Pickaxe = 6064630367571673324ll;
inline constexpr int64_t kT3Pickaxe = 6064629376467927053ll;
inline constexpr int64_t kT1Axe = 6064629375589613576ll;
inline constexpr int64_t kT1Sickle = 6064629375589482502ll;
inline constexpr int64_t kDaffodil = 6064632138835886135ll;
inline constexpr int64_t kMoonbell = 6064631483095515243ll;
inline constexpr int64_t kLimestone = 6064631483017789494ll;
inline constexpr int64_t kRividium = 6064631669497200654ll;
inline constexpr int64_t kHalcyonite = 6064631483054948432ll;
} // namespace id

inline const char *db_path() {
    const char *env = std::getenv("AOC_CONTENT_DB");
    if (env && *env)
        return env;
#ifdef GATHER_CONTENT_DB_PATH
    if (FILE *f = std::fopen(GATHER_CONTENT_DB_PATH, "rb")) {
        std::fclose(f);
        return GATHER_CONTENT_DB_PATH;
    }
#endif
    return nullptr;
}

inline bool is_artisan_rating_name(const std::string &n) {
    if (n.rfind("Artisan", 0) != 0 || n.size() <= 7)
        return false;
    return n.compare(n.size() - 7, 7, "_Rating") == 0;
}

inline void patch_node_interaction(aoc::worldd::world::WorldActor &node, int64_t def_guid, int64_t interaction_guid) {
    aoc::protocol::repl::ObjectState &actor = node.actor.obj;
    actor.state.set(*actor.schema, "GatherableActorDef.Guid", aoc::protocol::repl::Value::i64(def_guid));
    for (aoc::protocol::repl::Subobject &s : node.actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
            continue;
        const aoc::protocol::repl::Value *data = s.obj.state.get(*s.obj.schema, "InteractionInputData");
        const auto *arr = data ? data->get_if<aoc::protocol::repl::ArrayValue>() : nullptr;
        if (!arr || arr->elements.empty())
            continue;
        aoc::protocol::repl::ArrayValue copy = *arr;
        copy.elements[0].values[6] = aoc::protocol::repl::Value::i64(interaction_guid);
        s.obj.state.set(*s.obj.schema, "InteractionInputData", aoc::protocol::repl::Value(std::move(copy)));
    }
}

struct World {
    std::optional<aoc::worldd::content::ContentDb> db;
    aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
    std::unique_ptr<aoc::worldd::PlayerWorld> world;
    std::optional<aoc::worldd::spawns::CapturedData> captured;
    const aoc::worldd::spawns::OpenTemplate *donor = nullptr;
    aoc::worldd::gather::Gathering *g = nullptr;
    std::vector<int64_t> ratings;

    bool init_spawns_and_gathering(std::string &error) {
        auto c = aoc::worldd::spawns::CapturedData::load(error);
        if (!c) {
            std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
            return false;
        }
        captured = std::move(*c);
        donor = aoc::worldd::spawns::garden_donor(*captured);
        if (!donor) {
            std::printf("  [FAIL] no captured gatherable open\n");
            return false;
        }
        aoc::worldd::gather::Config cfg;
        if (!world->start_gathering(cfg, error)) {
            std::printf("  [FAIL] gathering does not start: %s\n", error.c_str());
            return false;
        }
        g = world->gathering();
        const aoc::worldd::stats::StatBundle &bundle = *world->stat_bundle();
        for (size_t i = 0; i < bundle.stats.size(); ++i) {
            if (is_artisan_rating_name(bundle.stat_name(i)))
                ratings.push_back(bundle.stats[i].guid);
        }
        return true;
    }

    bool open(const char *path) {
        std::string error;
        db = aoc::worldd::content::ContentDb::open(path, error);
        if (!db) {
            std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str());
            return false;
        }
        world = std::make_unique<aoc::worldd::PlayerWorld>(&*db, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId, guids);
        if (!world->ready()) {
            std::printf("  [FAIL] the world is not ready: %s\n", world->content_problem().c_str());
            return false;
        }
        return init_spawns_and_gathering(error);
    }

    aoc::worldd::world::ActorRegistry &registry() {
        return world->registry();
    }
    aoc::protocol::emit::FIntrepidNetworkGUID mint() {
        return world->mint_guid();
    }

    uint64_t add_node(int64_t def_guid, uint32_t seed, double x = 0, double y = 0, std::string *error = nullptr) {
        std::string why;
        aoc::worldd::spawns::InstanceParams p;
        p.keep_captured_position = true;
        p.server_id = kServerId;
        p.seed = 0x6A12DE0ull ^ seed;
        auto node = aoc::worldd::spawns::instantiate(*donor, [this] {
            return world->mint_guid();
        },
                                                     p, why);
        const aoc::worldd::content::GatherDefRow *def = g->def(def_guid);
        if (!node || !def) {
            if (error)
                *error = !node ? why : ("no gather def " + std::to_string(def_guid));
            return 0;
        }
        patch_node_interaction(*node, def_guid, def->interaction_guid);
        node->x = x;
        node->y = y;
        node->z = 0;
        node->npc.source = aoc::worldd::world::Source::Runtime;
        const uint64_t id = node->id;
        if (!registry().add(std::move(*node))) {
            if (error)
                *error = "the registry refuses the node";
            return 0;
        }
        return id;
    }

    const aoc::worldd::content::GatherDefRow *def_named(const std::string &name) const {
        for (const auto &[guid, d] : g->data().defs) {
            if (d.name == name)
                return &d;
        }
        return nullptr;
    }

    std::unique_ptr<aoc::worldd::stats::CharacterStats> make_stats(int level = 25) {
        const aoc::worldd::bootstrap::StatContext ctx = world->stat_context();
        auto cs = std::make_unique<aoc::worldd::stats::CharacterStats>(*ctx.bundle, *ctx.gear, 2, level, ctx.rounding);
        std::string err;
        if (!cs->start({}, err)) {
            std::printf("  [FAIL] the character's stats do not start: %s\n", err.c_str());
            std::exit(2);
        }
        return cs;
    }

    void rate_all(double rating) {
        std::map<int64_t, double> m;
        for (const int64_t g2 : ratings)
            m[g2] = rating;
        g->set_ratings(std::move(m));
    }
};

inline bool payload_is(const aoc::protocol::repl::ContentBlock &b, const capture_gather::Block &want) {
    if (b.has_rep_layout != want.rep)
        return false;
    const tsupport::Bits mine(b.payload);
    const tsupport::Bits theirs = tsupport::bits_of_hex(want.hex, want.bits);
    if (mine == theirs)
        return true;
    size_t at = 0;
    while (at < mine.size() && at < theirs.size() && mine.b[at] == theirs.b[at])
        ++at;
    std::printf("      (differs: %zu bits against the capture's %zu, the first difference at bit %zu)\n", mine.size(), theirs.size(), at);
    return false;
}

inline std::optional<aoc::protocol::repl::ObjectState> load(const capture_gather::Block &b, const char *cls) {
    return aoc::protocol::repl::load_object_state_hex(tsupport::schema_named(cls), b.hex, b.bits, b.rep);
}

inline const aoc::protocol::repl::ElementState *element(const aoc::protocol::repl::ObjectState &o, const char *path, size_t index) {
    const aoc::protocol::repl::Value *v = o.state.get(*o.schema, path);
    const auto *a = v ? v->get_if<aoc::protocol::repl::ArrayValue>() : nullptr;
    if (!a)
        return nullptr;
    return index < a->elements.size() ? &a->elements[index] : nullptr;
}

inline void setup_node_as_1427(World &w, uint64_t node_id, const aoc::protocol::repl::ObjectState &cap_actor, const aoc::protocol::repl::ObjectState &cap_comp) {
    w.registry().modify(node_id, aoc::worldd::world::Change::State, [&](aoc::worldd::world::WorldActor &a) {
        aoc::protocol::repl::ObjectState &o = a.actor.obj;
        for (const char *path : {"AuthServerIDReplicated", "RemoteRole", "Role", "GatherableGuid.A", "GatherableGuid.B", "GatherableGuid.C", "GatherableGuid.D"}) {
            if (const aoc::protocol::repl::Value *v = cap_actor.state.get(*cap_actor.schema, path))
                o.state.set(*o.schema, path, *v);
        }
        o.state.erase(7);
        for (aoc::protocol::repl::Subobject &s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
                continue;
            if (const aoc::protocol::repl::Value *v = cap_comp.state.get(*cap_comp.schema, "InteractionInputData")) {
                s.obj.state.set(*s.obj.schema, "InteractionInputData", *v);
            }
            s.obj.arrays = aoc::protocol::repl::FastArrays();
            if (const aoc::protocol::repl::Value *v = cap_comp.state.get(*cap_comp.schema, "CurrentInputStates")) {
                s.obj.state.set(*s.obj.schema, "CurrentInputStates", *v);
            }
        }
    });
}

inline aoc::protocol::repl::Actor player_view_of(World &w, uint64_t node_id, uint64_t player) {
    const aoc::worldd::world::WorldActor *a = w.registry().find(node_id);
    aoc::protocol::repl::Actor v = a->actor;
    aoc::worldd::gather::show_interacting_player(*a, player, v);
    return v;
}

inline std::optional<uint32_t> interacting_player_id_in_view(const aoc::protocol::repl::Actor &view) {
    for (const aoc::protocol::repl::Subobject &s : view.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
            continue;
        const aoc::protocol::repl::ElementState *el = element(s.obj, "InputEffectStatuses", 0);
        if (!el)
            return 0xffffffffu;
        const auto it = el->values.find(3);
        const auto *g = it != el->values.end() ? it->second.get_if<aoc::protocol::emit::FIntrepidNetworkGUID>() : nullptr;
        return g ? static_cast<uint32_t>(g->ObjectId) : 0xffffffffu;
    }
    return std::nullopt;
}

inline void setup_node_as_1454(World &w, uint64_t node_id) {
    const auto cap_comp = load(capture_gather::kGatheredComponent, "UInteractableComponent");
    w.registry().modify(node_id, aoc::worldd::world::Change::State, [&](aoc::worldd::world::WorldActor &a) {
        for (aoc::protocol::repl::Subobject &s : a.actor.subobjects) {
            if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
                continue;
            if (cap_comp) {
                if (const aoc::protocol::repl::Value *v = cap_comp->state.get(*cap_comp->schema, "InteractionInputData")) {
                    s.obj.state.set(*s.obj.schema, "InteractionInputData", *v);
                }
            }
            s.obj.arrays = aoc::protocol::repl::FastArrays();
        }
    });
}

inline bool is_gathering_profession(int64_t p) {
    if (p == id::kMining)
        return true;
    return p == id::kLumberjacking || p == id::kHerbalism;
}

inline bool is_valid_tool_slot(int32_t s) {
    static const int32_t slots[] = {16, 26, 29, 33, 34, 35, 36, 37, 38};
    for (int32_t v : slots) {
        if (s == v)
            return true;
    }
    return false;
}

inline aoc::worldd::gather::Channeler channeler(uint64_t player, uint32_t n) {
    aoc::worldd::gather::Channeler by;
    by.player_id = player;
    by.character = aoc::protocol::repl::GuidValue{n, n + 1, n + 2, n + 3};
    by.pawn.ObjectId = 1000 + player * 2;
    return by;
}

inline std::optional<std::vector<std::optional<bool>>> can_input_on_wire(const aoc::protocol::repl::ContentBlock &b) {
    if (!b.sub || !b.sub->obj.schema || !b.has_rep_layout)
        return std::nullopt;
    if (std::string(b.sub->obj.schema->name) != "UInteractableComponent")
        return std::nullopt;
    const tsupport::Bits bits(b.payload);
    return tsupport::with_exact(bits, [&](const uint8_t *d, size_t, size_t n) -> std::optional<std::vector<std::optional<bool>>> {
        size_t pos = 0;
        const auto st = aoc::protocol::repl::read_rep_block(*b.sub->obj.schema, d, n, pos, {});
        const aoc::protocol::repl::Value *v = st ? st->get(*b.sub->obj.schema, "CanInputActivate") : nullptr;
        const auto *a = v ? v->get_if<aoc::protocol::repl::ArrayValue>() : nullptr;
        if (!a)
            return std::nullopt;
        std::vector<std::optional<bool>> out;
        for (const aoc::protocol::repl::ElementState &e : a->elements) {
            const auto it = e.values.find(1);
            const bool *x = it == e.values.end() ? nullptr : it->second.get_if<bool>();
            out.push_back(x ? std::optional<bool>(*x) : std::nullopt);
        }
        return out;
    });
}

inline std::optional<std::vector<std::optional<bool>>> read_can_input_update(World &w, uint64_t a, aoc::protocol::repl::ActorShadow &shadow) {
    const aoc::worldd::world::WorldActor *n = w.registry().find(a);
    std::vector<aoc::protocol::repl::ContentBlock> update;
    if (!n || !aoc::protocol::repl::collect_update_blocks(n->actor, shadow, update))
        return std::nullopt;
    for (const aoc::protocol::repl::ContentBlock &b : update) {
        if (auto c = can_input_on_wire(b))
            return c;
    }
    return std::nullopt;
}

inline bool four_and_first_is(const std::optional<std::vector<std::optional<bool>>> &c, bool first) {
    if (!c || c->size() != 4)
        return false;
    return (*c)[0].has_value() && *(*c)[0] == first;
}

inline size_t registry_can_input_size(const aoc::worldd::world::WorldActor *n) {
    if (!n)
        return 99;
    for (const aoc::protocol::repl::Subobject &s : n->actor.subobjects) {
        if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
            continue;
        const aoc::protocol::repl::Value *v = s.obj.state.get(*s.obj.schema, "CanInputActivate");
        const auto *arr = v ? v->get_if<aoc::protocol::repl::ArrayValue>() : nullptr;
        return arr ? arr->elements.size() : 0;
    }
    return 99;
}

inline aoc::worldd::gather::ToolInfo make_tool(World &w, int64_t item, float durability = 100.0f) {
    aoc::worldd::gather::ToolInfo t;
    t.worn = true;
    t.tool = w.g->tool(item);
    t.durability = durability;
    return t;
}

struct Rig {
    World &w;
    std::unique_ptr<aoc::worldd::stats::CharacterStats> cs;
    std::unique_ptr<aoc::worldd::stats::CharacterStats::Evaluation> run;
    aoc::worldd::stats::SplitMix rng;

    explicit Rig(World &world, uint64_t seed = 1)
        : w(world), cs(world.make_stats()) {
        std::string err;
        run = cs->evaluation({}, err);
        if (!run) {
            std::printf("  [FAIL] the stat run does not start: %s\n", err.c_str());
            std::exit(2);
        }
        rng.state = seed;
    }

    std::optional<aoc::worldd::gather::Harvest> harvest(int64_t def_guid, std::string *why = nullptr) {
        std::string e;
        const aoc::worldd::content::GatherDefRow *d = w.g->def(def_guid);
        auto h = w.g->harvest(*d, *run, 25, aoc::worldd::world::NodeSite{}, nullptr, rng, e);
        if (why)
            *why = e;
        return h;
    }
};

inline bool valid_common_harvest(const std::optional<aoc::worldd::gather::Harvest> &h, int64_t quantity, int xp) {
    if (!h || h->items.size() != 1)
        return false;
    if (h->items[0].quantity != quantity || h->items[0].quality != 1)
        return false;
    return h->profession_xp == xp && h->primary_rarity == 0;
}

inline void check_1427_beats_and_done(World &w, aoc::worldd::gather::NodeBoard &board, uint64_t node_id, aoc::protocol::repl::ActorShadow &shadow) {
    size_t same = 0;
    size_t one_block = 0;
    aoc::worldd::Clock::time_point now = t0;
    for (size_t i = 0; i < capture_gather::kRunBeatCount; ++i) {
        now += std::chrono::milliseconds(100);
        if (!board.beat(node_id, now)) {
            break;
        }
        aoc::protocol::repl::Actor view = player_view_of(w, node_id, 7);
        std::vector<aoc::protocol::repl::ContentBlock> update;
        if (!aoc::protocol::repl::collect_update_blocks(view, shadow, update)) {
            break;
        }
        if (update.size() != 1) {
            continue;
        }
        ++one_block;
        same += payload_is(update[0], capture_gather::kRunBeats[i].block) ? 1 : 0;
    }
    CHECK(one_block == capture_gather::kRunBeatCount, "every heartbeat is one block: the interactable's");
    CHECK(same == capture_gather::kRunBeatCount, "all 96 heartbeats are the capture's, to the bit (81 bits each; the first, 217, also takes InteractingPlayer off)");
    now += std::chrono::milliseconds(100);
    std::string why;
    CHECK(board.complete(node_id, now, std::chrono::seconds(1800), why), "the board completes the channel");
    aoc::protocol::repl::Actor view = player_view_of(w, node_id, 7);
    std::vector<aoc::protocol::repl::ContentBlock> update;
    const bool two_done = aoc::protocol::repl::collect_update_blocks(view, shadow, update) && update.size() == 2;
    CHECK(two_done, "the done update is two blocks");
    if (two_done) {
        CHECK(payload_is(update[0], capture_gather::kRunDoneActor), "the actor block (bGathered) is the capture's, to the bit (18 bits)");
        CHECK(payload_is(update[1], capture_gather::kRunDoneComponent), "the component block (the status 0x20000004, the pawn, nobody channels, CanInputActivate, the state 0) is the capture's, to the bit (334 bits)");
    }
    CHECK(board.state_of(node_id) == aoc::worldd::gather::NodeState::Gathered, "the node is Gathered");
    CHECK(aoc::protocol::repl::collect_update_blocks(player_view_of(w, node_id, 7), shadow, update) && update.empty(), "and nothing changes until the respawn");
    const auto o_comp = interacting_player_id_in_view(player_view_of(w, node_id, 99));
    CHECK(o_comp && *o_comp == 0, "another player's connection sees InteractingPlayer as none");
}
