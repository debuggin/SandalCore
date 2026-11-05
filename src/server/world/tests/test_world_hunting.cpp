// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_world_hunting_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;
namespace ch = capture_hunting;
using std::chrono::milliseconds;
using std::chrono::seconds;
using tsupport::Bits;
using tsupport::bits_of_hex;

static void
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

static void
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

static void check_same_archetype_exports(
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

static void check_hunting_wolf_interactable(
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

static void
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

static void test_template(World &w) {
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

static bool has_living_alignments(const aoc::protocol::repl::Actor &actor,
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

static void check_living_creature_open(World &w,
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

static void test_living_open(World &w) {
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

static void test_stands(World &w) {
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

static void
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

static void check_data_novice_and_riverlands(
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

static void test_data(World &w) {
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

static int64_t find_timid_asset_set(const World &w, int64_t timid_guid) {
  int64_t timid_set = 0;
  for (const auto &[set, members] : w.pop.asset_sets) {
    for (const auto &[asset, weight] : members) {
      if (asset == timid_guid) {
        timid_set = set;
      }
    }
  }
  return timid_set;
}

static bool should_sample_instance(bool in_timid,
                                   const content::NpcInstanceRow &inst,
                                   size_t plain_instances,
                                   size_t radius_instances) {
  if (in_timid) {
    return true;
  }
  if (!inst.override_aggro) {
    return plain_instances < 20;
  }
  return inst.aggro_radius > 0 && radius_instances < 20;
}

struct AggroCounts {
  size_t instances = 0;
  size_t off = 0;
  size_t plain_instances = 0;
  size_t plain_off = 0;
  size_t radius_instances = 0;
  size_t radius_ok = 0;
};

static void tally_aggro_instance(AggroCounts &c, bool in_timid,
                                 const content::NpcInstanceRow &inst,
                                 const world::WorldActor &actor) {
  if (in_timid) {
    ++c.instances;
    c.off += actor.npc.aggro_off && actor.npc.aggro_radius_cm == 0 &&
                     inst.override_aggro && inst.aggro_radius == 0
                 ? 1
                 : 0;
  } else if (!inst.override_aggro) {
    ++c.plain_instances;
    c.plain_off += actor.npc.aggro_off ? 1 : 0;
  } else {
    ++c.radius_instances;
    c.radius_ok +=
        !actor.npc.aggro_off && actor.npc.aggro_radius_cm == inst.aggro_radius
            ? 1
            : 0;
  }
}

static void test_timid_grem_ai() {
  // [C.O.A x C-Y-B]
  world::NpcInfo off_info;
  world::NpcInfo on_info;
  off_info.aggro_off = true;
  CHECK(!combat::notices_players(true, off_info) &&
            combat::notices_players(true, on_info) &&
            !combat::notices_players(false, on_info),
        "a Timid Grem (aggro off) and a huntable (friendly) never notice a "
        "player; a hostile creature with an aggro "
        "radius does");
  npc::ai::Params attack_params;
  npc::ai::Params calm_params;
  calm_params.proximity_aggro = false;
  npc::ai::Sense sense;
  sense.now = 1.0;
  sense.self = {0, 0, 0};
  sense.home = {0, 0, 0};
  npc::ai::Candidate player;
  player.id = 1;
  player.pos = {100, 0, 0};
  player.hostile = true;
  sense.known.push_back(player);
  npc::ai::NpcAi aggressive(attack_params);
  npc::ai::NpcAi timid_ai(calm_params);
  const npc::ai::Orders a1 = aggressive.think(sense);
  npc::ai::Orders a2;
  for (int i = 0; i < 40; ++i) {
    sense.now = 1.0 + 0.25 * i;
    a2 = timid_ai.think(sense);
    if (a2.began_fight || a2.attack) {
      break;
    }
  }
  CHECK(a1.began_fight,
        "(an NPC with proximity aggro starts the fight at once at 1 m)");
  CHECK(!a2.began_fight && !a2.attack &&
            timid_ai.state() == npc::ai::State::Idle,
        "a Timid Grem's AI stays idle for ten seconds with a player standing "
        "one metre away");
  timid_ai.notice(1, 50.0, 12.0);
  sense.now = 12.0;
  const npc::ai::Orders a3 = timid_ai.think(sense);
  CHECK(a3.began_fight, "...and fights back once the player hurts it");
}

static void test_timid_grem_aggro_and_ai(World &w,
                                         const content::NpcAssetRow *timid,
                                         const content::NpcAssetRow *normal,
                                         const spawns::OpenTemplate *tt,
                                         const spawns::OpenTemplate *nt) {
  const int64_t timid_set = find_timid_asset_set(w, timid->guid);
  AggroCounts c;
  std::string why;
  for (const auto &[guid, inst] : w.pop.instances) {
    const bool in_timid =
        std::find(inst.asset_sets.begin(), inst.asset_sets.end(), timid_set) !=
        inst.asset_sets.end();
    if (!should_sample_instance(in_timid, inst, c.plain_instances,
                                c.radius_instances)) {
      continue;
    }
    spawns::DataSpawn s;
    s.x = 1000;
    s.y = 2000;
    s.z = 3000;
    s.level = 2;
    s.instance = &inst;
    s.open = in_timid ? tt : nt;
    s.asset = in_timid ? timid : normal;
    auto actor = spawns::make_data_actor(
        s, [&] { return w.mint(); }, kServerId, 0.0, nullptr, why);
    if (actor) {
      tally_aggro_instance(c, in_timid, inst, *actor);
    }
  }
  CHECK(c.instances >= 31 && c.off == c.instances,
        "all instances of the Timid Grem set (31) override the aggro radius "
        "with 0, and each makes an actor with "
        "aggro_off");
  CHECK(c.plain_instances >= 5 && c.plain_off == 0,
        "an instance with no override keeps the Blueprint's radius (no "
        "aggro_off): the ordinary Rabid still aggros");
  CHECK(c.radius_instances >= 5 && c.radius_ok == c.radius_instances,
        "an instance that overrides with a radius keeps it, and is not "
        "switched off");
  test_timid_grem_ai();
}

static bool
check_timid_grem_combat_relation(World &w, const std::vector<uint64_t> &got,
                                 const std::vector<uint64_t> &hunting_keys) {
  std::string error;
  const auto rows = w.db->combat_rows(error);
  const bootstrap::StatContext ctx = w.world->stat_context();
  auto data = rows ? combat::CombatData::build(*rows, *ctx.bundle, error)
                   : std::nullopt;
  CHECK(data.has_value(), "the combat data builds");
  if (!data) {
    return false;
  }
  const std::vector<int64_t> timid_al(got.begin(), got.end());
  const std::vector<int64_t> hunting_al(hunting_keys.begin(),
                                        hunting_keys.end());
  const bool timid_hostile =
      data->relation(timid_al, combat::CombatData::player_alignments()) ==
      combat::Relation::Hostile;
  const bool hunting_hostile =
      data->relation(hunting_al, combat::CombatData::player_alignments()) ==
      combat::Relation::Hostile;
  CHECK(timid_hostile && !hunting_hostile,
        "the relation table: a Timid Grem is Hostile to players "
        "(NPC_PlayerEnemy), the huntable Grem is not");
  return true;
}

static void check_timid_grem_blocks(const world::WorldActor &timid_actor,
                                    const world::WorldActor &hunting_actor,
                                    const ch::Hostile &rabid) {
  const std::vector<uint64_t> want(rabid.alignments,
                                   rabid.alignments + rabid.alignment_count);
  const std::vector<uint64_t> got = alignment_keys(timid_actor.actor);
  CHECK(got == want, "the Timid Grem lists the capture's Rabid alignments, in "
                     "order: Rabid and NPC_PlayerEnemy "
                     "(6064631663140143148, 6064629454762803230)");
  const std::vector<uint64_t> hunting_keys =
      alignment_keys(hunting_actor.actor);
  CHECK(hunting_keys.size() == 1 && hunting_keys[0] == 6064632310208593979ull &&
            got != hunting_keys,
        "the huntable Grem's one Friendly alignment (HuntingCreature) did not "
        "leak onto the Timid Grem");
  const repl::Subobject *combat = find_sub(timid_actor.actor, "UNPCCombatInfo");
  CHECK(combat && rep_block_of(combat->obj) ==
                      bits_of_hex(rabid.combat.hex, rabid.combat.bits),
        "its CombatInfo is the hostile Rabid's to the bit (18 bits), not the "
        "living huntable's 36");
  const repl::Subobject *ability =
      find_sub(timid_actor.actor, "UAoCAbilityComponent");
  const repl::Subobject *hunting_ability =
      find_sub(hunting_actor.actor, "UAoCAbilityComponent");
  CHECK(ability && hunting_ability &&
            rep_block_of(hunting_ability->obj).size() == 0,
        "the huntable's AbilityComponent is empty (the Timid Grem keeps its "
        "template's)");
}

static void test_timid_grem(World &w) {
  std::printf("a normal Timid Grem (Rabid_Timid) against the retail capture's "
              "hostile Rabid: the same creature to the "
              "client, and its data says it never notices a player\n");
  const content::NpcAssetRow *timid = w.asset("Pop_TimidGrem_RVR");
  const content::NpcAssetRow *hunting = w.asset("Hunting_Rabid");
  const content::NpcAssetRow *normal = w.asset("Pop_Rabid_RVR");
  CHECK(timid && hunting && normal &&
            timid->archetype_path.find("Rabid_Timid") != std::string::npos,
        "the assets are in the data: Pop_TimidGrem_RVR is the Rabid_Timid "
        "Blueprint");
  if (!timid || !hunting || !normal) {
    return;
  }
  spawns::GenericNpcs on(*w.captured, &w.motion, w.config(true));
  spawns::InstanceParams p;
  p.server_id = kServerId;
  p.level = 2;
  std::string why;
  const spawns::OpenTemplate *tt = on.template_for(*timid);
  const spawns::OpenTemplate *ht = on.template_for(*hunting);
  const spawns::OpenTemplate *nt = on.template_for(*normal);
  auto timid_actor = tt ? spawns::instantiate(
                              *tt, [&] { return w.mint(); }, p, why)
                        : std::nullopt;
  auto hunting_actor = ht ? spawns::instantiate(
                                *ht, [&] { return w.mint(); }, p, why)
                          : std::nullopt;
  CHECK(timid_actor.has_value() && hunting_actor.has_value() && nt,
        "a Timid Grem and the huntable Grem are made from their templates");
  if (!timid_actor || !hunting_actor || !nt) {
    return;
  }
  check_timid_grem_blocks(*timid_actor, *hunting_actor, ch::kHostile[0]);
  if (check_timid_grem_combat_relation(w, alignment_keys(timid_actor->actor),
                                       alignment_keys(hunting_actor->actor))) {
    test_timid_grem_aggro_and_ai(w, timid, normal, tt, nt);
  }
}

int main() {
  spdlog::set_level(spdlog::level::off);
  const char *path = db_path();
  if (!path) {
    std::printf("[skip] no content.db (AOC_CONTENT_DB, or "
                "dist/content/content.db): the hunting test needs the "
                "real file\n");
    return 0;
  }
  World w;
  if (!w.open(path)) {
    return 1;
  }
  test_template(w);
  test_living_open(w);
  test_timid_grem(w);
  test_stands(w);
  test_data(w);
  run_hunting_corpse_tests(w);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
