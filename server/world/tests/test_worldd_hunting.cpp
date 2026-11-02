// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_hunting_common.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace emit = aoc::protocol::emit;
namespace ch = capture_hunting;
using std::chrono::milliseconds;
using std::chrono::seconds;
using tsupport::Bits;
using tsupport::bits_of_hex;

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

// == C-O-A | Cyb ==

static bool is_retired_harvest(World &w,
                               const std::optional<gather::Harvest> &h) {
  if (!h) {
    return false;
  }
  bool retired_only = true;
  for (const combat::RolledItem &it : h->items) {
    std::string err;
    const auto rec = w.db->item(it.item, err);
    retired_only = retired_only && rec &&
                   (rec->name.rfind("RETIRED_", 0) == 0 ||
                    rec->name.rfind("NOTUSED_", 0) == 0);
  }
  return retired_only;
}

static const gather::Gathering::HuntKind *find_hunt_kind(World &w,
                                                         const char *name) {
  for (const auto &[def, k] : w.g->hunting()) {
    if (k.name == name) {
      return &k;
    }
  }
  return nullptr;
}

static const ch::FrameEvent *find_frame_event(int capture) {
  for (const ch::FrameEvent &e : ch::kFrameEvents) {
    if (e.capture == capture) {
      return &e;
    }
  }
  return nullptr;
}

static bool roll_matches_capture(World &w,
                                 const gather::Gathering::HuntKind &kind,
                                 const ch::FrameEvent &ev,
                                 stats::CharacterStats::Evaluation &run) {
  std::string why;
  stats::SplitMix rng;
  rng.state = 99;
  bool found = false;
  for (int i = 0; i < 400 && !found; ++i) {
    const auto h =
        w.g->harvest(*kind.row, run, 25, world::NodeSite{}, nullptr, rng, why);
    for (const combat::RolledItem &it :
         h ? h->items : std::vector<combat::RolledItem>{}) {
      found = found || (static_cast<uint64_t>(it.item) == ev.event.record &&
                        it.quantity == static_cast<int64_t>(ev.event.quantity));
    }
  }
  return found;
}

static void test_rolls_captures(World &w,
                                stats::CharacterStats::Evaluation &run) {
  struct Cap {
    int capture;
    const char *kind;
  };
  const Cap caps[] = {{1456, "Hunting_Bear"},
                      {1457, "Hunting_Raven"},
                      {1458, "Hunting_Wolf"},
                      {1178, "Hunting_Ram"},
                      {1179, "Hunting_Hawk"}};
  for (const Cap &c : caps) {
    const gather::Gathering::HuntKind *kind = find_hunt_kind(w, c.kind);
    const ch::FrameEvent *ev = find_frame_event(c.capture);
    const bool found = kind && ev && roll_matches_capture(w, *kind, *ev, run);
    char what[160];
    std::snprintf(what, sizeof what,
                  "%d (%s): the captured item x%u is among the kind's rolls",
                  c.capture, c.kind, ev ? ev->event.quantity : 0u);
    CHECK(found, what);
  }
  std::string why;
  const gather::Gathering::HuntKind *any = find_hunt_kind(w, "Hunting_Wolf");
  const auto secs =
      any ? w.g->channel_seconds(*any->row, run, why) : std::nullopt;
  char msg[160];
  std::snprintf(msg, sizeof msg,
                "the channel at rating 0 is %.2f s (base 15 s, the captured "
                "hunts: 13.9 to 14.8 s with their ratings)",
                secs ? *secs : -1.0);
  CHECK(secs && *secs >= 13.5 && *secs <= 15.0, msg);
}

static void
test_rolls_empty_and_captures(World &w,
                              stats::CharacterStats::Evaluation &run) {
  std::string why;
  size_t empty_ok = 0;
  size_t empty_n = 0;
  for (const auto &[def, k] : w.g->hunting()) {
    if (!k.gives_nothing) {
      continue;
    }
    ++empty_n;
    stats::SplitMix rng;
    rng.state = 5;
    const auto h =
        w.g->harvest(*k.row, run, 25, world::NodeSite{}, nullptr, rng, why);
    empty_ok += is_retired_harvest(w, h) ? 1 : 0;
  }
  CHECK(empty_n == 10 && empty_ok == 10,
        "the ten that give nothing roll only retired items (or none): the "
        "session drops them");
  test_rolls_captures(w, run);
}

