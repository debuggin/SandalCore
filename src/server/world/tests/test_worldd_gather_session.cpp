// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_gather_session_common.h"

#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cmath>

using namespace aoc::worldd;

struct NodeSeen {
  std::vector<uint32_t> statuses;
  bool gathered = false;
  size_t chains = 0;
  std::vector<std::vector<std::optional<bool>>> can_input;
};

static void read_can_input(const repl::Value *can, NodeSeen &out) {
  const auto *can_arr = can ? can->get_if<repl::ArrayValue>() : nullptr;
  if (!can_arr)
    return;
  std::vector<std::optional<bool>> entries;
  for (const repl::ElementState &e : can_arr->elements) {
    const auto w = e.values.find(1);
    const bool *x = w == e.values.end() ? nullptr : w->second.get_if<bool>();
    entries.push_back(x ? std::optional<bool>(*x) : std::nullopt);
  }
  out.can_input.push_back(std::move(entries));
}

static void read_node_block(const fake::Delivered &d, const repl::BlockView &b,
                            uint64_t comp_id, NodeSeen &out) {
  if (!b.header.has_rep_layout)
    return;
  size_t pos = b.payload_pos;
  const size_t end = b.payload_pos + b.payload_bits;
  if (b.header.is_actor) {
    const schema::ClassSchema &actor_class = class_named("AGatherableActor");
    const auto st =
        repl::read_rep_block(actor_class, d.content.data(), end, pos);
    const repl::Value *v = st ? st->get(actor_class, "bGathered") : nullptr;
    if (v && v->get_if<bool>() && *v->get_if<bool>())
      out.gathered = true;
    return;
  }
  if (b.header.object.ObjectId != comp_id)
    return;
  const schema::ClassSchema &inter = class_named("UInteractableComponent");
  const auto st = repl::read_rep_block(inter, d.content.data(), end, pos);
  const repl::Value *v = st ? st->get(inter, "InputEffectStatuses") : nullptr;
  const auto *arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
  if (arr && !arr->elements.empty()) {
    const auto it = arr->elements[0].values.find(1);
    if (it != arr->elements[0].values.end() && it->second.get_if<uint64_t>())
      out.statuses.push_back(
          static_cast<uint32_t>(*it->second.get_if<uint64_t>()));
  }
  read_can_input(st ? st->get(inter, "CanInputActivate") : nullptr, out);
}

