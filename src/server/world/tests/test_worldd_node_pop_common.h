// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "fake_client.h"
#include "nav_chunk_fixture.h"
#include "node_pop_golden.h"
#include "protocol/schema/generated/index.h"
#include "session/player_world.h"
#include "spawns/npc_looks.h"
#include "world/node_origins.h"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <bit>
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
#include <thread>
#include <vector>

namespace aoc::worldd::test_node_pop {

namespace bs = aoc::worldd::bootstrap;
namespace fs = std::filesystem;
namespace conn = aoc::protocol::conn;
namespace repl = aoc::protocol::repl;
using aoc::protocol::emit::FIntrepidNetworkGUID;
using spawns::Why;

inline int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            std::printf("  [ok ] %s\n", msg);                                                                          \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            std::printf("  [FAIL] %s\n", msg);                                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)
#define CHECKF(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            g_pass++;                                                                                                  \
        } else {                                                                                                       \
            std::printf("  [FAIL] ");                                                                                  \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

#ifndef NODES_CONTENT_DB_PATH
#define NODES_CONTENT_DB_PATH "dist/content/content.db"
#endif
#ifndef NODES_SEED_PATH
#define NODES_SEED_PATH "config/seeds/citynode_seed.json"
#endif
#ifndef NODES_MOTION_PATH
#define NODES_MOTION_PATH "dist/content/npc_motion.tsv"
#endif
#ifndef NODES_ROADS_LEVELS_PATH
#define NODES_ROADS_LEVELS_PATH "dist/content/roads_levels.tsv"
#endif
#ifndef NODES_NAV_PATH
#define NODES_NAV_PATH "dist/content/area_nav.bin"
#endif

inline std::string db_path() {
    const char *env = std::getenv("AOC_CONTENT_DB");
    return env && *env ? env : NODES_CONTENT_DB_PATH;
}
inline std::string motion_path() {
    const char *env = std::getenv("AOC_NPC_MOTION");
    return env && *env ? env : NODES_MOTION_PATH;
}

inline constexpr int64_t kJoeva = 108646121231993ll, kHalcyon = 108646610851452ll, kWinstead = 108646108518008ll;

struct Env {
    std::optional<content::ContentDb> db;
    std::shared_ptr<const content::PopulationData> pop;
    std::optional<spawns::CapturedData> captured;
    npc::MotionTable table;
    std::optional<spawns::GenericNpcs> generic;
    std::unique_ptr<spawns::NpcLooks> looks;
    std::unique_ptr<world::CityNodes> nodes;
    std::unique_ptr<world::NodeWire> wire;
    std::unique_ptr<world::CityNodes> shipped_nodes;
    std::unique_ptr<world::NodeWire> shipped_wire;
    std::unique_ptr<spawns::NodePopulationData> data;
    spawns::SpawnContext base;
    std::set<std::string> shown;
    spawns::TemplateSource templates;
};
inline Env g_env;

inline std::string seed_without_plots() {
    std::ifstream in(NODES_SEED_PATH);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::printf("  [FAIL] the seed file does not read: %s\n", NODES_SEED_PATH);
        std::exit(2);
    }
    j.erase("plots");
    const std::string path = (std::filesystem::temp_directory_path() / "node_pop_seed_no_plots.json").string();
    std::ofstream(path) << j.dump();
    return path;
}

inline std::unique_ptr<world::CityNodes> fresh_nodes(bool with_plots = false) {
    std::string error;
    auto nodes = world::CityNodes::load(db_path(), error);
    if (!nodes) {
        std::printf("  [FAIL] city nodes: %s\n", error.c_str());
        std::exit(2);
    }
    size_t applied = 0;
    nodes->apply_seed_file(with_plots ? std::string(NODES_SEED_PATH) : seed_without_plots(), error, &applied);
    return nodes;
}

inline size_t people_of(const world::ActorRegistry &registry) {
    return registry.count(world::ActorKind::Npc) + registry.count(world::ActorKind::Station);
}

