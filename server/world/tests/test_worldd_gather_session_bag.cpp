// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_gather_session_common.h"

#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <cmath>

using namespace aoc::worldd;

static void check_cancel_unequip_and_rewear(Rig &rig, Player &p,
                                            uint64_t node) {
  const Snap s0 = snap(p);
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  CHECK(rig.board().state_of(node) == gather::NodeState::Channeling &&
            p.s->harvesting_node() == node,
        "a channel starts again");
  p.send_now(item_used(4, 33, guid_n(133)), p.pawn_ch);
  rig.run(milliseconds(100));
  CHECK(!p.s->inventory().at(Worn, 33) &&
            rig.board().state_of(node) == gather::NodeState::Available &&
            p.s->harvesting_node() == 0,
        "the sickle is taken off: the channel ends");
  rig.run(seconds(13));
  const Snap s1 = snap(p);
  CHECK(s1.completed == s0.completed && s1.xp == s0.xp && s1.bag == s0.bag + 1,
        "no reward (the bag holds only the sickle that was taken off)");
  int sickle_slot = -1;
  for (int slot = 0; slot < items::kBackpackSlots; ++slot) {
    const svc::InventoryItem *it = p.s->inventory().at(Bag, slot);
    if (it && it->instance_guid == guid_n(133))
      sickle_slot = slot;
  }
  CHECK(sickle_slot >= 0, "the sickle is in the bag");
  if (sickle_slot >= 0)
    p.send_now(item_used(3, sickle_slot, guid_n(133)), p.pawn_ch);
  rig.run(milliseconds(100));
  CHECK(p.s->inventory().at(Worn, 33) &&
            p.s->inventory().at(Worn, 33)->instance_guid == guid_n(133),
        "the sickle is worn again");
}

static void check_cancel_interrupt_and_unequip(Rig &rig, Player &p,
                                               uint64_t node) {
  const Snap s0 = snap(p);
  const size_t first_from = p.got.size();
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  CHECK(rig.board().state_of(node) == gather::NodeState::Channeling &&
            p.s->harvesting_node() == node,
        "a channel starts");
  const size_t interrupt_from = p.got.size();
  p.send_now(interrupt_channel(), p.pawn_ch);
  CHECK(rig.board().state_of(node) == gather::NodeState::Available &&
            p.s->harvesting_node() == 0 &&
            p.s->harvests_cancelled() == s0.cancelled + 1,
        "ServerInterruptChannel: the channel ends, the node is Available");
  rig.run(milliseconds(300));
  CHECK(can_input_ok(node_seen(p, node, interrupt_from), true),
        "the cancel told the client CanInputActivate with four entries, the "
        "first true (an empty array crashed the client in test 1: its access "
        "violation 487 ms after the cancel)");
  const auto cd = channeling_seen(p, first_from);
  CHECK(cd.size() == 2 && cd[0].second.flags == 6 && cd[0].second.timed &&
            cd[1].second.flags == 0x106 && !cd[1].second.active &&
            !cd[1].second.timed && cd[0].second.target == cd[1].second.target,
        "the cancel cleared the pawn's ChannelingData (the client's animation "
        "and bar end), once");
  rig.run(seconds(13));
  CHECK(same_rewards(s0, snap(p)) &&
            rig.board().state_of(node) == gather::NodeState::Available,
        "...and nothing was granted when the time would have come");
  check_cancel_unequip_and_rewear(rig, p, node);
}

static void check_cancel_teleport_and_remove(Rig &rig, Player &p,
                                             uint64_t node) {
  Snap s0 = snap(p);
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  CHECK(rig.board().state_of(node) == gather::NodeState::Channeling &&
            p.s->harvesting_node() == node,
        "a channel starts a third time");
  const auto moved = p.s->gm_teleport(p.conn, kCx + 30000, kCy,
                                      bs::kDefaultSpawn.z, rig.t, "the test");
  CHECK(moved.status == PlayerSession::GmMoveResult::Status::Moved,
        "the player is moved 300 m away");
  rig.run(milliseconds(200));
  CHECK(rig.board().state_of(node) == gather::NodeState::Available &&
            p.s->harvesting_node() == 0,
        "moving away from the node ends the channel");
  rig.run(seconds(13));
  CHECK(same_rewards(s0, snap(p)), "...with no reward");
  p.s->gm_teleport(p.conn, kCx, kCy, bs::kDefaultSpawn.z, rig.t,
                   "the test back");
  play(p);
  const uint64_t node2 = rig.add_node(id::kDaffodil, 200, 0, 32);
  rig.run(seconds(2));
  s0 = snap(p);
  p.send_now(interact(rig.component_of(node2)), p.pawn_ch);
  CHECK(rig.board().state_of(node2) == gather::NodeState::Channeling,
        "a channel starts on a new node");
  rig.world->registry().remove(node2);
  rig.run(milliseconds(200));
  CHECK(p.s->harvesting_node() == 0 &&
            rig.board().state_of(node2) == gather::NodeState::Available,
        "the node leaves the world: the channel ends");
  rig.run(seconds(13));
  CHECK(same_rewards(s0, snap(p)), "...with no reward");
}

