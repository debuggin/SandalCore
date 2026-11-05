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
    if (const char *env = std::getenv("AOC_CONTENT_DB"); env && *env)
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
    return n.rfind("Artisan", 0) == 0 && n.size() > 7 && n.compare(n.size() - 7, 7, "_Rating") == 0;
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
        if (!c)
            return std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str()), false;
        captured = std::move(*c);
        donor = aoc::worldd::spawns::garden_donor(*captured);
        if (!donor)
            return std::printf("  [FAIL] no captured gatherable open\n"), false;
        if (!world->start_gathering(aoc::worldd::gather::Config{}, error))
            return std::printf("  [FAIL] gathering does not start: %s\n", error.c_str()), false;
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
        if (!db)
            return std::printf("  [FAIL] the content.db does not open: %s\n", error.c_str()), false;
        world = std::make_unique<aoc::worldd::PlayerWorld>(&*db, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId, guids);
        if (!world->ready())
            return std::printf("  [FAIL] the world is not ready: %s\n", world->content_problem().c_str()), false;
        return init_spawns_and_gathering(error);
    }

    aoc::worldd::world::ActorRegistry &registry() { return world->registry(); }
    aoc::protocol::emit::FIntrepidNetworkGUID mint() { return world->mint_guid(); }

    uint64_t add_node(int64_t def_guid, uint32_t seed, double x = 0, double y = 0, std::string *error = nullptr) {
        std::string why;
        aoc::worldd::spawns::InstanceParams p;
        p.keep_captured_position = true;
        p.server_id = kServerId;
        p.seed = 0x6A12DE0ull ^ seed;
        auto node = aoc::worldd::spawns::instantiate(*donor, [this] { return world->mint_guid(); }, p, why);
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

void run_gather_rolls_tests(World &w);
