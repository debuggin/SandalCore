// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_hunting_session_common.h"

using namespace aoc::worldd;
using std::chrono::milliseconds;
using std::chrono::seconds;

void Player::join() {
  s = aoc::worldd::PlayerSession::join(conn, *rig.world, record, identity(),
                                       world_time_at(rig.t), error);
  if (!s) {
    std::printf("  [FAIL] the session does not join: %s\n", error.c_str());
    std::exit(2);
  }
  pawn_ch = s->channel_of(bs::Role::Pawn);
  pc_ch = s->channel_of(bs::Role::PlayerController);
}

void Player::flush() {
  for (auto &pkt : conn.flush(rig.t)) {
    const auto r = client.receive(pkt);
    for (const auto &d : r.delivered)
      got.push_back(d);
  }
}

void Player::client_sends(std::vector<fake::Bunch> bunches) {
  const auto pkt = client.packet(std::move(bunches));
  const std::vector<aoc::worldd::InBunch> in = conn.receive(pkt, rig.t);
  s->take_acks(conn);
  for (const aoc::worldd::InBunch &b : in)
    s->on_bunch(conn, b, rig.t);
  s->end_of_datagram(conn);
}

void Player::step_session() {
  s->tick(conn, rig.t);
  flush();
  s->take_acks(conn);
  client_sends({});
}

void Player::request(const emit::BunchWriter &rpc, uint32_t channel) {
  client_sends({client.actor(channel, rpc, true)});
  rig.settle();
}

void Player::send_now(const emit::BunchWriter &rpc, uint32_t channel) {
  client_sends({client.actor(channel, rpc, true)});
  rig.run(std::chrono::milliseconds(60));
}

emit::BunchWriter actor_rpc(const schema::ClassSchema &cls, const char *name,
                            const repl::RpcParams &params) {
  emit::BunchWriter out;
  const schema::RpcSchema *rpc = cls.rpc(name);
  if (!rpc)
    return out;
  emit::BunchWriter field;
  bool failed = false;
  if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 &&
      !params.values.empty()) {
    std::printf("  [FAIL] %s does not write\n", name);
    return out;
  }
  if (failed)
    return out;
  repl::BlockHeader header;
  repl::write_content_block(out, header, field, /*server_form=*/false);
  return out;
}

emit::BunchWriter interact(uint64_t component_object_id) {
  const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
  const schema::RpcSchema *rpc =
      pawn.rpc("ServerInteractWithInteractableComponent");
  repl::InteractRequest r;
  r.interactable.ObjectId = component_object_id;
  r.interactable.ServerId = kServerId;
  r.interactable.Randomizer = kRandomizer;
  r.counter = 1;
  const auto params = repl::interact_params(*rpc, r);
  return params
             ? actor_rpc(pawn, "ServerInteractWithInteractableComponent",
                         *params)
             : emit::BunchWriter();
}

static emit::BunchWriter interrupt_channel() {
  return actor_rpc(class_named("PlayerPawn_C"), "ServerInterruptChannel", {});
}

size_t count_in_log(const char *what) {
  const std::string text = g_log.str();
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos;
       at = text.find(what, at + 1))
    ++n;
  return n;
}

