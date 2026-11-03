// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "capture_node_layout.h"
#include "content_db_fixture.h"
#include "cyb_appearance.h"
#include "fake_client.h"
#include "protocol/repl/block_reader.h"
#include "protocol/schema/generated/index.h"
#include "session/player_session.h"

namespace world = aoc::worldd::world;
namespace conn = aoc::protocol::conn;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
namespace gen = aoc::protocol::schema::gen;
namespace fs = std::filesystem;
namespace cf = content_fixture;
using std::chrono::milliseconds;
using std::chrono::seconds;

inline int g_pass = 0;
inline int g_fail = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (cond) {                                                                \
      std::printf("  [ok ] %s\n", msg);                                        \
      g_pass++;                                                                \
    } else {                                                                   \
      std::printf("  [FAIL] %s\n", msg);                                       \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)
#define CHECKF(cond, ...)                                                      \
  do {                                                                         \
    if (cond) {                                                                \
      g_pass++;                                                                \
    } else {                                                                   \
      std::printf("  [FAIL] ");                                                \
      std::printf(__VA_ARGS__);                                                \
      std::printf("\n");                                                       \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

#ifndef NODES_CONTENT_DB_PATH
#define NODES_CONTENT_DB_PATH "dist/content/content.db"
#endif
#ifndef NODES_SEED_PATH
#define NODES_SEED_PATH "deploy/citynode_seed.json"
#endif

inline std::string db_path() {
  const char *env = std::getenv("AOC_CONTENT_DB");
  return env && *env ? env : NODES_CONTENT_DB_PATH;
}

constexpr int64_t kJoeva = 108646121231993ll, kHalcyon = 108646610851452ll,
                  kWinstead = 108646108518008ll;
constexpr uint32_t kServerId = 60;
constexpr uint32_t kRandomizer = 0x6ee87ee4u;

inline std::unique_ptr<world::CityNodes> g_nodes;
inline std::unique_ptr<world::NodeWire> g_wire;
inline std::unique_ptr<world::NodeLayoutData> g_data;

inline std::vector<repl::NodeLayoutItem>
default_items(int64_t node, int level, int season = world::kSpring) {
  const world::CityNodeInfo *info = g_nodes->by_guid(node);
  world::LayoutRequest req;
  req.node = node;
  req.context.level = level;
  req.context.node_type = info->type;
  req.context.season = season;
  const world::CityZoneInfo *zone = g_nodes->zone_by_guid(info->zoi_guid);
  req.context.biome = zone ? zone->biome : -1;
  return g_data->items(*g_wire, req);
}

struct Scene {
  world::ActorRegistry registry;
  std::unique_ptr<world::CityNodes> nodes;
  aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
  std::unique_ptr<world::NodeScenery> scenery;

  explicit Scene(world::NodeScenery::Config config = {}, bool seeded = true) {
    std::string error;
    nodes = world::CityNodes::load(db_path(), error);
    if (!nodes) {
      std::printf("  [FAIL] no city nodes: %s\n", error.c_str());
      std::exit(2);
    }
    if (seeded && !nodes->apply_seed_file(NODES_SEED_PATH, error)) {
      std::printf("  [FAIL] the seed does not load: %s\n", error.c_str());
      std::exit(2);
    }
    scenery = std::make_unique<world::NodeScenery>(
        registry, *nodes, *g_wire, *g_data, [this] { return guids.next(); },
        config);
  }
  void set_level(int64_t node, int level) {
    world::LiveNode n;
    n.guid = node;
    n.level = level;
    nodes->apply_change(n);
  }
  const world::WorldActor *actor_of(int64_t node) const {
    const auto *e = scenery->entry_of(node);
    return e && e->actor ? registry.find(e->actor) : nullptr;
  }
};

inline int init_node_layout_globals() {
  spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug
                                                : spdlog::level::off);
  std::string error;
  g_nodes = world::CityNodes::load(db_path(), error);
  if (!g_nodes) {
    std::printf("  [FAIL] the real content.db has no city nodes (%s): %s\n",
                db_path().c_str(), error.c_str());
    return 2;
  }
  g_wire = world::NodeWire::load(db_path(), *g_nodes, error);
  if (!g_wire) {
    std::printf("  [FAIL] the node wire does not load (%s): %s\n",
                db_path().c_str(), error.c_str());
    return 2;
  }
  g_data = world::NodeLayoutData::load(db_path(), error);
  if (!g_data) {
    std::printf("  [FAIL] the layout data does not load (%s): %s\n",
                db_path().c_str(), error.c_str());
    return 2;
  }
  return 0;
}