inline void set_level(world::CityNodes &nodes, int64_t guid, int level) {
    world::LiveNode ln;
    ln.guid = guid;
    ln.level = level;
    nodes.apply_change(ln);
}
inline std::vector<spawns::NodeSpawn> select(const world::CityNodes &nodes, const world::NodeWire &wire,
                                             const world::CityNodeInfo &info, int level, spawns::NodePopStats &st) {
    const spawns::NodeSite site = spawns::node_site_of(nodes, wire, info, level);
    const spawns::SpawnContext ctx = spawns::node_spawn_context(g_env.base, site, g_env.shown);
    const spawns::SpawnFilter filter(ctx, &g_env.table);
    return spawns::select_node_spawns(*g_env.pop, g_env.templates, *g_env.data, wire, site,
                                      spawns::AreaConfig::riverlands(), filter, st);
}
inline std::vector<spawns::NodeSpawn> select_real(const world::CityNodeInfo &info, int level, spawns::NodePopStats &st) {
    return select(*g_env.nodes, *g_env.wire, info, level, st);
}
inline std::vector<spawns::NodeSpawn> select_in_area(const world::CityNodes &nodes, const world::NodeWire &wire,
                                                     const world::CityNodeInfo &info, int level,
                                                     const spawns::AreaConfig &area) {
    spawns::NodePopStats st;
    const spawns::NodeSite site = spawns::node_site_of(nodes, wire, info, level);
    const spawns::SpawnContext ctx = spawns::node_spawn_context(g_env.base, site, g_env.shown);
    const spawns::SpawnFilter filter(ctx, &g_env.table);
    return spawns::select_node_spawns(*g_env.pop, g_env.templates, *g_env.data, wire, site, area, filter, st);
}
inline const tests::NodePopCase *golden_of(const std::string &node, int level) {
    for (size_t i = 0; i < tests::kNodePopCaseCount; ++i) {
        if (node == tests::kNodePopCases[i].node && level == tests::kNodePopCases[i].level)
            return &tests::kNodePopCases[i];
    }
    return nullptr;
}
inline size_t golden_npcs(const std::string &node, int level) {
    const tests::NodePopCase *c = golden_of(node, level);
    return c ? c->spawned_layout + c->spawned_building : 0;
}
inline uint64_t g_next = 8192;
inline FIntrepidNetworkGUID mint() {
    FIntrepidNetworkGUID g;
    g.ObjectId = g_next;
    g_next += 2;
    g.ServerId = 60;
    g.Randomizer = 0x6ee87ee4u;
    return g;
}
struct Rig {
    world::ActorRegistry registry;
    std::vector<uint64_t> spawned, despawned;
    std::unique_ptr<spawns::NodePopulations> pops;
    std::function<void(uint64_t, const content::NpcInstanceRow *, const content::NpcRouteRow *)> on_extra;
    std::function<void(uint64_t, const content::NpcInstanceRow *, const content::NpcRouteRow *,
                       const world::CityNodeInfo &, int64_t)>
        on_spawn;
    Rig(const world::CityNodes &nodes, const world::NodeWire &wire, const spawns::AreaConfig &area,
        std::function<void(uint64_t, const content::NpcInstanceRow *, const content::NpcRouteRow *)> extra = {})
        : on_extra(std::move(extra)) {
        init_pops(nodes, wire, area, g_env.pop, g_env.data.get());
    }
    Rig(const world::CityNodes &nodes, const world::NodeWire &wire, const spawns::AreaConfig &area,
        std::shared_ptr<const content::PopulationData> population, const spawns::NodePopulationData *data) {
        init_pops(nodes, wire, area, std::move(population), data);
    }
    void init_pops(const world::CityNodes &nodes, const world::NodeWire &wire, const spawns::AreaConfig &area,
                   std::shared_ptr<const content::PopulationData> population, const spawns::NodePopulationData *data) {
        spawns::NodePopulations::Hooks hooks;
        hooks.spawned = [this](uint64_t id, const content::NpcInstanceRow *inst, const content::NpcRouteRow *route,
                               const world::CityNodeInfo &node, int64_t plot) {
            spawned.push_back(id);
            if (on_extra) on_extra(id, inst, route);
            if (on_spawn) on_spawn(id, inst, route, node, plot);
        };
        hooks.despawned = [this](uint64_t id) { despawned.push_back(id); };
        spawns::NodePopulations::Config cfg;
        cfg.shown_humanoids = g_env.shown;
        pops = std::make_unique<spawns::NodePopulations>(
            registry, nodes, wire, data ? *data : *g_env.data, population ? population : g_env.pop, g_env.templates,
            g_env.base, &g_env.table, area, [] { return mint(); }, 60, std::move(hooks), cfg);
    }
    size_t npcs_of(int64_t node) const { return pops->actors_of(node).size(); }
};
inline std::set<uint64_t> as_set(const std::vector<uint64_t> &v) {
    return {v.begin(), v.end()};
}
inline bool init_node_pop_db(std::string &error) {
    g_env.db = content::ContentDb::open(db_path(), error);
    if (!g_env.db) {
        std::printf("  [FAIL] the real content.db does not open (%s): %s\n", db_path().c_str(), error.c_str());
        return false;
    }
    auto pop = g_env.db->population_data(error);
    if (!pop) {
        std::printf("  [FAIL] the population chain does not read: %s\n", error.c_str());
        return false;
    }
    g_env.pop = std::make_shared<const content::PopulationData>(std::move(*pop));
    g_env.captured = spawns::CapturedData::load(error);
    if (!g_env.captured) {
        std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
        return false;
    }
    if (!g_env.table.load(motion_path(), error)) {
        std::printf("  [FAIL] the Blueprint table does not load (%s): %s\n", motion_path().c_str(), error.c_str());
        return false;
    }
    g_env.generic.emplace(*g_env.captured, &g_env.table, spawns::GenericConfig{});
    g_env.templates = spawns::TemplateSource{&*g_env.captured, &*g_env.generic};
    return true;
}
inline int init_node_pop_env() {
    spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
    if (!fs::exists(db_path()) || !fs::exists(motion_path())) {
        std::printf("[skip] no content.db or npc_motion.tsv: skipped\n");
        std::exit(0);
    }
    std::string error;
    if (!init_node_pop_db(error)) return 2;
    g_env.nodes = fresh_nodes();
    g_env.wire = world::NodeWire::load(db_path(), *g_env.nodes, error);
    if (!g_env.wire) {
        std::printf("  [FAIL] the node wire does not load: %s\n", error.c_str());
        return 2;
    }
    g_env.shipped_nodes = fresh_nodes(true);
    g_env.shipped_wire = world::NodeWire::load(db_path(), *g_env.shipped_nodes, error);
    if (!g_env.shipped_wire) {
        std::printf("  [FAIL] the node wire of the shipped seed does not load: %s\n", error.c_str());
        return 2;
    }
    g_env.data = spawns::NodePopulationData::load(db_path(), error);
    if (!g_env.data) {
        std::printf("  [FAIL] the node population data does not load: %s\n", error.c_str());
        return 2;
    }
    g_env.base.hour = 11.0;
    g_env.shown = g_env.captured->npc_leaves();
    std::string looks_error;
    const auto combat = g_env.db->combat_rows(looks_error);
    if (combat)
        g_env.looks = spawns::NpcLooks::load(fs::path(motion_path()).parent_path().string(), &g_env.table, &*combat,
                                             &*g_env.captured, looks_error);
    if (!g_env.looks) {
        std::printf("  [FAIL] the people's looks do not load (%s): %s\n", motion_path().c_str(), looks_error.c_str());
        return 2;
    }
    g_env.base.looks_ok = [](const content::NpcAssetRow &a) { return g_env.looks->can_dress(a); };
    return 0;
}
} // namespace aoc::worldd::test_node_pop
