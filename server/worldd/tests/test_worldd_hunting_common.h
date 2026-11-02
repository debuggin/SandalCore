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

inline void
check_template_golden_blocks(World &w, aoc::worldd::spawns::GenericNpcs &on,
                             const aoc::worldd::spawns::OpenTemplate *cap) {
  const aoc::worldd::content::NpcAssetRow *spider = w.asset("Hunting_Spider");
  const aoc::worldd::content::NpcAssetRow *tide =
      w.asset("Hunting_Tidensapper");
  const aoc::worldd::content::NpcAssetRow *raven = w.asset("Hunting_Raven");
  const char *names[] = {"Hunting_Spider", "Hunting_Tidesnapper"};
  const aoc::worldd::content::NpcAssetRow *assets[] = {spider, tide};
  for (int i = 0; i < 2; ++i) {
    const aoc::worldd::spawns::OpenTemplate *t = on.template_for(*assets[i]);
    const aoc::protocol::repl::ObjectState *block =
        t && t->blocks.size() == 8 ? interactable(*t) : nullptr;
    const capture_hunting::Living *golden = nullptr;
    for (const capture_hunting::Living &l : capture_hunting::kLiving) {
      if (std::string(l.kind) == names[i]) {
        golden = &l;
      }
    }
    CHECK(block && golden &&
              golden->component_def ==
                  static_cast<uint64_t>(assets[i]->interact_component) &&
              golden->interaction_def ==
                  static_cast<uint64_t>(assets[i]->interaction_def) &&
              rep_block_of(*block) ==
                  tsupport::bits_of_hex(golden->block.hex, golden->block.bits),
          (std::string("the ") + names[i] +
           "'s eighth block, made from the Raven's, is the capture's, to the "
           "bit (276 bits)")
              .c_str());
  }
  aoc::worldd::spawns::InstanceParams p;
  p.x = 100;
  p.y = 200;
  p.z = 300;
  p.level = 7;
  p.server_id = kServerId;
  std::string why;
  auto cap_made = cap ? aoc::worldd::spawns::instantiate(
                            *cap, [&] { return w.mint(); }, p, why)
                      : std::nullopt;
  CHECK(cap_made && cap_made->npc.interact_def == raven->interaction_def &&
            cap_made->npc.interact_component != 0,
        "the captured Raven's instance reads its interaction def from its "
        "block too");
}

inline void
test_template_instances(World &w, aoc::worldd::spawns::GenericNpcs &on,
                        const aoc::worldd::spawns::OpenTemplate *hunting_wolf,
                        const aoc::worldd::spawns::OpenTemplate *plain_wolf) {
  const aoc::worldd::content::NpcAssetRow *wolf = w.asset("Hunting_Wolf");
  const aoc::worldd::content::NpcAssetRow *raven = w.asset("Hunting_Raven");
  const aoc::worldd::spawns::TemplateSource source{&*w.captured, &on};
  bool is_generic = true;
  const aoc::worldd::spawns::OpenTemplate *cap =
      source.find(*raven, &is_generic);
  check_template_golden_blocks(w, on, cap);
  CHECK(cap && !is_generic && cap->blocks.size() == 8 &&
            cap == w.captured->template_for(kRavenPath),
        "the Raven's template is the captured open (eight blocks) when a "
        "capture opened it");
  aoc::worldd::spawns::InstanceParams p;
  p.x = 100;
  p.y = 200;
  p.z = 300;
  p.level = 7;
  p.server_id = kServerId;
  std::string why;
  auto made = hunting_wolf
                  ? aoc::worldd::spawns::instantiate(
                        *hunting_wolf, [&] { return w.mint(); }, p, why)
                  : std::nullopt;
  CHECK(made && made->npc.interact_component != 0 &&
            made->npc.interact_def == wolf->interaction_def &&
            made->actor.subobjects.size() == 7,
        "an instance has the component's id in NpcInfo and the asset's "
        "interaction def");
  if (made) {
    const uint64_t id = made->id;
    const uint64_t comp = made->npc.interact_component;
    CHECK(w.registry().add(std::move(*made)), "it joins the registry");
    const aoc::worldd::world::WorldActor *found =
        w.registry().find_by_component(comp);
    CHECK(found && found->id == id, "and find_by_component finds it");
  }
  auto plain_made = plain_wolf
                        ? aoc::worldd::spawns::instantiate(
                              *plain_wolf, [&] { return w.mint(); }, p, why)
                        : std::nullopt;
  CHECK(plain_made && plain_made->npc.interact_def == 0 &&
            plain_made->npc.interact_component == 0,
        "a plain creature has neither");
}

