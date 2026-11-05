// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#pragma once

#include "protocol/schema/generated/index.h"
#include "session/player_world.h"
#include "spawns/area_loader.h"
#include "spawns/generic_npc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifndef AREA_CONTENT_DB_PATH
#define AREA_CONTENT_DB_PATH "dist/content/content.db"
#endif

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

inline uint64_t g_next = 4608;

inline aoc::protocol::emit::FIntrepidNetworkGUID mint() {
  aoc::protocol::emit::FIntrepidNetworkGUID g;
  g.ObjectId = g_next;
  g_next += 2;
  g.ServerId = 60;
  g.Randomizer = 0x6ee87ee4u;
  return g;
}

inline const char *chr_n() {
  return "\n";
}

inline std::string ms_since(std::chrono::steady_clock::time_point t) {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count()) + " ms";
}

inline std::string handle_path(const aoc::protocol::schema::ClassSchema &schema, uint16_t h) {
  for (const aoc::protocol::schema::RepCommand &c : schema.commands) {
    if (c.handle == h)
      return c.path;
  }
  return "#" + std::to_string(h);
}

void run_area_vendors_ground(const aoc::worldd::content::ContentDb &db, const aoc::worldd::spawns::CapturedData &captured);
