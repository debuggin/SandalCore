// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#pragma once

#include "capture_hunting.h"
#include "combat/npc_replicator.h"
#include "gather/rules.h"
#include "protocol/repl/block_reader.h"
#include "session/player_world.h"
#include "test_support.h"
#include "world/interaction_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif
// (C Y B) (C.O.A)

inline int g_pass = 0;
inline int g_fail = 0;
#undef CHECK
inline void CHECK(bool cond, const char *msg) {
  if (cond) {
    std::printf("  [ok ] %s\n", msg);
    g_pass++;
  } else {
    std::printf("  [FAIL] %s\n", msg);
    g_fail++;
  }
}

inline constexpr uint32_t kServerId = 60;
inline constexpr uint32_t kRandomizer = 0x6ee87ee4u;
inline const aoc::worldd::Clock::time_point t0{std::chrono::seconds(5000)};

inline const char *db_path() {
  if (!std::filesystem::exists(NPC_MOTION_PATH)) {
    return nullptr;
  }
  const char *env = std::getenv("AOC_CONTENT_DB");
  if (env && *env) {
    return env;
  }
#ifdef HUNT_CONTENT_DB_PATH
  if (std::filesystem::exists(HUNT_CONTENT_DB_PATH)) {
    return HUNT_CONTENT_DB_PATH;
  }
#endif
  return nullptr;
}

inline const char *kRavenPath =
    "/Game/DesignAssets/Artisanship/Gathering/Hunting/Hunting_Creatures/"
    "Hunting_Raven.Default__Hunting_Raven_C";

struct World {
  std::optional<aoc::worldd::content::ContentDb> db;
  aoc::protocol::ServerGuidAllocator guids{kServerId, kRandomizer};
  std::unique_ptr<aoc::worldd::PlayerWorld> world;
  std::optional<aoc::worldd::spawns::CapturedData> captured;
  aoc::worldd::npc::MotionTable motion;
  aoc::worldd::content::PopulationData pop;
  std::set<int64_t> hunting_defs;
  std::map<int64_t, std::string> hunting_names;
  aoc::worldd::gather::Gathering *g = nullptr;

  static bool fail(const char *label, const std::string &err) {
    std::printf("  [FAIL] %s: %s\n", label, err.c_str());
    return false;
  }
  bool open(const char *path) {
    std::string error;
    db = aoc::worldd::content::ContentDb::open(path, error);
    if (!db) {
      return fail("the content.db does not open", error);
    }
    world = std::make_unique<aoc::worldd::PlayerWorld>(
        &*db, "", aoc::worldd::bootstrap::GameStateMode::Tier2, kServerId,
        guids);
    if (!world->ready()) {
      return fail("the world is not ready", world->content_problem());
    }
    auto c = aoc::worldd::spawns::CapturedData::load(error);
    if (!c) {
      return fail("the captured spawns do not load", error);
    }
    captured = std::move(*c);
    if (!motion.load(NPC_MOTION_PATH, error)) {
      return fail("the Blueprint table does not load", error);
    }
    auto p = db->population_data(error);
    if (!p) {
      return fail("the population does not read", error);
    }
    pop = std::move(*p);
    aoc::worldd::gather::Config cfg;
    if (!world->start_gathering(cfg, error)) {
      return fail("gathering does not start", error);
    }
    g = world->gathering();
    for (const auto &[def, kind] : g->hunting()) {
      hunting_defs.insert(def);
    }
    std::string fe;
    if (const auto foliage = db->foliage_data(fe)) {
      hunting_names = aoc::worldd::gather::hunting_names(*foliage);
    }
    return true;
  }
  aoc::worldd::world::ActorRegistry &registry() { return world->registry(); }
  aoc::protocol::emit::FIntrepidNetworkGUID mint() {
    return world->mint_guid();
  }
  // [Cyb :: C O A]

  const aoc::worldd::content::NpcAssetRow *
  asset(const std::string &name) const {
    for (const auto &[guid, a] : pop.assets) {
      if (a.name == name) {
        return &a;
      }
    }
    return nullptr;
  }
  aoc::worldd::spawns::GenericConfig config(bool hunting) const {
    aoc::worldd::spawns::GenericConfig c;
    if (hunting) {
      c.hunting_defs = hunting_defs;
      c.hunting_names = hunting_names;
    }
    return c;
  }
  std::unique_ptr<aoc::worldd::stats::CharacterStats>
  make_stats(int level = 25) {
    const aoc::worldd::bootstrap::StatContext ctx = world->stat_context();
    auto cs = std::make_unique<aoc::worldd::stats::CharacterStats>(
        *ctx.bundle, *ctx.gear, 2, level, ctx.rounding);
    std::string err;
    if (!cs->start({}, err)) {
      std::printf("  [FAIL] the character's stats do not start: %s\n",
                  err.c_str());
      std::exit(2);
    }
    return cs;
  }
};