static void check_cancel_death_disconnect_and_stall(Rig &rig, Player &p) {
  const uint64_t node3 = rig.add_node(id::kDaffodil, 0, 200, 33);
  rig.run(seconds(2));
  const Snap s0 = snap(p);
  p.send_now(interact(rig.component_of(node3)), p.pawn_ch);
  CHECK(rig.board().state_of(node3) == gather::NodeState::Channeling,
        "a channel starts on a third node");
  p.s->take_damage(p.conn, 1.0e9, rig.t);
  rig.run(milliseconds(300));
  CHECK(p.s->dead() && p.s->harvesting_node() == 0 &&
            rig.board().state_of(node3) == gather::NodeState::Available,
        "the player dies: the channel ends, the node is Available");
  rig.run(seconds(13));
  CHECK(p.s->harvests_completed() == s0.completed &&
            p.s->professions().xp(id::kHerbalism) == s0.xp,
        "...with no reward");
  Player &q =
      rig.add_player(record_of("0000000000000000000000000000cc01", "carol"));
  play(q);
  const Snap qs = snap(q);
  q.send_now(interact(rig.component_of(node3)), q.pawn_ch);
  CHECK(rig.board().state_of(node3) == gather::NodeState::Channeling &&
            q.s->harvesting_node() == node3,
        "a second player channels the node");
  q.s->finish("the test ends the session");
  CHECK(rig.board().state_of(node3) == gather::NodeState::Available,
        "the session ends mid-channel: the node is Available at once");
  rig.run(seconds(13));
  CHECK(q.s->harvests_completed() == 0 && same_rewards(qs, snap(q)),
        "...and nothing is granted");
  const uint64_t node4 = rig.add_node(id::kDaffodil, 0, -200, 34);
  gather::Channeler ghost;
  ghost.player_id = 424242;
  ghost.character = repl::GuidValue{1, 2, 3, 4};
  ghost.pawn.ObjectId = 4242;
  std::string why;
  CHECK(
      rig.board().begin(node4, ghost, rig.t, seconds(5), why),
      "a channel of a player whose session is gone (no session completes it)");
  rig.run(seconds(40));
  CHECK(rig.board().state_of(node4) == gather::NodeState::Available,
        "is freed 30 s after its time: no node is stuck");
}