struct KindRollVerdict {
  bool items = true;
  bool xp = true;
  bool adv = true;
};

static void log_roll_failure(const gather::Gathering::HuntKind &k,
                             const KindRollVerdict &v, size_t empty_rolls,
                             const std::set<int64_t> &xps) {
  if (v.items && v.xp && v.adv) {
    return;
  }
  std::printf("      (%s: items %d, xp %d, adventuring %d, empty rolls %zu, "
              "tier %d, xp*1000+rarity seen:",
              k.name.c_str(), v.items ? 1 : 0, v.xp ? 1 : 0, v.adv ? 1 : 0,
              empty_rolls, k.row->cert_tier);
  for (const int64_t val : xps) {
    std::printf(" %lld", static_cast<long long>(val));
  }
  std::printf(")\n");
}

static KindRollVerdict roll_kind_300(World &w, int64_t def,
                                     const gather::Gathering::HuntKind &k,
                                     stats::CharacterStats::Evaluation &run) {
  std::string why;
  stats::SplitMix rng;
  rng.state = 11 + static_cast<uint64_t>(def & 0xffff);
  KindRollVerdict v;
  std::set<int64_t> xps;
  size_t empty_rolls = 0;
  const double base = k.row->cert_tier == 0   ? 160.0
                      : k.row->cert_tier == 1 ? 200.0
                                              : 240.0;
  for (int i = 0; i < 300; ++i) {
    const auto h =
        w.g->harvest(*k.row, run, 25, world::NodeSite{}, nullptr, rng, why);
    if (!h) {
      v.items = false;
      break;
    }
    xps.insert(h->profession_xp * 1000 + h->primary_rarity);
    empty_rolls += h->items.empty() ? 1 : 0;
    v.items = v.items && !h->items.empty() && h->items.size() <= 2;
    for (const combat::RolledItem &it : h->items) {
      v.items = v.items && it.quantity >= 1 && it.quantity <= 20;
    }
    v.xp = v.xp &&
           h->profession_xp ==
               static_cast<int64_t>(base * (1.0 + 0.5 * h->primary_rarity));
    v.adv = v.adv && h->adventuring_xp > 0;
  }
  log_roll_failure(k, v, empty_rolls, xps);
  return v;
}

static void test_rolls(World &w) {
  std::printf("the rolls: every kind that gives, the captured items\n");
  auto owner = w.make_stats();
  std::string why;
  auto run = owner->evaluation({}, why);
  if (!run) {
    std::printf("  [FAIL] the stat run does not start: %s\n", why.c_str());
    return;
  }
  w.g->set_ratings({});
  size_t checked = 0;
  size_t items_ok = 0;
  size_t xp_ok = 0;
  size_t adv_ok = 0;
  for (const auto &[def, k] : w.g->hunting()) {
    if (k.gives_nothing) {
      continue;
    }
    ++checked;
    const KindRollVerdict v = roll_kind_300(w, def, k, *run);
    items_ok += v.items ? 1 : 0;
    xp_ok += v.xp ? 1 : 0;
    adv_ok += v.adv ? 1 : 0;
  }
  char msg[160];
  std::snprintf(msg, sizeof msg,
                "the %zu kinds that give: 300 rolls each, every roll gives an "
                "item (1 to 20), of one subtable (one or two "
                "stacks)",
                checked);
  CHECK(checked == 43 && items_ok == checked, msg);
  CHECK(xp_ok == checked,
        "and Hunter XP of 160 (Novice), 200 (Apprentice) or 240 (Journeyman) "
        "for a Common item, +50 % of that for "
        "each better rarity: once, not once for each subtable");
  CHECK(adv_ok == checked, "and Adventuring XP");
  test_rolls_empty_and_captures(w, *run);
}
// C O A with C_Y_B