inline void check_same_archetype_exports(
    const aoc::worldd::spawns::OpenTemplate *hunting_wolf,
    const aoc::worldd::spawns::OpenTemplate *test_tpl) {
  if (!hunting_wolf || !test_tpl) {
    return;
  }
  bool same_export =
      hunting_wolf->archetype.size() == test_tpl->archetype.size();
  for (size_t i = 0; same_export && i < hunting_wolf->archetype.size(); ++i) {
    same_export = hunting_wolf->archetype[i].guid.ObjectId ==
                  test_tpl->archetype[i].guid.ObjectId;
  }
  CHECK(same_export,
        "and both name the Blueprint's export by the same NetGUIDs");
}

inline void check_hunting_wolf_interactable(
    const aoc::worldd::spawns::OpenTemplate *hunting_wolf,
    const aoc::worldd::spawns::OpenTemplate *test_tpl,
    const aoc::worldd::content::NpcAssetRow &wolf) {
  check_same_archetype_exports(hunting_wolf, test_tpl);
  const aoc::protocol::repl::ObjectState *o =
      hunting_wolf ? interactable(*hunting_wolf) : nullptr;
  CHECK(o != nullptr, "the Hunting_Wolf's interactable reads");
  if (!o) {
    return;
  }
  const auto comp = int_of(o->state.get(*o->schema, "ComponentDefId.Guid"));
  const aoc::protocol::repl::ElementState *input =
      element(*o, "InteractionInputData", 0);
  const auto def = input && input->values.count(6)
                       ? int_of(&input->values.at(6))
                       : std::nullopt;
  CHECK(comp && *comp == wolf.interact_component && def &&
            *def == wolf.interaction_def,
        "it holds the asset's ComponentDefId (ICD_Hunting_Wolf) and "
        "PlayerInteractDefinition (Hunting_Wolf)");
  bool ones = true;
  for (size_t i = 0; i < 4; ++i) {
    const aoc::protocol::repl::ElementState *s =
        element(*o, "CurrentInputStates", i);
    const auto cur =
        s && s->values.count(1) ? int_of(&s->values.at(1)) : std::nullopt;
    ones = ones && cur && *cur == 1;
  }
  CHECK(ones, "and four interaction states of 1, as a living huntable's");
}

inline void
check_wolf_templates(World &w, aoc::worldd::spawns::GenericNpcs &on,
                     const aoc::worldd::content::NpcAssetRow &wolf,
                     const aoc::worldd::content::NpcAssetRow &test_wolf,
                     const aoc::worldd::content::NpcAssetRow &plain) {
  aoc::worldd::spawns::GenericNpcs off(*w.captured, &w.motion, w.config(false));
  const aoc::worldd::spawns::OpenTemplate *old_wolf = off.template_for(wolf);
  CHECK(old_wolf && old_wolf->blocks.size() == 7 &&
            old_wolf->find("UInteractableComponent") < 0,
        "without the hunting defs a Hunting_Wolf has the seven blocks of a "
        "plain creature (as before)");
  aoc::worldd::spawns::Verdict v = aoc::worldd::spawns::Verdict::NoPath;
  const aoc::worldd::spawns::OpenTemplate *hunting_wolf =
      on.template_for(wolf, &v);
  CHECK(hunting_wolf && v == aoc::worldd::spawns::Verdict::Generic &&
            hunting_wolf->blocks.size() == 8 &&
            hunting_wolf->blocks.back().cls == "UInteractableComponent" &&
            hunting_wolf->blocks.back().kind == 2 &&
            !hunting_wolf->blocks.back().class_chain.empty(),
        "a Hunting_Wolf has eight blocks, the eighth a dynamic "
        "UInteractableComponent with its class export");
  const aoc::worldd::spawns::OpenTemplate *plain_wolf = on.template_for(plain);
  CHECK(plain_wolf && plain_wolf->blocks.size() == 7 &&
            on.template_for(plain) == plain_wolf,
        "a plain Wolf has its seven blocks, and the template is the same "
        "object when asked again");
  const aoc::worldd::spawns::OpenTemplate *test_tpl =
      on.template_for(test_wolf);
  CHECK(test_tpl && test_tpl != hunting_wolf && test_tpl->blocks.size() == 7 &&
            on.template_for(wolf) == hunting_wolf &&
            on.template_for(test_wolf) == test_tpl,
        "HuntingTest (its def is no hunting one) stays a plain creature beside "
        "the Hunting_Wolf, whatever order they "
        "are asked in");
  check_hunting_wolf_interactable(hunting_wolf, test_tpl, wolf);
  test_template_instances(w, on, hunting_wolf, plain_wolf);
}