static emit::BunchWriter item_used(int storage, int slot,
                                   const std::string &guid_hex) {
  const schema::ClassSchema &pawn = class_named("PlayerPawn_C");
  const schema::RpcSchema *rpc = pawn.rpc("ServerItemUsed");
  repl::RpcParams p;
  const auto g = aoc::worldd::items::guid_of(guid_hex);
  const repl::GuidValue gv = g ? *g : repl::GuidValue{};
  repl::set_rpc_param(*rpc, p, "Storage",
                      repl::Value::u64(static_cast<uint64_t>(storage)));
  repl::set_rpc_param(*rpc, p, "Slot", repl::Value::i64(slot));
  repl::set_rpc_param(*rpc, p, "ExpectedItem.A",
                      repl::Value::i64(static_cast<int32_t>(gv.a)));
  repl::set_rpc_param(*rpc, p, "ExpectedItem.B",
                      repl::Value::i64(static_cast<int32_t>(gv.b)));
  repl::set_rpc_param(*rpc, p, "ExpectedItem.C",
                      repl::Value::i64(static_cast<int32_t>(gv.c)));
  repl::set_rpc_param(*rpc, p, "ExpectedItem.D",
                      repl::Value::i64(static_cast<int32_t>(gv.d)));
  repl::set_rpc_param(
      *rpc, p, "RequestedEquipSlot",
      repl::Value::i64(static_cast<int32_t>(0xFFFFFFFFu)));
  return actor_rpc(pawn, "ServerItemUsed", p);
}

void play(Player &p) {
  p.rig.settle();
  p.request(
      actor_rpc(class_named("PlayerPawn_C"), "ServerClientIsReadyToPhaseIn", {}),
      p.pawn_ch);
  p.rig.run(std::chrono::seconds(2));
  p.rig.settle();
}

bool run_until(Rig &rig, const std::function<bool()> &done,
               std::chrono::milliseconds limit) {
  for (std::chrono::milliseconds t{0}; t < limit;
       t += std::chrono::milliseconds(20)) {
    if (done())
      return true;
    rig.step();
  }
  return done();
}

aoc::worldd::svc::Character record_here(const char *id_hex, const char *name) {
  aoc::worldd::svc::Character c = record_of(id_hex, name);
  c.has_position = true;
  c.position.x = kCx;
  c.position.y = kCy;
  c.position.z = bs::kDefaultSpawn.z;
  return c;
}

uint64_t pawn_id(const Player &p) {
  return p.s->bootstrap().setup().guids.pawn.ObjectId;
}

bool dead_for_clients(const aoc::worldd::world::WorldActor *a) {
  if (!a)
    return false;
  for (const repl::Subobject &s : a->actor.subobjects) {
    if (!s.obj.schema || std::string(s.obj.schema->name) != "UNPCCombatInfo")
      continue;
    const repl::Value *v = s.obj.state.get(*s.obj.schema, "bIsDead");
    const bool *b = v ? v->get_if<bool>() : nullptr;
    return b && *b;
  }
  return false;
}

static void read_channeling_chain(
    const schema::ClassSchema &info, const fake::Delivered &d, size_t idx,
    std::vector<std::pair<size_t, repl::ChannelingData>> &out) {
  const auto blocks =
      repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
  if (!blocks)
    return;
  for (const auto &b : *blocks) {
    if (b.header.is_actor || !b.header.has_rep_layout)
      continue;
    size_t pos = b.payload_pos;
    const auto st = repl::read_rep_block(info, d.content.data(),
                                         b.payload_pos + b.payload_bits, pos);
    const repl::Value *v = st ? st->get(info, "ChannelingData") : nullptr;
    if (v && v->get_if<repl::ChannelingData>())
      out.emplace_back(idx, *v->get_if<repl::ChannelingData>());
  }
}

std::vector<std::pair<size_t, repl::ChannelingData>>
channeling_seen(const Player &p, size_t from) {
  std::vector<std::pair<size_t, repl::ChannelingData>> out;
  const schema::ClassSchema &info = class_named("UCharacterSecondaryInfo");
  for (size_t i = from; i < p.got.size(); ++i) {
    if (p.got[i].channel == p.pawn_ch && !p.got[i].open && !p.got[i].close) {
      read_channeling_chain(info, p.got[i], i, out);
    }
  }
  return out;
}