inline const aoc::protocol::repl::ElementState *
element(const aoc::protocol::repl::ObjectState &o, const char *path,
        size_t index) {
  const aoc::protocol::repl::Value *v = o.state.get(*o.schema, path);
  const auto *a = v ? v->get_if<aoc::protocol::repl::ArrayValue>() : nullptr;
  return a && index < a->elements.size() ? &a->elements[index] : nullptr;
}

inline std::optional<int64_t> int_of(const aoc::protocol::repl::Value *v) {
  if (!v) {
    return std::nullopt;
  }
  if (const int64_t *i = v->get_if<int64_t>()) {
    return *i;
  }
  if (const uint64_t *u = v->get_if<uint64_t>()) {
    return static_cast<int64_t>(*u);
  }
  return std::nullopt;
}

inline bool payload_is(const aoc::protocol::repl::ContentBlock &b,
                       const capture_hunting::Block &want) {
  if (b.has_rep_layout != want.rep) {
    return false;
  }
  const tsupport::Bits mine(b.payload);
  const tsupport::Bits theirs = tsupport::bits_of_hex(want.hex, want.bits);
  if (mine == theirs) {
    return true;
  }
  size_t at = 0;
  while (at < mine.size() && at < theirs.size() && mine.b[at] == theirs.b[at]) {
    ++at;
  }
  std::printf("      (differs: %zu bits against the capture's %zu, the first "
              "difference at bit %zu)\n",
              mine.size(), theirs.size(), at);
  return false;
}

inline std::optional<aoc::protocol::repl::ObjectState>
load(const capture_hunting::Block &b, const char *cls) {
  return aoc::protocol::repl::load_object_state_hex(tsupport::schema_named(cls),
                                                    b.hex, b.bits, b.rep);
}

inline const aoc::protocol::repl::ObjectState *
interactable(const aoc::worldd::spawns::OpenTemplate &t) {
  const int i = t.find("UInteractableComponent");
  return i < 0 ? nullptr : &t.blocks[static_cast<size_t>(i)].state;
}

inline tsupport::Bits rep_block_of(const aoc::protocol::repl::ObjectState &o) {
  aoc::protocol::repl::RepShadow shadow;
  aoc::protocol::emit::BunchWriter w;
  bool failed = false;
  if (aoc::protocol::repl::write_rep_block(*o.schema, o.state, shadow, w, {},
                                           &failed) == 0 ||
      failed) {
    return tsupport::Bits();
  }
  return tsupport::Bits(w);
}

inline std::string name_of(const aoc::protocol::repl::Actor &a) {
  const aoc::protocol::repl::Subobject *info = nullptr;
  for (const aoc::protocol::repl::Subobject &s : a.subobjects) {
    if (s.obj.schema && std::string(s.obj.schema->name) == "UNPCInfo") {
      info = &s;
    }
  }
  const aoc::protocol::repl::Value *v =
      info ? info->obj.state.get(*info->obj.schema, "CharacterName") : nullptr;
  const auto *s = v ? v->get_if<std::string>() : nullptr;
  return s ? *s : std::string();
}

inline const aoc::protocol::repl::Subobject *
find_sub(const aoc::protocol::repl::Actor &a, const char *cls) {
  for (const aoc::protocol::repl::Subobject &s : a.subobjects) {
    if (s.obj.schema && std::string(s.obj.schema->name) == cls) {
      return &s;
    }
  }
  return nullptr;
}

inline std::vector<uint64_t>
alignment_keys(const aoc::protocol::repl::Actor &a) {
  std::vector<uint64_t> out;
  const aoc::protocol::repl::Subobject *al = find_sub(a, "UAlignmentComponent");
  const aoc::protocol::repl::Value *av =
      al ? al->obj.state.get(*al->obj.schema, "Alignments") : nullptr;
  const auto *cont =
      av ? av->get_if<aoc::protocol::repl::AlignmentContainer>() : nullptr;
  if (!cont) {
    return out;
  }
  for (const auto &inst : cont->instances) {
    if (inst.key != 6064630243140894720ull) {
      out.push_back(inst.key);
    }
  }
  return out;
}

inline const aoc::protocol::repl::Subobject *
sub_of(const aoc::protocol::repl::Actor &a, const char *cls) {
  return find_sub(a, cls);
}

inline aoc::protocol::repl::Actor corpse_view_of(World &w, uint64_t id,
                                                 uint64_t player) {
  const aoc::worldd::world::WorldActor *a = w.registry().find(id);
  aoc::protocol::repl::Actor v = a->actor;
  aoc::worldd::gather::show_interacting_player(*a, player, v);
  return v;
}

void run_hunting_corpse_tests(World &w);