inline void test_template(World &w) {
  std::printf("the huntable creature's template\n");
  const aoc::worldd::content::NpcAssetRow *wolf = w.asset("Hunting_Wolf");
  const aoc::worldd::content::NpcAssetRow *test_wolf = w.asset("HuntingTest");
  const aoc::worldd::content::NpcAssetRow *plain = w.asset("Pop_Wolf");
  const aoc::worldd::content::NpcAssetRow *spider = w.asset("Hunting_Spider");
  const aoc::worldd::content::NpcAssetRow *tide =
      w.asset("Hunting_Tidensapper");
  const aoc::worldd::content::NpcAssetRow *raven = w.asset("Hunting_Raven");
  CHECK(wolf && test_wolf && plain && spider && tide && raven,
        "the assets are in the data");
  if (!(wolf && test_wolf && plain && spider && tide && raven)) {
    return;
  }
  CHECK(wolf->archetype_path == test_wolf->archetype_path &&
            wolf->interaction_def != test_wolf->interaction_def,
        "(Hunting_Wolf and HuntingTest share one Blueprint and differ in their "
        "interaction)");
  // C:Y:B - CoA
  aoc::worldd::spawns::GenericNpcs on(*w.captured, &w.motion, w.config(true));
  CHECK(w.hunting_defs.size() == 53, "53 hunting interaction defs are known");
  check_wolf_templates(w, on, *wolf, *test_wolf, *plain);
}

inline bool has_living_alignments(const aoc::protocol::repl::Actor &actor,
                                  uint64_t want_align) {
  constexpr uint64_t kSelf = 6064630243140894720ull;
  const aoc::protocol::repl::Subobject *al =
      find_sub(actor, "UAlignmentComponent");
  const aoc::protocol::repl::Value *av =
      al ? al->obj.state.get(*al->obj.schema, "Alignments") : nullptr;
  const auto *cont =
      av ? av->get_if<aoc::protocol::repl::AlignmentContainer>() : nullptr;
  if (!cont || cont->instances.size() != 2) {
    return false;
  }
  bool one_friendly = true;
  bool own = false;
  for (const auto &inst : cont->instances) {
    if (inst.key == kSelf) {
      own = true;
    } else {
      one_friendly = one_friendly && inst.key == want_align;
    }
  }
  return one_friendly && own;
}

inline void check_living_creature_open(World &w,
                                       aoc::worldd::spawns::GenericNpcs &on,
                                       const capture_hunting::Living &l) {
  const aoc::worldd::content::NpcAssetRow *asset = nullptr;
  for (const auto &[guid, row] : w.pop.assets) {
    if (static_cast<uint64_t>(row.interaction_def) == l.interaction_def &&
        !asset) {
      asset = &row;
    }
  }
  const aoc::worldd::spawns::OpenTemplate *tpl =
      asset ? on.template_for(*asset) : nullptr;
  aoc::worldd::spawns::InstanceParams p;
  p.server_id = kServerId;
  p.level = 12;
  std::string why;
  auto made = tpl ? aoc::worldd::spawns::instantiate(
                        *tpl, [&] { return w.mint(); }, p, why)
                  : std::nullopt;
  const std::string label = std::string("the ") + l.kind;
  CHECK(made.has_value(), (label + " is made from a generic template").c_str());
  if (!made) {
    return;
  }
  CHECK(
      name_of(made->actor) == l.name,
      (label +
       "'s UNPCInfo.CharacterName is the capture's (the nameplate's name: \"" +
       l.name + "\")")
          .c_str());
  CHECK(has_living_alignments(made->actor, l.alignment),
        (label + "'s alignments are the capture's: the one Friendly Blueprint "
                 "alignment and its own record (a "
                 "hostile Raven of the same instance lists two others)")
            .c_str());
  const aoc::protocol::repl::Subobject *combat =
      find_sub(made->actor, "UNPCCombatInfo");
  CHECK(combat && rep_block_of(combat->obj) ==
                      tsupport::bits_of_hex(l.combat.hex, l.combat.bits),
        (label + "'s CombatInfo is the capture's to the bit (bIsDead, "
                 "bInvulnerable, bNotTargetable written: 36 bits)")
            .c_str());
  const aoc::protocol::repl::Subobject *ability =
      find_sub(made->actor, "UAoCAbilityComponent");
  CHECK(ability && l.ability.bits == 0 &&
            rep_block_of(ability->obj).size() == 0,
        (label +
         "'s AbilityComponent is the capture's: nothing (it fights nothing)")
            .c_str());
}