static void
read_pc_chain_rpc(const fake::Delivered &d, size_t idx,
                  std::vector<std::pair<size_t, std::string>> &out) {
  const auto blocks =
      repl::read_content_blocks(d.content.data(), d.content.bit_pos(), 0);
  if (!blocks)
    return;
  for (const auto &b : *blocks) {
    if (!b.header.is_actor || b.header.has_rep_layout)
      continue;
    size_t pos = b.payload_pos;
    auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"),
                                       d.content.data(),
                                       b.payload_pos + b.payload_bits, pos);
    if (!calls)
      continue;
    for (const auto &c : *calls)
      out.emplace_back(idx, c.rpc ? c.rpc->name : "?");
  }
}

std::vector<std::pair<size_t, std::string>> pc_chain_calls(const Player &p,
                                                           size_t from) {
  std::vector<std::pair<size_t, std::string>> out;
  for (size_t i = from; i < p.got.size(); ++i) {
    if (p.got[i].channel == p.pc_ch && !p.got[i].open && !p.got[i].close) {
      read_pc_chain_rpc(p.got[i], i, out);
    }
  }
  return out;
}

std::vector<std::string> pc_call_names_since(const Player &p, size_t from) {
  std::vector<std::string> out;
  for (const auto &[chain, name] : pc_chain_calls(p, from))
    out.push_back(name);
  return out;
}

size_t count_names(const std::vector<std::string> &v, const char *name) {
  return static_cast<size_t>(std::count(v.begin(), v.end(), name));
}

static void read_corpse_arrays(const schema::ClassSchema &inter,
                               const repl::RepState &st, CorpseSeen &out) {
  const repl::Value *can = st.get(inter, "CanInputActivate");
  const auto *can_arr = can ? can->get_if<repl::ArrayValue>() : nullptr;
  if (can_arr) {
    std::vector<std::optional<bool>> entries;
    for (const repl::ElementState &e : can_arr->elements) {
      const auto w = e.values.find(1);
      const bool *x =
          (w == e.values.end()) ? nullptr : w->second.get_if<bool>();
      entries.push_back(x ? std::optional<bool>(*x) : std::nullopt);
    }
    out.can_input.push_back(std::move(entries));
  }
  const repl::Value *states = st.get(inter, "CurrentInputStates");
  const auto *sa = states ? states->get_if<repl::ArrayValue>() : nullptr;
  if (sa) {
    std::vector<uint64_t> s;
    for (const repl::ElementState &e : sa->elements) {
      const auto w = e.values.find(1);
      s.push_back((w != e.values.end() && w->second.get_if<uint64_t>())
                      ? *w->second.get_if<uint64_t>()
                      : 99);
    }
    out.last_states = std::move(s);
  }
}

static void read_corpse_rep_state(const schema::ClassSchema &inter,
                                  const repl::RepState &st, CorpseSeen &out) {
  const repl::Value *v = st.get(inter, "InputEffectStatuses");
  const auto *arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
  if (arr && !arr->elements.empty()) {
    const auto it = arr->elements[0].values.find(1);
    if (it != arr->elements[0].values.end() && it->second.get_if<uint64_t>()) {
      out.statuses.push_back(
          static_cast<uint32_t>(*it->second.get_if<uint64_t>()));
    }
  }
  read_corpse_arrays(inter, st, out);
}

static void scan_corpse_blocks(const schema::ClassSchema &inter,
                               const uint8_t *data,
                               const std::vector<repl::BlockView> &blocks,
                               uint64_t comp_id, CorpseSeen &out) {
  for (const auto &b : blocks) {
    if (b.header.is_actor || b.header.object.ObjectId != comp_id ||
        !b.header.has_rep_layout)
      continue;
    size_t pos = b.payload_pos;
    const auto st =
        repl::read_rep_block(inter, data, b.payload_pos + b.payload_bits, pos);
    if (st)
      read_corpse_rep_state(inter, *st, out);
  }
}