static void check_corpse_wire_after_done(World &w, gather::NodeBoard &board,
                                         uint64_t id,
                                         repl::ActorShadow &shadow) {
  const world::WorldActor *done = w.registry().find(id);
  float health = -1;
  const bool have_health =
      done && combat::read_stat(*done, content::kGuidHealth, health);
  CHECK(!have_health || health == 0.0f,
        "and Health is 0 (an item of the stats, when the actor has one)");
  CHECK(board.state_of(id) == gather::NodeState::Gathered &&
            !gather::node_gathered(*done),
        "the corpse is Gathered for the board and still has no bGathered");
  std::vector<repl::ContentBlock> update;
  CHECK(repl::collect_update_blocks(corpse_view_of(w, id, 7), shadow, update) &&
            update.empty(),
        "nothing changes after");
  const repl::Actor other = corpse_view_of(w, id, 99);
  const repl::Subobject *comp = sub_of(other, "UInteractableComponent");
  const repl::ElementState *e =
      comp ? element(comp->obj, "InputEffectStatuses", 0) : nullptr;
  const auto it = e ? e->values.find(3) : decltype(e->values.end()){};
  const auto *pg = e && it != e->values.end()
                       ? it->second.get_if<emit::FIntrepidNetworkGUID>()
                       : nullptr;
  CHECK(pg && pg->ObjectId == 0,
        "another player's connection sees InteractingPlayer as none");
}

static void check_corpse_wire_done_blocks(World &w, gather::NodeBoard &board,
                                          uint64_t id, Clock::time_point now,
                                          repl::ActorShadow &shadow) {
  std::string why;
  CHECK(board.complete(id, now, seconds(3600), why),
        "the board completes the channel");
  repl::Actor view = corpse_view_of(w, id, 7);
  std::vector<repl::ContentBlock> update;
  CHECK(repl::collect_update_blocks(view, shadow, update),
        "the done update builds");
  bool inter_ok = false;
  bool combat_ok = false;
  for (const repl::ContentBlock &b : update) {
    if (b.payload.bit_pos() == ch::kRun1457Done.bits) {
      inter_ok = payload_is(b, ch::kRun1457Done);
    }
    if (b.payload.bit_pos() == ch::kRun1457DoneCombat.bits) {
      combat_ok = payload_is(b, ch::kRun1457DoneCombat);
    }
  }
  CHECK(inter_ok,
        "the interactable's block is the capture's done update, to the bit "
        "(370 bits: the status 0xD0000004, the "
        "pawn, nobody channels, CanInputActivate, all four states 0)");
  CHECK(combat_ok, "the corpse's CombatInfo block is the capture's (27 bits: "
                   "bIsDead 1, bNotTargetable 0)");
  check_corpse_wire_after_done(w, board, id, shadow);
}

static void test_corpse_wire_beats_and_done(World &w, gather::NodeBoard &board,
                                            uint64_t id,
                                            repl::ActorShadow &shadow) {
  std::vector<repl::ContentBlock> update;
  size_t same = 0;
  size_t one_block = 0;
  Clock::time_point now = t0;
  for (size_t i = 0; i < ch::kRun1457BeatCount; ++i) {
    now += milliseconds(67);
    if (!board.beat(id, now)) {
      break;
    }
    repl::Actor view = corpse_view_of(w, id, 7);
    update.clear();
    if (!repl::collect_update_blocks(view, shadow, update)) {
      break;
    }
    if (update.size() != 1) {
      continue;
    }
    ++one_block;
    same += payload_is(update[0], ch::kRun1457Beats[i].block) ? 1 : 0;
  }
  CHECK(one_block == ch::kRun1457BeatCount,
        "every heartbeat is one block: the interactable's");
  CHECK(same == ch::kRun1457BeatCount,
        "all 204 heartbeats are the capture's, to the bit (the counter +2 "
        "each; the first, 217 bits, also takes "
        "InteractingPlayer off)");
  check_corpse_wire_done_blocks(w, board, id, now + milliseconds(36), shadow);
}

static void mark_corpse_not_targetable(World &w, uint64_t id) {
  w.registry().modify(id, world::Change::State, [&](world::WorldActor &a) {
    for (repl::Subobject &s : a.actor.subobjects) {
      if (s.obj.schema && std::string(s.obj.schema->name) == "UNPCCombatInfo") {
        s.obj.state.set(*s.obj.schema, "bNotTargetable",
                        repl::Value::boolean(true));
      }
    }
  });
}