inline void test_living_open(World &w) {
  std::printf("a living huntable's open against the captures: what makes the "
              "client draw it friendly\n");
  aoc::worldd::spawns::GenericNpcs on(*w.captured, &w.motion, w.config(true));
  for (const capture_hunting::Living &l : capture_hunting::kLiving) {
    check_living_creature_open(w, on, l);
  }
  const std::pair<const char *, const char *> known[] = {
      {"Hunting_Raven", "Raven"},
      {"Hunting_Spider", "Spider"},
      {"Hunting_Tidesnapper", "Tidesnapper"},
      {"Hunting_Bear", "Bear"},
      {"Hunting_PallasCat", "Pallas Cat"},
      {"Hunting_Rabid", "Grem"}};
  for (const auto &[kind, want] : known) {
    std::string got;
    for (const auto &[def, k] : w.g->hunting()) {
      if (k.name == kind) {
        got = w.hunting_names.count(def) ? w.hunting_names.at(def)
                                         : std::string("(none)");
      }
    }
    CHECK(got == want, (std::string(kind) + " is named \"" + want +
                        "\" (got \"" + got + "\")")
                           .c_str());
  }
  CHECK(w.hunting_names.size() == w.hunting_defs.size(),
        "every hunting def has a nameplate name");
}
// (C-Y-B ~ CoA)

inline void test_stands(World &w) {
  std::printf("a huntable creature stands: no wander, no patrol\n");
  const aoc::worldd::content::NpcAssetRow *wolf = w.asset("Hunting_Wolf");
  const aoc::worldd::content::NpcAssetRow *plain = w.asset("Pop_Wolf");
  if (!wolf || !plain) {
    CHECK(false, "the assets are in the data");
    return;
  }
  aoc::worldd::spawns::GenericNpcs on(*w.captured, &w.motion, w.config(true));
  aoc::worldd::spawns::InstanceParams p;
  p.server_id = kServerId;
  std::string why;
  const aoc::worldd::spawns::OpenTemplate *hw = on.template_for(*wolf);
  const aoc::worldd::spawns::OpenTemplate *pw = on.template_for(*plain);
  auto hunt = hw ? aoc::worldd::spawns::instantiate(
                       *hw, [&] { return w.mint(); }, p, why)
                 : std::nullopt;
  auto pl = pw ? aoc::worldd::spawns::instantiate(
                     *pw, [&] { return w.mint(); }, p, why)
               : std::nullopt;
  if (!hunt || !pl) {
    CHECK(false, "both creatures are made");
    return;
  }
  aoc::worldd::content::NpcInstanceRow inst;
  inst.ambient_behavior = "Default";
  aoc::worldd::npc::SpeedModel standing;
  standing.standing_defs = &w.hunting_defs;
  CHECK(aoc::worldd::npc::make_setup(*hunt, w.motion, &inst, nullptr,
                                     aoc::worldd::npc::SpeedModel{})
                .ambient == aoc::worldd::npc::Ambient::Wander,
        "(without the rule an instance of Default wanders)");
  CHECK(aoc::worldd::npc::make_setup(*hunt, w.motion, &inst, nullptr, standing)
                .ambient == aoc::worldd::npc::Ambient::Stationary,
        "a huntable creature of that instance stands");
  inst.ambient_behavior = "Patrol";
  CHECK(aoc::worldd::npc::make_setup(*hunt, w.motion, &inst, nullptr, standing)
                .ambient == aoc::worldd::npc::Ambient::Stationary,
        "and a huntable of a Patrol instance stands too");
  inst.ambient_behavior = "Default";
  CHECK(aoc::worldd::npc::make_setup(*pl, w.motion, &inst, nullptr, standing)
                .ambient == aoc::worldd::npc::Ambient::Wander,
        "a plain Wolf of the same instance still wanders");
}