static NodeSeen node_seen(const Player &p, uint64_t node, size_t from) {
  NodeSeen out;
  const uint32_t ch =
      p.s->actor_view() ? p.s->actor_view()->channel_of(node) : 0;
  const world::WorldActor *a = p.rig.world->registry().find(node);
  if (ch == 0 || !a)
    return out;
  for (size_t i = from; i < p.got.size(); ++i) {
    if (p.got[i].channel != ch || p.got[i].open || p.got[i].close)
      continue;
    const auto blocks = repl::read_content_blocks(
        p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
    if (!blocks)
      continue;
    ++out.chains;
    for (const auto &b : *blocks)
      read_node_block(p.got[i], b, a->npc.interact_component, out);
  }
  return out;
}

static bool can_input_ok(const NodeSeen &n, bool last_first) {
  for (const auto &c : n.can_input) {
    if (c.size() != 4)
      return false;
  }
  return !n.can_input.empty() && n.can_input.back()[0] &&
         *n.can_input.back()[0] == last_first;
}

static std::string refusals_of(const Player &p) {
  std::string out;
  for (const auto &[kind, n] : p.s->harvest_refusals())
    out += (out.empty() ? "" : ", ") + kind + " x" + std::to_string(n);
  return out.empty() ? "none" : out;
}

static size_t refusal_count(const Player &p, const char *kind) {
  const auto it = p.s->harvest_refusals().find(kind);
  return it == p.s->harvest_refusals().end() ? 0 : it->second;
}

static std::vector<std::string>
call_names(const std::vector<repl::RpcCall> &calls) {
  std::vector<std::string> out;
  for (const auto &c : calls)
    out.push_back(c.rpc ? c.rpc->name : "?");
  return out;
}

static std::vector<std::pair<size_t, std::string>>
pc_chain_calls(const Player &p, size_t from) {
  std::vector<std::pair<size_t, std::string>> out;
  const schema::ClassSchema &cls = class_named("AoCPlayerControllerBP_C");
  for (size_t i = from; i < p.got.size(); ++i) {
    if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close)
      continue;
    const auto blocks = repl::read_content_blocks(
        p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
    if (!blocks)
      continue;
    std::vector<repl::RpcCall> calls;
    for (const auto &b : *blocks)
      append_block_rpcs(cls, p.got[i], b, calls);
    for (const auto &c : calls)
      out.emplace_back(i, c.rpc ? c.rpc->name : "?");
  }
  return out;
}

static bool has_object_block(const std::vector<repl::BlockView> &blocks,
                             const emit::FIntrepidNetworkGUID &obj_guid) {
  for (const auto &b : blocks) {
    if (!b.header.is_actor && b.header.object == obj_guid)
      return true;
  }
  return false;
}

static size_t xp_chain_since(const Player &p, size_t from) {
  const bs::SessionGuids &g = p.s->bootstrap().setup().guids;
  for (size_t i = from; i < p.got.size(); ++i) {
    if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close)
      continue;
    const auto blocks = repl::read_content_blocks(
        p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
    if (blocks && has_object_block(*blocks, g.xp_tracker))
      return i;
  }
  return static_cast<size_t>(-1);
}

struct Tracker {
  repl::LevelingState state;
  size_t updates = 0;
  void absorb_block(const fake::Delivered &d, const repl::BlockView &b,
                    const emit::FIntrepidNetworkGUID &xp_tracker) {
    if (b.header.is_actor || !(b.header.object == xp_tracker))
      return;
    size_t pos = b.payload_pos;
    const size_t end = b.payload_pos + b.payload_bits;
    const schema::ClassSchema &cls = state.schema();
    if (b.header.has_rep_layout &&
        !repl::read_rep_block(cls, d.content.data(), end, pos))
      return;
    if (pos >= end)
      return;
    const auto fields =
        repl::read_fast_array_fields(cls, d.content.data(), end, pos);
    if (!fields)
      return;
    for (const repl::FastArrayField &f : *fields)
      state.apply(f);
    ++updates;
  }
  void absorb(const Player &p, size_t from) {
    const bs::SessionGuids &g = p.s->bootstrap().setup().guids;
    for (size_t i = from; i < p.got.size(); ++i) {
      if (p.got[i].channel != p.pc_ch || p.got[i].open || p.got[i].close)
        continue;
      const auto blocks = repl::read_content_blocks(
          p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
      if (!blocks)
        continue;
      for (const auto &b : *blocks)
        absorb_block(p.got[i], b, g.xp_tracker);
    }
  }
};

static repl::DesignDataId leveling_id(int64_t guid) {
  return {guid, content::kTypeLevelingTypeDef};
}

static void check_harvest_start(Rig &rig, Player &p, uint64_t node,
                                size_t from) {
  CHECK(rig.board().state_of(node) == gather::NodeState::Channeling &&
            p.s->harvesting_node() == node && p.s->harvests_started() == 1,
        "the interact starts the channel: the node is Channeling for this "
        "player");
  const NodeSeen first = node_seen(p, node, from);
  CHECK(!first.statuses.empty() && first.statuses[0] == 0x08000001u,
        "the client was sent the start at once: the status 0x08000001");
  const auto cd = channeling_seen(p, from);
  const world::WorldActor *n = rig.world->registry().find(node);
  const gather::Channel *ch = rig.board().channel_of(node);
  const double length =
      ch ? std::chrono::duration<double>(ch->completes - ch->started).count()
         : 0.0;
  const auto sickle_guid = items::guid_of(guid_n(133));
  CHECK(cd.size() == 1,
        "the start told the client the pawn's ChannelingData, once (the gather "
        "animation, the bar and the client's own cancel come from it)");
  if (cd.size() == 1 && n && sickle_guid) {
    const repl::ChannelingData &d = cd[0].second;
    CHECK(d.target == n->actor.guid && d.flags == 6 && d.active && d.timed,
          "it names the node and has Flags 6, active (1427: the node 2259212, "
          "6, 1)");
    CHECK(
        length > 10.0 &&
            std::fabs(static_cast<double>(d.duration) - length) < 0.01,
        "its duration is the channel's length (11.25 s for a Novice Daffodil)");
    CHECK(d.tool_record == 6064629375589482502ll && d.tool_guid == *sickle_guid,
          "it names the sickle in the hand: its item record and its instance "
          "guid (the one of the durability update)");
    CHECK(
        d.start_time == std::floor(d.start_time) &&
            std::fabs(d.start_time - bs::effect_clock(world_time_at(rig.t))) <
                2.0,
        "its start is a whole number of seconds on the synchronized clock of "
        "the effects (retail's 13788648.0 beside CurrentEffects' 13788648.65)");
  }
}

static void check_harvest_durability_rpc(Player &p, const repl::RpcCall &c) {
  const auto u = repl::parse_item_durability(c);
  CHECK(u && u->items.size() == 1 && u->items[0].slot == 33 &&
            u->items[0].durability == 149.0f &&
            u->container == p.s->bootstrap().setup().guids.equipment &&
            items::guid_hex(u->items[0].item) == guid_n(133),
        "the update names the Equipment, the sickle's instance in slot 33, and "
        "149");
}

static void check_harvest_storage_callout_rpc(
    const std::optional<repl::ItemStorageEvent> &ev) {
  CHECK(ev && ev->callout &&
            ev->callout->message ==
                "Acquired <a id=\"Item\" style=\"Item_Common\" "
                "metaType=\"ItemLink\" metaGUID=\"6064629184431849472\" "
                "metaItemQuality=\"1\" metaEnchantLevel=\"0\" "
                "metaTemperingPercent=\"0\" metaDurability=\"0\" "
                "metaSockets=\"\"> [Daffodil] </>x3" &&
            ev->callout->formatted_on_screen == ev->callout->message &&
            ev->callout->item == ev->item && ev->callout->callout_type == 6 &&
            ev->callout->point_size == 36 &&
            ev->callout->chat_category == 141 &&
            ev->callout->creation_time.ticks > 600000000000000000ll,
        "and carries the pick-up popup's callout: 1427's text (Acquired <a "
        "...> [Daffodil] </>x3), the stored item for the icon, type 6, size "
        "36, category 141, and the time");
}

static void check_harvest_storage_event_rpc(Player &p, const repl::RpcCall &c) {
  const auto ev = repl::parse_storage_event(c);
  CHECK(ev && ev->type == static_cast<uint8_t>(repl::StorageEventType::Add) &&
            ev->item.record_id == static_cast<uint64_t>(id::kDaffodilItem) &&
            ev->item.quantity == 3 && ev->item.quality == 1 &&
            ev->targets.size() == 1 &&
            ev->targets[0].object ==
                p.s->bootstrap().setup().guids.resource_storage[0] &&
            ev->targets[0].slots.size() == 1 &&
            ev->targets[0].slots[0].slot == 0 &&
            ev->targets[0].slots[0].count == 3 &&
            ev->targets[0].status == repl::kStorageNoError,
        "the storage event adds Resource_Raw_Plant_Daffodil x3 q1 to the "
        "Resource container at slot 0 (1427's: Equipment + 8, slot 0, status "
        "14)");
  check_harvest_storage_callout_rpc(ev);
}

static void check_harvest_channel_clear_order(Player &p, size_t from) {
  const auto cd = channeling_seen(p, from);
  CHECK(cd.size() == 2 && cd[1].second.flags == 0x106 && !cd[1].second.active &&
            !cd[1].second.timed && cd[0].second.target == cd[1].second.target,
        "the result cleared the pawn's ChannelingData, once, naming the same "
        "node (Flags 0x106, nothing else)");
  size_t at_dur = static_cast<size_t>(-1), at_ev = static_cast<size_t>(-1);
  for (const auto &[chain, name] : pc_chain_calls(p, from)) {
    if (name == "ClientUpdateItemDurability")
      at_dur = chain;
    if (name == "ClientReceiveStorageEvent")
      at_ev = chain;
  }
  const size_t at_xp =
      xp_chain_since(p, at_dur == static_cast<size_t>(-1) ? from : at_dur);
  CHECK(cd.size() == 2 && at_ev < cd[1].first && cd[1].first < at_xp,
        "the clear comes after the storage event and before the XPTracker's "
        "chain (retail's pawn block precedes the PlayerController's in that "
        "packet)");
}

static void
find_durability_and_event_indices(const std::vector<std::string> &names,
                                  size_t &at_durability, size_t &at_event) {
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == "ClientUpdateItemDurability")
      at_durability = i;
    if (names[i] == "ClientReceiveStorageEvent")
      at_event = i;
  }
}