CorpseSeen corpse_seen(const Player &p, uint64_t comp_id, uint32_t channel,
                       size_t from) {
  CorpseSeen out;
  if (channel == 0)
    return out;
  const schema::ClassSchema &inter = class_named("UInteractableComponent");
  for (size_t i = from; i < p.got.size(); ++i) {
    if (p.got[i].channel != channel || p.got[i].open || p.got[i].close)
      continue;
    const auto blocks = repl::read_content_blocks(
        p.got[i].content.data(), p.got[i].content.bit_pos(), 0);
    if (!blocks)
      continue;
    ++out.chains;
    scan_corpse_blocks(inter, p.got[i].content.data(), *blocks, comp_id, out);
  }
  return out;
}

static void check_plain_wolf_loot(HuntRig &h) {
  Player &p = *h.p;
  Rig &rig = h.rig;
  uint64_t looted = 0;
  for (int i = 0; i < 60 && !looted; ++i) {
    const uint64_t id = rig.spawn("Pop_Wolf", 200, -100 - 10 * i, 4);
    if (!id)
      break;
    h.kill_it(id);
    const combat::Corpse *c = rig.world->aftermath()->corpse_of(id);
    if (c && c->parts)
      looted = id;
  }
  CHECK(looted != 0, "a plain Wolf's kill leaves loot within sixty tries");
  if (looted) {
    const size_t started = p.s->harvests_started();
    const size_t refused_before = p.s->harvests_refused();
    h.interact_with(p, looted);
    CHECK(p.s->loot_opens() == 1 && p.s->harvests_started() == started &&
              p.s->harvests_refused() == refused_before &&
              !rig.world->registry().find(looted)->npc.interact_def,
          "a plain creature's corpse opens its loot window and starts no hunt");
  }
}

static void equip_bow_from_bag(Player &p, Rig &rig, milliseconds wait_ms) {
  int slot = -1;
  for (int s = 0; s < items::kBackpackSlots; ++s) {
    const svc::InventoryItem *it = p.s->inventory().at(Bag, s);
    if (it && it->instance_guid == guid_n(138))
      slot = s;
  }
  if (slot >= 0)
    p.send_now(item_used(3, slot, guid_n(138)), p.pawn_ch);
  rig.run(wait_ms);
}

static void test_refusals() {
  std::printf("the refusals of a hunt: logged, counted, nothing sent\n");
  HuntRig h;
  if (!h.open("Hunting_Bork"))
    return;
  Player &p = *h.p;
  Rig &rig = h.rig;
  const auto refused = [&](const char *kind) {
    return p.s->harvest_refusals().count(kind)
               ? p.s->harvest_refusals().at(kind)
               : size_t(0);
  };
  h.settle();
  size_t from = p.got.size();
  h.interact_with(p, h.wolf);
  CHECK(refused("not certified") == 1 && p.s->harvesting_node() == 0 &&
            channeling_seen(p, from).empty(),
        "a Novice hunter and an Apprentice kind (a Bork): not certified");
  CHECK(count_in_log("is refused (not certified)") >= 1, "the log says so");
  const uint64_t wolf = rig.spawn("Hunting_Wolf", 160, 40);
  h.settle();
  p.send_now(item_used(4, 38, guid_n(138)), p.pawn_ch);
  rig.run(milliseconds(100));
  CHECK(!p.s->inventory().at(Worn, 38), "(the Bow is taken off)");
  from = p.got.size();
  h.interact_with(p, wolf);
  CHECK(refused("no tool") == 1 && channeling_seen(p, from).empty() &&
            p.s->harvests_started() == 0,
        "no Bow in slot 38: no tool");
  equip_bow_from_bag(p, rig, milliseconds(100));
  CHECK(p.s->inventory().at(Worn, 38) != nullptr, "(the Bow is worn again)");
  const uint64_t far_wolf = rig.spawn("Hunting_Wolf", 1200, 0);
  h.settle();
  h.interact_with(p, far_wolf);
  CHECK(refused("out of range") == 1 && p.s->harvests_started() == 0,
        "12 m from the corpse: out of range");
  CHECK(count_in_log("[hunt] cyb: the hunt of") >= 3,
        "(each refusal is a [hunt] line)");
  check_plain_wolf_loot(h);
}