inline void
check_data_nothing_and_one_of(World &w, size_t nothing,
                              const std::set<std::string> &nothing_names) {
  char msg[200];
  std::snprintf(msg, sizeof msg,
                "%zu kinds give nothing: the RETIRED_ and NOTUSED_ items (Ebon "
                "Omen, Kirin, Midnight Mare, Obsidian Stallion, "
                "Plumed Haleberder, Runic Steed, Terrapin), an empty table "
                "(Shell), a weight of 0 (Wyrmling), predicates on "
                "the creature's wounds (the CanTame prototype)",
                nothing);
  CHECK(nothing == 10 && nothing_names.count("Hunting_ShellOfTheAncients") &&
            nothing_names.count("Hunting_Kirin") &&
            nothing_names.count("Hunting_MidnightMare") &&
            nothing_names.count("Hunting_Wyrmling") &&
            nothing_names.count("Hunting_Grem_Prototype_CanTame"),
        msg);
  std::snprintf(msg, sizeof msg,
                "the data's NumberOfSubtablesToSelect of 2 is read as 1 for "
                "the %zu roots with two subtables (a carcass or a "
                "trainable)",
                w.g->hunting_one_of());
  CHECK(w.g->hunting_one_of() >= 10 && w.g->hunting_one_of() <= 30, msg);
}

inline void check_data_novice_and_riverlands(
    const std::map<int64_t, aoc::worldd::gather::Gathering::HuntKind> &kinds) {
  for (const char *name : {"Hunting_Wolf", "Hunting_Raven", "Hunting_Spider",
                           "Hunting_Rabid", "Hunting_Bear", "Hunting_Horse"}) {
    bool found = false;
    for (const auto &[def, k] : kinds) {
      found = found ||
              (k.name == name && !k.gives_nothing && k.row->cert_tier == 0);
    }
    CHECK(found,
          (std::string(name) + " is a Novice kind that gives items").c_str());
  }
  const char *riverlands[] = {
      "Hunting_Wolf",           "Hunting_Bear",        "Hunting_Rabid",
      "Hunting_SilkwormLarvae", "Hunting_Otter",       "Hunting_Horse",
      "Hunting_Raven",          "Hunting_Spider",      "Hunting_Bullywog",
      "Hunting_PallasCat",      "Hunting_Gryphon",     "Hunting_Deer",
      "Hunting_Bork",           "Hunting_Tidesnapper", "Hunting_Skinwalker",
      "Hunting_Swiftclaw",      "Hunting_TerrorBird"};
  size_t present = 0;
  for (const char *name : riverlands) {
    for (const auto &[def, k] : kinds) {
      present += k.name == name && !k.gives_nothing ? 1 : 0;
    }
  }
  CHECK(present == 17 || present == 16,
        "the Riverlands' kinds (17 spawn today) are all there and give items");
}

inline void test_data(World &w) {
  std::printf("the data: the 53 hunting interactions\n");
  const auto &kinds = w.g->hunting();
  CHECK(kinds.size() == 53, "53 hunting kinds");
  size_t tier[5] = {0, 0, 0, 0, 0};
  size_t bow = 0, fifteen = 0, reach = 0, loaded = 0, nothing = 0;
  std::set<std::string> nothing_names;
  for (const auto &[def, k] : kinds) {
    const aoc::worldd::content::GatherDefRow &row = *k.row;
    if (row.cert_tier >= 0 && row.cert_tier <= 4) {
      ++tier[row.cert_tier];
    }
    bow += row.tool_slot == 38 && row.profession_guid ==
                                      aoc::worldd::content::kProfessionHunting
               ? 1
               : 0;
    fifteen += row.base_duration == 15.0 ? 1 : 0;
    reach += row.interact_radius == 200.0 && row.interact_tolerance == 30.0 &&
                     aoc::worldd::gather::reach_cm(row) == 330.0
                 ? 1
                 : 0;
    loaded += w.g->interaction(row.guid) == &row ? 1 : 0;
    if (k.gives_nothing) {
      ++nothing;
      nothing_names.insert(k.name);
    }
  }
  CHECK(tier[0] == 13 && tier[1] == 22 && tier[2] == 18,
        "Novice 13, Apprentice 22, Journeyman 18");
  CHECK(bow == 53 && fifteen == 53,
        "all use the Bow (slot 38) of the Hunting profession, 15 s base");
  CHECK(reach == 53, "radius 200 cm and tolerance 30 cm: a start reaches 330 "
                     "cm (with the leeway)");
  CHECK(loaded == 53, "each is a gather interaction Gathering holds");
  check_data_nothing_and_one_of(w, nothing, nothing_names);
  check_data_novice_and_riverlands(kinds);
}