static void check_harvest_result_frame(Player &p, size_t from) {
  const auto calls = pc_calls_since(p, from);
  const auto names = call_names(calls);
  CHECK(count_calls(calls, "ClientUpdateItemDurability") == 1 &&
            count_calls(calls, "ClientReceiveStorageEvent") == 1,
        "the result frame: one durability update and one storage event");
  size_t at_durability = 99, at_event = 99;
  find_durability_and_event_indices(names, at_durability, at_event);
  CHECK(at_durability < at_event,
        "the durability update comes before the item (as 1427's packet 673)");
  for (const auto &c : calls) {
    if (!c.rpc)
      continue;
    if (std::string(c.rpc->name) == "ClientUpdateItemDurability")
      check_harvest_durability_rpc(p, c);
    if (std::string(c.rpc->name) == "ClientReceiveStorageEvent")
      check_harvest_storage_event_rpc(p, c);
  }
  check_harvest_channel_clear_order(p, from);
}

static void check_harvest_saved_and_respawn(Rig &rig, Player &p,
                                            uint64_t node) {
  bool saved_xp = false, saved_bag = false;
  for (const auto &pr : rig.sink.progress) {
    for (const svc::ProfessionState &ps : pr.progress.professions)
      saved_xp = saved_xp || (ps.xp == 80);
  }
  for (const auto &inv : rig.sink.inventories) {
    for (const svc::InventoryItem &it : inv.second)
      saved_bag = saved_bag || (it.item_record_id == id::kDaffodilItem &&
                                it.container == R0);
  }
  CHECK(saved_xp && saved_bag,
        "the progress (the Herbalist's 80 XP) and the inventory (the Daffodil "
        "in Resource0) were handed to the saver");
  CHECK(rig.board().state_of(node) == gather::NodeState::Gathered,
        "the node is Gathered");
  const size_t chains_before = p.got.size();
  CHECK(
      run_until(
          rig,
          [&] {
            return rig.board().state_of(node) == gather::NodeState::Available;
          },
          seconds(20)),
      "it is Available again after the respawn (the data's 14,400 s x 0.001)");
  rig.run(milliseconds(300));
  const NodeSeen back = node_seen(p, node, chains_before);
  CHECK(!back.statuses.empty() && back.statuses.back() == 0,
        "the client was sent the reset: the status 0");
  CHECK(can_input_ok(back, true),
        "the respawn told the client CanInputActivate with four entries, the "
        "first true (an empty array crashed the client in test 1)");
}