static bool start_wolf_hunt(HuntRig &h) {
  Player &p = *h.p;
  h.interact_with(p, h.wolf);
  h.rig.run(milliseconds(300));
  return p.s->harvesting_node() == h.wolf &&
         h.rig.board().state_of(h.wolf) == gather::NodeState::Channeling;
}

static bool hunt_nothing_granted(HuntRig &h, size_t items0, int64_t xp0) {
  Player &p = *h.p;
  return p.s->inventory().count(R0) == items0 &&
         p.s->professions().xp(hid::kHunting) == xp0 &&
         p.s->harvests_completed() == 0 &&
         h.rig.board().state_of(h.wolf) == gather::NodeState::Available;
}

static void check_cancel_by_interrupt(HuntRig &h, uint64_t comp, uint32_t ch,
                                      size_t items0, int64_t xp0) {
  Player &p = *h.p;
  Rig &rig = h.rig;
  const size_t from = p.got.size();
  CHECK(start_wolf_hunt(h), "a hunt starts");
  p.send_now(interrupt_channel(), p.pawn_ch);
  CHECK(p.s->harvesting_node() == 0 &&
            rig.board().state_of(h.wolf) == gather::NodeState::Available &&
            p.s->harvests_cancelled() == 1,
        "ServerInterruptChannel: the hunt ends and the creature is free");
  CHECK(rig.world->aftermath()->corpse_of(h.wolf) == nullptr &&
            rig.world->registry().find(h.wolf) != nullptr &&
            !dead_for_clients(rig.world->registry().find(h.wolf)),
        "and it is alive as it was: no record, still in the world");
  rig.run(milliseconds(300));
  const auto ch1 = channeling_seen(p, from);
  CHECK(ch1.size() == 2 && ch1[1].second.flags == 0x106 &&
            !ch1[1].second.active,
        "the pawn's ChannelingData was cleared");
  const CorpseSeen back = corpse_seen(p, comp, ch, from);
  bool can_ok = !back.can_input.empty();
  for (const auto &c : back.can_input)
    can_ok = can_ok && c.size() == 4;
  CHECK(!back.statuses.empty() && back.statuses.back() == 0u && can_ok &&
            back.can_input.back()[0] && *back.can_input.back()[0],
        "the corpse was told: status 0, CanInputActivate four entries (the "
        "first true)");
  CHECK(back.last_states.empty() ||
            back.last_states == std::vector<uint64_t>({1, 1, 1, 1}),
        "(and its four interaction states are 1: they were never changed, so "
        "none is sent)");
  rig.run(seconds(16));
  CHECK(hunt_nothing_granted(h, items0, xp0),
        "and nothing comes when the time would have been up");
}

static void check_cancel_by_move(HuntRig &h, size_t items0, int64_t xp0) {
  Player &p = *h.p;
  Rig &rig = h.rig;
  CHECK(start_wolf_hunt(h), "a hunt starts again");
  p.s->gm_teleport(p.conn, kCx + 70, kCy, bs::kDefaultSpawn.z, rig.t,
                   "the test: a step away");
  rig.run(milliseconds(300));
  CHECK(p.s->harvesting_node() == 0 && p.s->harvests_cancelled() == 2 &&
            rig.board().state_of(h.wolf) == gather::NodeState::Available,
        "70 cm from where it began: the hunt ends");
  CHECK(count_in_log("the player moved 70 cm since the channel began") == 1,
        "the log says why");
  rig.run(seconds(16));
  CHECK(hunt_nothing_granted(h, items0, xp0), "no reward");
  p.s->gm_teleport(p.conn, kCx, kCy, bs::kDefaultSpawn.z, rig.t,
                   "the test: back");
  rig.run(milliseconds(400));
}