static void start_and_beat_corpse_wire(World &w, uint64_t id,
                                       const emit::FIntrepidNetworkGUID &pawn) {
  std::string why;
  gather::Channeler by;
  by.player_id = 7;
  by.character =
      repl::GuidValue{ch::kRuns[1].character[0], ch::kRuns[1].character[1],
                      ch::kRuns[1].character[2], ch::kRuns[1].character[3]};
  by.pawn = pawn;
  gather::NodeBoard board(w.registry(), gather::NodeBoard::Config{});
  repl::ActorShadow shadow;
  std::vector<repl::ContentBlock> blocks;
  repl::Actor view = corpse_view_of(w, id, 7);
  CHECK(repl::collect_open_blocks(view, shadow, blocks) && blocks.size() == 8,
        "the living corpse opens with its eight blocks");
  CHECK(board.begin(id, by, t0, milliseconds(13915), why),
        "the board starts the channel (the corpse is no gatherable)");
  CHECK(board.state_of(id) == gather::NodeState::Channeling &&
            !gather::node_gathered(*w.registry().find(id)),
        "it is Channeling, and a corpse is never 'gathered' (no bGathered)");
  view = corpse_view_of(w, id, 7);
  std::vector<repl::ContentBlock> update;
  CHECK(repl::collect_update_blocks(view, shadow, update) && update.size() == 1,
        "the start is one block: the interactable's");
  if (update.size() == 1) {
    CHECK(payload_is(update[0], ch::kRun1457Start),
          "which is the capture's start, to the bit (418 bits: the status "
          "0x08000001, the pawn, who channels)");
  }
  test_corpse_wire_beats_and_done(w, board, id, shadow);
}

static void test_corpse_wire(World &w) {
  std::printf("the retail capture, replayed: the corpse's start, 204 "
              "heartbeats and the done update, to the bit\n");
  const spawns::OpenTemplate *raven_tpl = w.captured->template_for(kRavenPath);
  CHECK(raven_tpl != nullptr,
        "the captured Raven open is the corpse's template");
  if (!raven_tpl) {
    return;
  }
  spawns::InstanceParams p;
  p.x = 5000;
  p.y = 6000;
  p.z = 7000;
  p.level = 5;
  p.server_id = kServerId;
  std::string why;
  auto made = spawns::instantiate(*raven_tpl, [&] { return w.mint(); }, p, why);
  CHECK(made.has_value(), "a Raven is made");
  if (!made) {
    return;
  }
  made->npc.name = "Raven";
  const uint64_t id = made->id;
  CHECK(w.registry().add(std::move(*made)), "it joins the registry");
  mark_corpse_not_targetable(w, id);
  const auto cap_start = load(ch::kRun1457Start, "UInteractableComponent");
  const repl::ElementState *statuses =
      cap_start ? element(*cap_start, "InputEffectStatuses", 0) : nullptr;
  const auto pawn_it =
      statuses ? statuses->values.find(3) : decltype(statuses->values.end()){};
  CHECK(statuses && pawn_it != statuses->values.end() &&
            pawn_it->second.get_if<emit::FIntrepidNetworkGUID>(),
        "the capture's pawn reference is in the start block");
  if (!statuses || pawn_it == statuses->values.end()) {
    return;
  }
  start_and_beat_corpse_wire(
      w, id, *pawn_it->second.get_if<emit::FIntrepidNetworkGUID>());
}
// [C.O.A ~ C | Y | B]

static void check_corpse_board_after_cancel(World &w, gather::NodeBoard &board,
                                            uint64_t a,
                                            const gather::Channeler &two) {
  const world::WorldActor *wa = w.registry().find(a);
  const repl::Subobject *comp =
      wa ? sub_of(wa->actor, "UInteractableComponent") : nullptr;
  const repl::ElementState *can =
      comp ? element(comp->obj, "CanInputActivate", 0) : nullptr;
  const repl::Value *can_array =
      comp ? comp->obj.state.get(*comp->obj.schema, "CanInputActivate")
           : nullptr;
  CHECK(can_array && can_array->get_if<repl::ArrayValue>() &&
            can_array->get_if<repl::ArrayValue>()->elements.size() == 4 &&
            can && can->values.count(1) && *can->values.at(1).get_if<bool>(),
        "CanInputActivate is four entries, the first true again (never an "
        "empty array: the client crashes on one)");
  const repl::ElementState *s0 =
      comp ? element(comp->obj, "CurrentInputStates", 0) : nullptr;
  const auto cur =
      s0 && s0->values.count(1) ? int_of(&s0->values.at(1)) : std::nullopt;
  CHECK(cur && *cur == 1 && gather::node_status(*wa) == 0 &&
            !gather::node_gathered(*wa),
        "the state is 1 again, the status 0, no bGathered");
  std::string why;
  CHECK(board.begin(a, two, t0 + seconds(20), seconds(14), why),
        "and the corpse can be hunted again");
}