static void check_harvest_xp_and_respawn(Rig &rig, Player &p, uint64_t node,
                                         size_t from, size_t bag_before) {
  CHECK(p.s->inventory().count(Bag) == bag_before &&
            p.s->inventory().count(R0) == 1 && p.s->inventory().at(R0, 0) &&
            p.s->inventory().at(R0, 0)->item_record_id == id::kDaffodilItem &&
            p.s->inventory().at(R0, 0)->quantity == 3,
        "the Backpack is untouched; Resource0 holds the daffodils, one stack "
        "of 3 at slot 0");
  const svc::InventoryItem *tool = p.s->inventory().at(Worn, 33);
  CHECK(tool && tool->durability == 149.0f, "the sickle has 149");
  Tracker tracker;
  tracker.absorb(p, from);
  const auto herbalist =
      tracker.state.experience(leveling_id(id::kLtHerbalist));
  CHECK(herbalist && *herbalist == 80,
        "the XPTracker: the Herbalist has 80 XP (1427's +80)");
  CHECK(p.s->professions().xp(id::kHerbalism) == 80 &&
            p.s->professions().level(id::kHerbalism) == 1,
        "the session's professions: 80 XP, still level 1 (the curve's level 2 "
        "is at 1,600)");
  CHECK(p.s->progress().xp > 0 || p.s->progress().debt >= 0,
        "and Adventuring's experience was offered (the debt first)");
  CHECK(p.s->harvests_completed() == 1 && p.s->harvesting_node() == 0,
        "one harvest completed; the session harvests nothing now");
  check_harvest_saved_and_respawn(rig, p, node);
}