static void check_cancels_unequip_death_disconnect(HuntRig &h, int64_t xp0) {
  Player &p = *h.p;
  Rig &rig = h.rig;
  CHECK(start_wolf_hunt(h), "a hunt starts a third time");
  p.send_now(item_used(4, 38, guid_n(138)), p.pawn_ch);
  rig.run(milliseconds(100));
  CHECK(!p.s->inventory().at(Worn, 38) && p.s->harvesting_node() == 0 &&
            rig.board().state_of(h.wolf) == gather::NodeState::Available,
        "the Bow is taken off: the hunt ends");
  equip_bow_from_bag(p, rig, milliseconds(200));
  CHECK(p.s->inventory().at(Worn, 38) != nullptr, "(the Bow is worn again)");
  CHECK(start_wolf_hunt(h), "a hunt starts a fourth time");
  p.s->take_damage(p.conn, 1.0e9, rig.t);
  rig.run(milliseconds(300));
  CHECK(p.s->dead() && p.s->harvesting_node() == 0 &&
            rig.board().state_of(h.wolf) == gather::NodeState::Available,
        "the player dies: the hunt ends and the corpse is free");
  rig.run(seconds(16));
  CHECK(p.s->harvests_completed() == 0 &&
            p.s->professions().xp(hid::kHunting) == xp0,
        "...with no reward");
  Player &q =
      rig.add_player(record_here("0000000000000000000000000000cc01", "carol"));
  play(q);
  h.interact_with(q, h.wolf);
  rig.run(milliseconds(300));
  CHECK(q.s->harvesting_node() == h.wolf &&
            rig.board().state_of(h.wolf) == gather::NodeState::Channeling,
        "a second player hunts the corpse");
  q.s->finish("the test ends the session");
  CHECK(rig.board().state_of(h.wolf) == gather::NodeState::Available,
        "the session ends mid-hunt: the corpse is free at once");
  CHECK(count_in_log("the hunt of Hunting_Wolf") >= 4,
        "(each cancel is a [hunt] line)");
}

static void test_cancels() {
  std::printf("a hunt that ends with no reward: the corpse as it was, hunted "
              "again at once\n");
  HuntRig h;
  if (!h.open())
    return;
  Player &p = *h.p;
  Rig &rig = h.rig;
  h.settle();
  const uint64_t comp = rig.component_of(h.wolf);
  const uint32_t ch = p.s->actor_view()->channel_of(h.wolf);
  const size_t items0 = p.s->inventory().count(R0);
  const int64_t xp0 = p.s->professions().xp(hid::kHunting);
  check_cancel_by_interrupt(h, comp, ch, items0, xp0);
  check_cancel_by_move(h, items0, xp0);
  check_cancels_unequip_death_disconnect(h, xp0);
}

static void test_two_players() {
  std::printf("two players on one corpse: the second is refused until the "
              "first stops\n");
  HuntRig h;
  if (!h.open())
    return;
  Rig &rig = h.rig;
  Player &a = *h.p;
  Player &b =
      rig.add_player(record_here("0000000000000000000000000000cc01", "carol"));
  play(b);
  h.settle();
  h.interact_with(a, h.wolf);
  rig.run(milliseconds(300));
  CHECK(a.s->harvesting_node() == h.wolf, "the first player hunts the corpse");
  h.interact_with(b, h.wolf);
  CHECK(b.s->harvesting_node() == 0 &&
            b.s->harvest_refusals().count("corpse busy") == 1,
        "the second is refused: corpse busy");
  CHECK(rig.board().state_of(h.wolf) == gather::NodeState::Channeling &&
            a.s->harvesting_node() == h.wolf,
        "the first goes on");
  a.send_now(interrupt_channel(), a.pawn_ch);
  h.interact_with(b, h.wolf);
  rig.run(milliseconds(200));
  CHECK(b.s->harvesting_node() == h.wolf,
        "the first stops: the second can start");
}

