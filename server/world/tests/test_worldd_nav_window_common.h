// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.

#pragma once

#include "nav_chunk_fixture.h"
#include "protocol/repl/codecs.h"
#include "protocol/schema/generated/index.h"
#include "session/player_world.h"

#include <filesystem>
#include <fstream>
#include <thread>

inline int g_pass = 0;
inline int g_fail = 0;

#define CHECK(cond, msg)                 \
  do {                                   \
    if (cond) {                          \
      std::printf("  [ok ] %s\n", msg);  \
      g_pass++;                          \
    } else {                             \
      std::printf("  [FAIL] %s\n", msg); \
      g_fail++;                          \
    }                                    \
  } while (0)

using Clock = std::chrono::steady_clock;

inline double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

inline double cx_of(int j) {
  return aoc::worldd::navfix::chunk_x0(j) +
         aoc::worldd::navfix::kChunkSpan * 0.5;
}

inline void settle(aoc::worldd::nav::AreaNav &n, double seconds = 10) {
  const auto t0 = Clock::now();
  while (n.loading() && ms_since(t0) < seconds * 1000) {
    n.pump(50.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  n.pump(50.0);
}

inline size_t tiles_of(const aoc::worldd::nav::NavFile &f,
                       const std::vector<aoc::worldd::nav::ChunkKey> &keys) {
  size_t n = 0;
  for (const auto &c : f.chunks) {
    for (const auto &k : keys) {
      if (c.cx == k.x && c.cy == k.y) {
        n += c.tiles;
      }
    }
  }
  return n;
}

inline void run_window(aoc::worldd::nav::AreaNav &n,
                       const std::vector<std::pair<double, double>> &players,
                       double seconds = 8) {
  const auto t0 = Clock::now();
  int quiet = 0;
  while (ms_since(t0) < seconds * 1000) {
    const auto before = n.window_stats();
    n.set_interest(players);
    n.pump(20.0);
    const auto after = n.window_stats();
    const bool moved = after.chunk_loads != before.chunk_loads ||
                       after.chunk_unloads != before.chunk_unloads ||
                       after.committed_tiles != before.committed_tiles ||
                       after.resident_tiles != before.resident_tiles;
    quiet = (!n.loading() && !moved) ? quiet + 1 : 0;
    if (quiet >= 30) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

inline const aoc::protocol::schema::ClassSchema &
schema_named(const char *name) {
  const aoc::protocol::schema::ClassSchema *s =
      aoc::protocol::schema::gen::find_class_schema(name);
  if (!s) {
    std::printf("  [FAIL] no schema %s\n", name);
    std::exit(2);
  }
  return *s;
}

inline aoc::worldd::world::WorldActor make_npc(uint64_t id, double x, double y,
                                               double z) {
  aoc::worldd::world::WorldActor w;
  w.id = id;
  w.kind = aoc::worldd::world::ActorKind::Npc;
  w.x = x;
  w.y = y;
  w.z = z;
  aoc::protocol::repl::Actor &a = w.actor;
  aoc::protocol::emit::FIntrepidNetworkGUID g;
  g.ObjectId = id;
  g.ServerId = 60;
  g.Randomizer = 0x6ee87ee4u;
  a.guid = g;
  a.obj.schema = &schema_named("ANPCCharacter");
  aoc::protocol::repl::RepMovement m;
  m.location = aoc::protocol::repl::quantize_location(a.obj.opts, x, y, z);
  m.rotation = aoc::protocol::repl::Rot{0, 64, 0};
  m.linear_velocity = aoc::protocol::repl::QuantVec{
      aoc::protocol::repl::QuantVec::Mode::Scaled, 0, 0, 0};
  a.obj.state.set(*a.obj.schema, "ReplicatedMovement",
                  aoc::protocol::repl::Value(m));
  a.obj.state.set(*a.obj.schema, "ReplicatedServerLastTransformUpdateTimeStamp",
                  aoc::protocol::repl::Value::f32(0.f));
  a.obj.state.set(
      *a.obj.schema, "CurrentMovementInput",
      aoc::protocol::repl::Value(aoc::protocol::repl::PlayerMovementInput{0}));
  a.open.spawn.location = aoc::protocol::repl::quantize_vector(10, x, y, z);
  a.open.spawn.rotation = aoc::protocol::repl::Rot{0, 11000, 0};
  w.npc.name = "test npc";
  return w;
}

void run_nav_window_npcs_tests();