static void check_second_harvest(Rig &rig, Player &p, uint64_t node,
                                 size_t from) {
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  CHECK(rig.board().state_of(node) == gather::NodeState::Channeling &&
            p.s->harvests_started() == 2,
        "the node can be harvested again");
  run_until(rig, [&] { return p.s->harvests_completed() == 2; }, seconds(20));
  Tracker second;
  second.absorb(p, from);
  const auto h2 = second.state.experience(leveling_id(id::kLtHerbalist));
  CHECK(p.s->harvests_completed() == 2 && h2 && *h2 == 160 &&
            p.s->inventory().at(Worn, 33)->durability == 148.0f,
        "the second harvest: 160 XP in all, the sickle at 148");
  CHECK(p.s->inventory().count(R0) == 1 && p.s->inventory().at(R0, 0) &&
            p.s->inventory().at(R0, 0)->quantity == 6,
        "...and the second daffodils (x3 q1) topped the stack up to 6: one "
        "stack in Resource0");
}

static void test_a_harvest(Rig &rig) {
  std::printf("a harvest from the interact to the node's return\n");
  const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 1);
  Player &p =
      rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
  play(p);
  CHECK(p.s->actor_view()->state_of(node) == world::ClientState::Awake,
        "the node is open on the client's connection");
  CHECK(p.s->professions().valid() &&
            p.s->professions().tier(id::kHerbalism) == 0 &&
            p.s->professions().level(id::kHerbalism) == 1,
        "the character is a Novice Herbalist, level 1");
  const svc::InventoryItem *sickle = p.s->inventory().at(Worn, 33);
  CHECK(sickle && sickle->durability == 150.0f,
        "a T1 sickle with 150 durability is worn in slot 33");
  const size_t bag_before = p.s->inventory().count(Bag);
  CHECK(p.s->inventory().grid(R0).cells() == 24,
        "the Sanctus bag in slot 23 opens Resource0, 6 x 4 = 24 cells");
  const size_t from = p.got.size();
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  check_harvest_start(rig, p, node, from);
  const bool done = run_until(
      rig,
      [&] { return rig.board().state_of(node) == gather::NodeState::Gathered; },
      seconds(20));
  CHECK(done, "the channel completes by the clock");
  rig.run(milliseconds(400));
  const NodeSeen seen = node_seen(p, node, from);
  size_t steps3 = 0;
  for (size_t i = 1; i < seen.statuses.size(); ++i)
    steps3 += (((seen.statuses[i] >> 27) + 32 - (seen.statuses[i - 1] >> 27)) &
               31u) == 3
                  ? 1
                  : 0;
  char msg[200];
  std::snprintf(
      msg, sizeof msg,
      "the client saw %zu status updates (the start, about 112 heartbeats, the "
      "done update); the counter went up by 3 each time (%zu of %zu)",
      seen.statuses.size(), steps3,
      seen.statuses.empty() ? 0 : seen.statuses.size() - 1);
  CHECK(seen.statuses.size() > 105 && seen.statuses.size() < 120 &&
            steps3 == seen.statuses.size() - 1,
        msg);
  CHECK((seen.statuses.back() & 0x07ffffffu) == 4,
        "and the last is the done update: phase 4");
  CHECK(seen.gathered, "bGathered reached the client");
  check_harvest_result_frame(p, from);
  check_harvest_xp_and_respawn(rig, p, node, from, bag_before);
  check_second_harvest(rig, p, node, from);
}

static void check_journeyman_harvest(Rig &rig, uint64_t moonbell) {
  svc::Character jr =
      record_of("0000000000000000000000000000aa03", "journeyman", false);
  jr.professions.push_back(prof(id::kHerbalism, 2, 20, 160000));
  jr.items.push_back(make_tool(Worn, 33, id2::kT3Sickle, 133, 350.0f));
  Player &journeyman = rig.add_player(std::move(jr));
  play(journeyman);
  journeyman.send_now(interact(rig.component_of(moonbell)), journeyman.pawn_ch);
  CHECK(journeyman.s->harvests_started() == 1 &&
            rig.board().state_of(moonbell) == gather::NodeState::Channeling,
        "a Journeyman Herbalist with the T3 sickle starts at the Apprentice "
        "node");
  const int64_t xp_before = journeyman.s->professions().xp(id::kHerbalism);
  run_until(
      rig, [&] { return journeyman.s->harvests_completed() == 1; },
      seconds(20));
  const int64_t gain =
      journeyman.s->professions().xp(id::kHerbalism) - xp_before;
  CHECK(journeyman.s->harvests_completed() == 1 && gain >= 90 && gain <= 270,
        "and gets the Apprentice tier's 90 profession XP at least (the T3 "
        "sickle's experience rating raises it: its stats are in the rating, "
        "equipment included)");
  CHECK(journeyman.s->inventory().at(Worn, 33)->durability == 349.0f,
        "the T3 sickle: 349");
}