static void test_flood() {
  std::printf("a flood of interactions: bounded work, one hunt, the rest "
              "dropped or counted\n");
  HuntRig h;
  if (!h.open())
    return;
  Rig &rig = h.rig;
  Player &p = *h.p;
  h.settle();
  const uint64_t comp = rig.component_of(h.wolf);
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 400; ++i) {
    p.client_sends({p.client.actor(p.pawn_ch, interact(comp), true)});
    if (i % 50 == 49)
      rig.run(milliseconds(40));
  }
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - begin)
                        .count();
  rig.run(milliseconds(200));
  CHECK(p.s->harvests_started() == 1, "400 interactions in a row: one hunt");
  CHECK(p.s->harvests_refused() <= 30,
        "the budget drops the rest before the hunt's checks (at most a burst "
        "and a few per second are counted)");
  char msg[100];
  std::snprintf(msg, sizeof msg, "(400 interactions took %.0f ms)", ms);
  CHECK(ms < 3000.0, msg);
}

static void test_full_bag() {
  std::printf("a full resource bag: the rest of the hunt goes to a pile, the "
              "corpse is hunted all the same\n");
  Rig rig;
  if (!rig.open(g_path))
    return;
  svc::Character rec = record_here("bbec5c86eb101fe5f38c87f0fa22e7d0", "cyb");
  for (int slot = 0; slot < 24; ++slot) {
    rec.items.push_back(
        make_item(R0, slot, hid::kCoal, 300 + static_cast<uint32_t>(slot), 20));
  }
  Player &p = rig.add_player(std::move(rec));
  const uint64_t wolf = rig.spawn("Hunting_Wolf", 150, 0);
  play(p);
  rig.run(milliseconds(500));
  const size_t from = p.got.size();
  p.send_now(interact(rig.component_of(wolf)), p.pawn_ch);
  CHECK(run_until(
            rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20)),
        "the hunt completes");
  rig.run(milliseconds(500));
  const auto names = pc_call_names_since(p, from);
  CHECK(count_names(names, "ClientReceiveStorageEvent") == 0 &&
            count_names(names, "ClientUpdateItemDurability") == 1,
        "no storage event; the Bow's wear all the same");
  CHECK(p.s->harvest_piles() == 1 && rig.world->aftermath()->stats().piles == 1,
        "the carcass is in a pile where the player stands");
  CHECK(p.s->bootstrap().callouts() >= 1, "and a callout says why");
}

static void test_gives_nothing() {
  std::printf("a kind that gives no item: the experience only\n");
  HuntRig h;
  if (!h.open("Hunting_Grem_Prototype"))
    return;
  Player &p = *h.p;
  Rig &rig = h.rig;
  const world::WorldActor *a = rig.world->registry().find(h.wolf);
  const gather::Gathering::HuntKind *kind =
      a ? rig.g().hunt_kind(a->npc.interact_def) : nullptr;
  CHECK(kind && kind->gives_nothing, "(the kind is one that gives nothing)");
  if (!kind || !kind->gives_nothing)
    return;
  h.settle();
  const size_t items0 = p.s->inventory().items().size();
  const int64_t xp0 = p.s->professions().xp(hid::kHunting);
  h.interact_with(p, h.wolf);
  CHECK(run_until(
            rig, [&] { return p.s->harvests_completed() == 1; }, seconds(20)),
        "the hunt completes");
  rig.run(milliseconds(300));
  CHECK(p.s->inventory().items().size() == items0 && p.s->harvest_piles() == 0,
        "no item, no pile");
  CHECK(p.s->professions().xp(hid::kHunting) >= xp0,
        "(Hunter XP is the table's: none when its experience lines are gated)");
  CHECK(count_in_log("this kind gives no item") == 1,
        "and the start's log line says the kind gives no item");
}

void run_hunting_session_refusals() {
  test_refusals();
  test_cancels();
  test_two_players();
  test_flood();
  test_full_bag();
  test_gives_nothing();
}