static void test_cancellations() {
  std::printf("a channel that ends with no reward: the node is Available at "
              "once and nothing is granted\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 31);
  Player &p =
      rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
  play(p);
  check_cancel_interrupt_and_unequip(rig, p, node);
  check_cancel_teleport_and_remove(rig, p, node);
  check_cancel_death_disconnect_and_stall(rig, p);
}

static void test_stand_still() {
  std::printf("a channel needs standing still: a step away ends it with no "
              "reward, a fidget does not\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 51);
  Player &p =
      rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
  play(p);
  const auto channeling = [&] {
    return rig.board().state_of(node) == gather::NodeState::Channeling &&
           p.s->harvesting_node() == node;
  };
  const Snap s0 = snap(p);
  const size_t from = p.got.size();
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  CHECK(channeling(), "a channel starts, 200 cm from the node");
  p.s->gm_teleport(p.conn, kCx + 20, kCy, bs::kDefaultSpawn.z, rig.t,
                   "the test: a fidget");
  rig.run(milliseconds(400));
  CHECK(channeling(), "a step of 20 cm does not end the channel");
  p.s->gm_teleport(p.conn, kCx + 70, kCy, bs::kDefaultSpawn.z, rig.t,
                   "the test: a step away");
  rig.run(milliseconds(300));
  CHECK(!channeling() &&
            rig.board().state_of(node) == gather::NodeState::Available &&
            p.s->harvests_cancelled() == s0.cancelled + 1,
        "70 cm from where it began (still 130 cm from the node, well within "
        "its reach) the channel ends and the node is free");
  CHECK(count_in_log("the player moved 70 cm since the channel began") == 1,
        "the log says why: the player moved 70 cm since the channel began");
  const auto cd = channeling_seen(p, from);
  CHECK(
      cd.size() == 2 && cd[1].second.flags == 0x106 && !cd[1].second.timed,
      "and the client's ChannelingData is cleared: its animation and bar end");
  rig.run(seconds(13));
  CHECK(same_rewards(s0, snap(p)),
        "no reward comes when the time would have been up");
  const Snap s1 = snap(p);
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  CHECK(channeling(), "a new channel starts from where the player stands now");
  CHECK(run_until(
            rig, [&] { return p.s->harvests_completed() == s1.completed + 1; },
            seconds(20)),
        "standing still, the channel completes by the clock");
}

static void test_foliage_line() {
  std::printf(
      "foliage trees are Gathering stage 3: one info line per session\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  Player &p =
      rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
  play(p);
  const size_t before = count_in_log("foliage gathering is off in this world");
  p.request(foliage_interact(), p.pc_ch);
  p.request(foliage_interact(), p.pc_ch);
  p.request(foliage_interact(), p.pc_ch);
  CHECK(count_in_log("foliage gathering is off in this world") == before + 1,
        "three interactions with foliage log one line");
  CHECK(
      count_in_log(
          "cyb: a tree or plant of the world's foliage was interacted with") >=
          1,
      "naming the player and saying what it was");
  CHECK(p.s->harvests_started() == 0 && p.s->harvesting_node() == 0,
        "and no harvest starts");
}

struct Dropped2 {
  std::vector<combat::LootStack> stacks;
};

namespace id3 {
inline constexpr int64_t kCoal = 109162169652923ll;
} // namespace id3

struct FullBagDropped {
  double x, y, z;
  repl::GuidValue owner;
  std::string name;
  std::vector<combat::LootStack> stacks;
  Clock::time_point at;
};

static void check_full_bag_pile(const Player &p, const FullBagDropped &d,
                                Clock::time_point frame_at) {
  const auto &cg = p.s->bootstrap().setup().character_guid;
  CHECK(d.stacks.size() == 1 && d.stacks[0].item == id::kDaffodilItem &&
            d.stacks[0].quantity == 3 && d.stacks[0].quality == 1,
        "it holds the harvest: Daffodil x3 q1");
  const repl::GuidValue me{cg[0], cg[1], cg[2], cg[3]};
  CHECK(d.owner == me && d.name == "cyb", "for the harvester");
  const GoodPosition at = p.s->movement().good();
  CHECK(std::hypot(d.x - at.x, d.y - at.y) < 1.0 && std::fabs(d.z - at.z) < 1.0,
        "where the player stands");
  const double after_frame =
      std::chrono::duration<double>(d.at - frame_at).count();
  CHECK(after_frame >= 0.1 && after_frame < 0.2,
        "100 ms after the experience frame (the world steps 20 ms)");
}

static void test_full_bag() {
  std::printf("a full resource bag: no storage event, a callout, a pile 100 ms "
              "after the experience frame\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  std::vector<FullBagDropped> piles;
  rig.world->set_pile_hook(
      [&](double x, double y, double z, const repl::GuidValue &owner,
          const std::string &name,
          const std::vector<combat::LootStack> &stacks) -> uint64_t {
        piles.push_back({x, y, z, owner, name, stacks, rig.t});
        return 7777 + piles.size();
      });
  const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 41);
  svc::Character full = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
  for (int slot = 0; slot < 24; ++slot)
    full.items.push_back(
        make_item(R0, slot, id3::kCoal, 300 + static_cast<uint32_t>(slot), 20));
  Player &p = rig.add_player(std::move(full));
  play(p);
  CHECK(
      p.s->inventory().count(R0) == 24 && p.s->inventory().count(Bag) == 0,
      "Resource0 holds 24 stacks of coal, one in each cell of the 6 x 4 grid");
  const size_t from = p.got.size();
  const Snap s0 = snap(p);
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  const Clock::time_point frame_at = [&] {
    run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
    return rig.t;
  }();
  CHECK(p.s->harvests_completed() == 1, "the harvest completes");
  rig.run(milliseconds(40));
  const auto calls = pc_calls_since(p, from);
  CHECK(count_calls(calls, "ClientUpdateItemDurability") == 1 &&
            count_calls(calls, "ClientReceiveStorageEvent") == 0,
        "the result frame has the tool's wear and no storage event (1461)");
  CHECK(snap(p).bag == s0.bag && p.s->inventory().count(Bag) == 0 &&
            snap(p).durability == s0.durability - 1 && snap(p).xp == s0.xp + 80,
        "the resource bag is as it was (and the Backpack is not used: the "
        "daffodil 1 x 2 has no free rectangle); the tool wore and the "
        "experience came");
  CHECK(p.s->bootstrap().callouts() == 1,
        "a callout tells the player why (\"Unable to loot, not enough "
        "inventory space.\")");
  CHECK(piles.empty() || piles[0].at - frame_at >= milliseconds(60),
        "the pile is not made in the frame of the result");
  rig.run(milliseconds(200));
  CHECK(piles.size() == 1 && p.s->harvest_piles() == 1, "a pile is made");
  if (!piles.empty())
    check_full_bag_pile(p, piles[0], frame_at);
  rig.run(seconds(4));
  CHECK(p.s->bootstrap().callouts() == 0, "the callout is taken off after 3 s");
}

static void test_backpack_full_blocks_nothing() {
  std::printf("a full Backpack does not block a harvest\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  std::vector<Dropped2> piles;
  rig.world->set_pile_hook(
      [&](double, double, double, const repl::GuidValue &, const std::string &,
          const std::vector<combat::LootStack> &stacks) -> uint64_t {
        piles.push_back({stacks});
        return 8888 + piles.size();
      });
  const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 43);
  svc::Character full = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
  for (int slot = 0; slot < items::kBackpackSlots; ++slot)
    full.items.push_back(make_item(Bag, slot, id2::kPotion,
                                   300 + static_cast<uint32_t>(slot), 5));
  Player &p = rig.add_player(std::move(full));
  play(p);
  CHECK(p.s->inventory().count(Bag) == 70 && p.s->inventory().count(R0) == 0,
        "the Backpack holds 70 potions; Resource0 is empty");
  const size_t from = p.got.size();
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
  rig.run(milliseconds(400));
  const auto calls = pc_calls_since(p, from);
  CHECK(count_calls(calls, "ClientReceiveStorageEvent") == 1 &&
            p.s->inventory().count(R0) == 1 &&
            p.s->inventory().count(Bag) == 70,
        "the daffodils are in Resource0; the Backpack still holds its 70");
  CHECK(piles.empty() && p.s->bootstrap().callouts() == 0,
        "no pile, no callout");
}

static void check_relog_storage_rpcs(const Player &q) {
  size_t adds_to_resource = 0, resource_payloads = 0;
  for (const repl::RpcCall &c : pc_calls_since(q, 0)) {
    if (!c.rpc || std::string(c.rpc->name) != "ClientReceiveStorageEvent")
      continue;
    const auto ev = repl::parse_storage_event(c);
    if (ev && ev->targets.size() == 1 &&
        ev->targets[0].object ==
            q.s->bootstrap().setup().guids.resource_storage[0])
      ++adds_to_resource;
  }
  for (const repl::RpcCall &c : pawn_calls_since(q, 0)) {
    if (!c.rpc || std::string(c.rpc->name) != "ClientReceiveStoragePayload")
      continue;
    const auto pl = repl::parse_storage_payload(c);
    if (pl &&
        pl->target == q.s->bootstrap().setup().guids.resource_storage[0] &&
        pl->maximum_items == 24)
      ++resource_payloads;
  }
  CHECK(adds_to_resource == 1 && resource_payloads >= 1,
        "the join sent the Add into Resource0 and its 24-slot payload");
}

static void test_relog_keeps_the_resource_bag() {
  std::printf("a relog keeps the resource bag\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 44);
  Player &p =
      rig.add_player(record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb"));
  play(p);
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
  rig.run(milliseconds(400));
  CHECK(!rig.sink.inventories.empty(),
        "the harvest handed the inventory to the saver");
  if (rig.sink.inventories.empty())
    return;
  svc::Character again = record_of("bbec5c86eb101fe5f38c87f0fa22e7d1", "cyb2");
  again.items = rig.sink.inventories.back().second;
  bool in_resource = false;
  for (const svc::InventoryItem &it : again.items)
    in_resource = in_resource || (it.container == R0 &&
                                  it.item_record_id == id::kDaffodilItem &&
                                  it.slot == 0 && it.quantity == 3);
  CHECK(in_resource, "the saved items hold the daffodils in Resource0 slot 0");
  Player &q = rig.add_player(std::move(again));
  play(q);
  const svc::InventoryItem *it = q.s->inventory().at(R0, 0);
  CHECK(it && it->item_record_id == id::kDaffodilItem && it->quantity == 3 &&
            q.s->inventory().count(Bag) == p.s->inventory().count(Bag),
        "a relog: the daffodils are in Resource0 slot 0, the Backpack has the "
        "same items");
  check_relog_storage_rpcs(q);
}

static size_t
count_herbalist_level_ups(const std::vector<repl::RpcCall> &calls) {
  size_t level_ups = 0;
  for (const repl::RpcCall &c : calls) {
    if (!c.rpc || std::string(c.rpc->name) != "BroadcastPlayLevelUpEffects")
      continue;
    const schema::RepCommand *lv = repl::rpc_command(*c.rpc, "InLevel");
    const schema::RepCommand *cat = repl::rpc_command(*c.rpc, "Category");
    const auto l =
        lv ? c.params.values.find(lv->handle) : c.params.values.end();
    const auto k =
        cat ? c.params.values.find(cat->handle) : c.params.values.end();
    if (l != c.params.values.end() && l->second.get_if<int64_t>() &&
        *l->second.get_if<int64_t>() == 2 && k != c.params.values.end() &&
        k->second.get_if<int64_t>() &&
        *k->second.get_if<int64_t>() == id::kLtHerbalist) {
      ++level_ups;
    }
  }
  return level_ups;
}

static void test_levels() {
  std::printf(
      "a profession's level-up in a harvest, and the tiers' experience\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  const uint64_t node = rig.add_node(id::kDaffodil, 200, 0, 51);
  svc::Character rec = record_of("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
  rec.professions.push_back(prof(id::kHerbalism, 0, 1, 1560));
  Player &p = rig.add_player(std::move(rec));
  play(p);
  CHECK(p.s->professions().level(id::kHerbalism) == 1 &&
            p.s->professions().xp(id::kHerbalism) == 1560,
        "a Novice Herbalist level 1 with 1,560 XP");
  const size_t pawn_from = p.got.size();
  p.send_now(interact(rig.component_of(node)), p.pawn_ch);
  run_until(rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20));
  rig.run(milliseconds(300));
  CHECK(p.s->professions().xp(id::kHerbalism) == 1640 &&
            p.s->professions().level(id::kHerbalism) == 2,
        "+80 XP: 1,640, level 2");
  CHECK(count_herbalist_level_ups(pawn_calls_since(p, pawn_from)) == 1,
        "BroadcastPlayLevelUpEffects(Herbalist, 2) was sent once");
  bool saved = false;
  for (const auto &pr : rig.sink.progress) {
    for (const svc::ProfessionState &s : pr.progress.professions)
      saved = saved || (s.profession_guid == id::kHerbalism && s.xp == 1640 &&
                        s.level == 2);
  }
  CHECK(saved, "and the level and XP were handed to the saver");
  const uint64_t node2 = rig.add_node(id::kDaffodil, 0, 200, 52);
  svc::Character cap = record_of("0000000000000000000000000000dd01", "capped");
  cap.professions.push_back(prof(id::kHerbalism, 0, 10, 40000 + 5000));
  Player &q = rig.add_player(std::move(cap));
  play(q);
  const int64_t held = q.s->professions().xp(id::kHerbalism);
  q.send_now(interact(rig.component_of(node2)), q.pawn_ch);
  run_until(rig, [&] { return q.s->harvests_completed() == 1; }, seconds(20));
  CHECK(q.s->professions().level(id::kHerbalism) == 10 &&
            q.s->professions().xp(id::kHerbalism) >= held &&
            q.s->professions().xp(id::kHerbalism) <= 49999,
        "a Novice at level 10 stays at level 10; the XP is held below the "
        "Apprentice's first level");
}

int main() {
  auto logger = std::make_shared<spdlog::logger>(
      "test", std::make_shared<spdlog::sinks::ostream_sink_mt>(g_log));
  logger->set_level(spdlog::level::info);
  spdlog::set_default_logger(logger);
  const char *path = db_path();
  if (!path) {
    std::printf("[skip] test_worldd_gather_session_bag: no content.db (set "
                "AOC_CONTENT_DB, or build dist/content/content.db)\n");
    return 0;
  }
  g_path = path;
  std::printf("content.db: %s\n", path);
  test_cancellations();
  test_stand_still();
  test_foliage_line();
  test_full_bag();
  test_backpack_full_blocks_nothing();
  test_relog_keeps_the_resource_bag();
  test_levels();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