static void test_refusals() {
  std::printf("a start that is refused: each check logged and counted, nothing "
              "sent, nothing changed\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  const uint64_t daffodil = rig.add_node(id::kDaffodil, 200, 0, 11);
  const uint64_t moonbell = rig.add_node(id::kMoonbell, 0, 200, 12);
  const uint64_t far_node = rig.add_node(id::kDaffodil, 1000, 0, 13);
  const uint64_t dormant = rig.add_node(id::kDaffodil, 25000, 0, 14);
  Player &bare = rig.add_player(
      record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "bare", /*tools=*/false));
  Player &novice =
      rig.add_player(record_of("0000000000000000000000000000aa01", "novice"));
  svc::Character ar =
      record_of("0000000000000000000000000000aa02", "apprentice");
  ar.professions.push_back(prof(id::kHerbalism, 1, 10, 40000));
  Player &apprentice = rig.add_player(std::move(ar));
  for (Player *p : {&bare, &novice, &apprentice})
    play(*p);
  CHECK(novice.s->actor_view()->state_of(far_node) ==
                world::ClientState::Awake &&
            novice.s->actor_view()->state_of(dormant) !=
                world::ClientState::Awake,
        "the 10 m node is open on the client, the 250 m node is only dormant "
        "there");
  const size_t pc_before = pc_calls_since(novice, 0).size();
  bare.send_now(interact(rig.component_of(daffodil)), bare.pawn_ch);
  CHECK(refusal_count(bare, "no tool") == 1 &&
            bare.s->harvests_started() == 0 &&
            rig.board().state_of(daffodil) == gather::NodeState::Available,
        "nothing worn in the Sickle's slot: refused (no tool)");
  novice.send_now(interact(rig.component_of(moonbell)), novice.pawn_ch);
  CHECK(refusal_count(novice, "not certified") == 1 &&
            rig.board().state_of(moonbell) == gather::NodeState::Available,
        "a Novice Herbalist at an Apprentice node: not certified");
  novice.send_now(interact(rig.component_of(far_node)), novice.pawn_ch);
  CHECK(refusal_count(novice, "out of range") == 1 &&
            rig.board().state_of(far_node) == gather::NodeState::Available,
        "a node 10 m away: out of range");
  novice.send_now(interact(rig.component_of(dormant)), novice.pawn_ch);
  CHECK(refusal_count(novice, "not open") == 1,
        "a node the client has only dormant: not open");
  apprentice.send_now(interact(rig.component_of(moonbell)), apprentice.pawn_ch);
  CHECK(apprentice.s->professions().tier(id::kHerbalism) == 1 &&
            refusal_count(apprentice, "tool tier too low") == 1,
        "an Apprentice Herbalist with a T1 sickle at an Apprentice node: the "
        "tool's tier is too low");
  novice.send_now(interact(999999999), novice.pawn_ch);
  CHECK(novice.s->harvests_started() == 0 &&
            novice.s->interactions_ignored() >= 1,
        "a component no actor owns is ignored");
  CHECK(pc_calls_since(novice, 0).size() == pc_before,
        "and none of this sent the PlayerController a call");
  std::string all = "bare: " + refusals_of(bare) +
                    "; novice: " + refusals_of(novice) +
                    "; apprentice: " + refusals_of(apprentice);
  std::printf("      (%s)\n", all.c_str());
  check_journeyman_harvest(rig, moonbell);
}