static void check_corpse_board_beats_and_plain(World &w,
                                               gather::NodeBoard &board,
                                               uint64_t a, uint64_t b,
                                               const gather::Channeler &one) {
  const auto status_of_actor = [&](uint64_t id) {
    return gather::node_status(*w.registry().find(id));
  };
  Clock::time_point now = t0 + seconds(20);
  Clock::time_point now2 = t0;
  for (int i = 0; i < 3; ++i) {
    now += milliseconds(67);
    board.beat(a, now);
    now2 += milliseconds(67);
    board.beat(b, now2);
  }
  std::string why;
  CHECK(status_of_actor(a) == (7u << 27 | 1u),
        "three heartbeats: the counter 1 + 2 x 3");
  CHECK(board.complete(a, now + milliseconds(40), seconds(3600), why) &&
            status_of_actor(a) == (8u << 27 | 4u),
        "the done update 40 ms after a beat: one on (8), phase 4");
  CHECK(board.complete(b, now2 + milliseconds(70), seconds(3600), why) &&
            status_of_actor(b) == (9u << 27 | 4u),
        "70 ms after a beat: two on (9), phase 4");
  spawns::InstanceParams p;
  p.server_id = kServerId;
  const content::NpcAssetRow *plain = w.asset("Pop_Wolf");
  spawns::GenericNpcs off(*w.captured, &w.motion, w.config(false));
  const spawns::OpenTemplate *plain_tpl =
      plain ? off.template_for(*plain) : nullptr;
  auto pm = plain_tpl ? spawns::instantiate(
                            *plain_tpl, [&] { return w.mint(); }, p, why)
                      : std::nullopt;
  const uint64_t plain_id = pm && w.registry().add(std::move(*pm)) ? pm->id : 0;
  CHECK(plain_id != 0 && !board.begin(plain_id, one, t0, seconds(14), why),
        "a creature with no interactable cannot be begun");
}

static void test_corpse_board(World &w) {
  std::printf("the board and a corpse: a second player, a cancel, a "
              "done-counter rule\n");
  const spawns::OpenTemplate *raven_tpl = w.captured->template_for(kRavenPath);
  if (!raven_tpl) {
    return;
  }
  const auto make = [&]() -> uint64_t {
    spawns::InstanceParams p;
    p.x = 1;
    p.y = 2;
    p.z = 3;
    p.server_id = kServerId;
    std::string why;
    auto m = spawns::instantiate(*raven_tpl, [&] { return w.mint(); }, p, why);
    return m && w.registry().add(std::move(*m)) ? m->id : 0;
  };
  const uint64_t a = make();
  const uint64_t b = make();
  gather::NodeBoard board(w.registry(), gather::NodeBoard::Config{});
  gather::Channeler one;
  gather::Channeler two;
  one.player_id = 1;
  one.character = {1, 2, 3, 4};
  two.player_id = 2;
  two.character = {5, 6, 7, 8};
  std::string why;
  CHECK(board.begin(a, one, t0, seconds(14), why),
        "player 1 starts a hunt of corpse A");
  CHECK(!board.begin(a, two, t0, seconds(14), why) &&
            why.find("channeling") != std::string::npos,
        "player 2 is refused the same corpse (it is channeling)");
  CHECK(board.begin(b, two, t0, seconds(14), why), "but takes the other one");
  CHECK(gather::node_status(*w.registry().find(a)) == 0x08000001u,
        "the status of a channeling corpse is 0x08000001");
  CHECK(board.cancel(a, "test"), "a cancel");
  check_corpse_board_after_cancel(w, board, a, two);
  check_corpse_board_beats_and_plain(w, board, a, b, one);
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
  test_rolls(w);
  test_corpse_wire(w);
  test_corpse_board(w);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