static void check_second_player_exploits(Rig &rig, Player &p, uint64_t a,
                                         uint64_t b) {
  Player &q =
      rig.add_player(record_of("0000000000000000000000000000bb01", "bob"));
  play(q);
  p.send_now(interact(rig.component_of(b)), p.pawn_ch);
  CHECK(rig.board().state_of(b) == gather::NodeState::Channeling &&
            rig.board().channel_of(b)->player_id ==
                static_cast<uint64_t>(p.s->bootstrap().setup().player_id),
        "the first player channels the other node");
  q.send_now(interact(rig.component_of(b)), q.pawn_ch);
  CHECK(refusal_count(q, "node busy") == 1 && q.s->harvests_started() == 0,
        "the second player is refused: the state is shared");
  const Snap q0 = snap(q);
  run_until(rig, [&] { return p.s->harvests_completed() == 2; }, seconds(20));
  rig.run(seconds(1));
  const Snap q1 = snap(q);
  CHECK(p.s->harvests_completed() == 2 && same_rewards(q0, q1),
        "the first player gets the reward and the second nothing");
  q.send_now(interact(rig.component_of(b)), q.pawn_ch);
  CHECK(refusal_count(q, "node gathered") == 1,
        "and the node is gathered for the second player too");
  CHECK(run_until(
            rig,
            [&] {
              return rig.board().state_of(a) == gather::NodeState::Available &&
                     rig.board().state_of(b) == gather::NodeState::Available;
            },
            seconds(30)),
        "both nodes are back after the respawn");
  q.send_now(interact(rig.component_of(b)), q.pawn_ch);
  CHECK(q.s->harvests_started() == 1, "and the second player harvests it now");
}

static void test_exploits() {
  std::printf("exploits: a repeated or flooded interact, a gathered node, "
              "another player's node; everything is granted once\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  const uint64_t a = rig.add_node(id::kDaffodil, 200, 0, 21);
  const uint64_t b = rig.add_node(id::kDaffodil, 0, 200, 22);
  Player &p =
      rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
  play(p);
  const Snap s0 = snap(p);
  p.send_now(interact(rig.component_of(a)), p.pawn_ch);
  CHECK(p.s->harvests_started() == 1 &&
            rig.board().state_of(a) == gather::NodeState::Channeling,
        "the first interact starts the channel");
  const gather::Channel before = *rig.board().channel_of(a);
  for (int i = 0; i < 3; ++i)
    p.send_now(interact(rig.component_of(a)), p.pawn_ch);
  p.send_now(interact(rig.component_of(b)), p.pawn_ch);
  CHECK(refusal_count(p, "already channeling") == 4 &&
            p.s->harvests_started() == 1,
        "the interact again (x3) and another node's: refused, the channel is "
        "one");
  CHECK(rig.board().state_of(b) == gather::NodeState::Available &&
            rig.board().channel_of(a)->started == before.started &&
            rig.board().channel_of(a)->completes == before.completes,
        "the channel was not restarted or moved");
  std::vector<fake::Bunch> flood;
  for (int i = 0; i < 60; ++i)
    flood.push_back(
        p.client.actor(p.pawn_ch, interact(rig.component_of(a)), true));
  const size_t dropped_before = p.s->requests_dropped();
  p.client_sends(std::move(flood));
  rig.run(milliseconds(60));
  CHECK(p.s->requests_dropped() > dropped_before &&
            p.s->harvests_started() == 1 &&
            rig.board().state_of(a) == gather::NodeState::Channeling,
        "a flood of 60 interacts is cut at the budget and changes nothing");
  const bool ended = run_until(
      rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
  CHECK(ended, "the channel ends by the server's clock");
  rig.run(seconds(1));
  const Snap s1 = snap(p);
  CHECK(s1.completed == 1 && s1.bag == s0.bag + 1 &&
            s1.durability == s0.durability - 1 && s1.xp == s0.xp + 80,
        "granted once: one stack, one durability, 80 XP");
  p.send_now(interact(rig.component_of(a)), p.pawn_ch);
  p.send_now(interact(rig.component_of(a)), p.pawn_ch);
  CHECK(refusal_count(p, "node gathered") == 2 && p.s->harvests_started() == 1,
        "an interact on the gathered node (and its replay): refused");
  rig.run(seconds(2));
  const Snap s2 = snap(p);
  CHECK(same_rewards(s1, s2), "and nothing more was granted");
  check_second_player_exploits(rig, p, a, b);
}

int main() {
  auto logger = std::make_shared<spdlog::logger>(
      "test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
  logger->set_level(spdlog::level::info);
  spdlog::set_default_logger(logger);
  const char *path = db_path();
  if (!path) {
    std::printf("[skip] test_worldd_gather_session: no content.db (set "
                "AOC_CONTENT_DB, or build dist/content/content.db)\n");
    return 0;
  }
  g_path = path;
  std::printf("content.db: %s\n", path);
  {
    Rig rig;
    if (!rig.open(path))
      return 1;
    test_a_harvest(rig);
  }
  test_refusals();
  test_exploits();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
