// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "bootstrap/captured_character.h"
#include "cyb_appearance.h"
#include "protocol/repl/item_codec.h"
#include "protocol/schema/generated/index.h"

#include "test_world_items_common.h"

using namespace aoc::worldd;

static std::vector<BunchWriter> param_bits_of(const bs::Send &s, const schema::ClassSchema &cls) {
  std::vector<BunchWriter> out;
  const auto blocks = repl::read_content_blocks(s.emission.payload.data(), s.emission.payload.bit_pos(), 0);
  if (!blocks)
    return out;
  for (const auto &b : *blocks) {
    size_t pos = b.payload_pos;
    const auto h = repl::read_field_header(cls, s.emission.payload.data(), pos + b.payload_bits, pos);
    if (!h)
      return {};
    BunchWriter w;
    w.write_bit_range(s.emission.payload.data(), pos, h->payload_bits);
    out.push_back(std::move(w));
  }
  return out;
}

static bool bits_equal(const BunchWriter &w, const cap::Rpc &c) {
  const auto want = capture_blocks::bytes_of(c.hex);
  return w.bit_pos() == c.bits && capture_blocks::same_bit_range(w.data(), want.data(), c.bits, nullptr);
}
// -- C | O | A . Cyb --

struct Corpus {
  std::vector<bs::StoredItem> items;
  std::map<int, repl::StoragePayload> payload;
  std::vector<const cap::Rpc *> events;
  std::vector<const cap::Rpc *> payloads;
  repl::GuidValue character;
  emit::FIntrepidNetworkGUID guids[3];
  std::map<int, repl::StoragePayload> resource_payload;
  std::map<int, const cap::Rpc *> resource_rpc;
  std::vector<const cap::Rpc *> resource_events[3];
  std::vector<bs::StoredItem> resource_items;
};

static void store_corpus_resource_payload(Corpus &out, const cap::Rpc &c, const repl::StoragePayload &p) {
  const int index = p.maximum_items == 24 ? 0 : p.maximum_items == 40 ? 1 : p.maximum_items == 30 ? 2 : -1;
  if (index < 0 || out.resource_payload.count(index))
    return;
  out.resource_payload[index] = p;
  out.resource_rpc[index] = &c;
  for (const repl::StorageSlot &s : p.storage) {
    if (!s.item.has_item)
      continue;
    bs::StoredItem si;
    si.storage = static_cast<bs::ItemStorage>(static_cast<int>(bs::ItemStorage::Resource0) + index);
    si.slot = s.slot;
    si.item = s.item;
    out.resource_items.push_back(std::move(si));
  }
}

static bool read_corpus_payload_rpc(Corpus &out, const cap::Rpc &c) {
  if (std::string(c.rpc) != "ClientReceiveStoragePayload")
    return true;
  const auto call = decode_captured(c, "PlayerPawn_C");
  const auto p = call ? repl::parse_storage_payload(*call) : std::nullopt;
  if (!p)
    return false;
  int storage = -1;
  if (p->maximum_items == 70)
    storage = static_cast<int>(bs::ItemStorage::Backpack);
  else if (p->maximum_items == 83)
    storage = static_cast<int>(bs::ItemStorage::Equipment);
  else if (p->maximum_items == 100)
    storage = static_cast<int>(bs::ItemStorage::Quest);
  if (storage < 0) {
    store_corpus_resource_payload(out, c, *p);
    return true;
  }
  if (out.payload.count(storage))
    return true;
  out.payload[storage] = *p;
  out.payloads[static_cast<size_t>(storage)] = &c;
  out.guids[storage] = p->target;
  for (const repl::StorageSlot &s : p->storage) {
    if (!s.item.has_item)
      continue;
    bs::StoredItem si;
    si.storage = static_cast<bs::ItemStorage>(storage);
    si.slot = s.slot;
    si.item = s.item;
    out.items.push_back(std::move(si));
  }
  return true;
}

static void append_corpus_resource_event(Corpus &out, const cap::Rpc &c, const emit::FIntrepidNetworkGUID &obj) {
  for (const auto &[index, payload] : out.resource_payload) {
    if (obj == payload.target)
      out.resource_events[index].push_back(&c);
  }
}

static bool read_corpus_event_rpc(Corpus &out, const cap::Rpc &c, bool &have_character) {
  if (std::string(c.rpc) != "ClientReceiveStorageEvent" || c.pc == 0)
    return true;
  const auto call = decode_captured(c, "AoCPlayerControllerBP_C");
  const auto e = call ? repl::parse_storage_event(*call) : std::nullopt;
  if (!e || e->targets.size() != 1)
    return false;
  const auto &obj = e->targets[0].object;
  if (obj != out.guids[0] && obj != out.guids[1] && obj != out.guids[2]) {
    append_corpus_resource_event(out, c, obj);
    return true;
  }
  out.events.push_back(&c);
  if (!have_character && e->type == 0) {
    out.character = e->instigator;
    have_character = true;
  }
  return true;
}

template <size_t N> static std::optional<Corpus> read_corpus(const cap::Rpc (&rpcs)[N]) {
  Corpus out;
  out.payloads.assign(3, nullptr);
  for (const cap::Rpc &c : rpcs) {
    if (!read_corpus_payload_rpc(out, c))
      return std::nullopt;
  }
  if (out.payload.size() != 3)
    return std::nullopt;
  bool have_character = false;
  for (const cap::Rpc &c : rpcs) {
    if (!read_corpus_event_rpc(out, c, have_character))
      return std::nullopt;
  }
  return have_character ? std::optional<Corpus>(std::move(out)) : std::nullopt;
}
// [C_Y_B ~ CoA]

static bs::PlayerSetup setup_of(const Corpus &c) {
  auto s = bs::captured_character_setup();
  if (!s) {
    std::printf("  [FAIL] the captured character does not build\n");
    std::exit(2);
  }
  s->character_guid = {c.character.a, c.character.b, c.character.c, c.character.d};
  s->guids.backpack = c.guids[0];
  s->guids.equipment = c.guids[1];
  s->guids.quest_storage = c.guids[2];
  s->items = c.items;
  for (const auto &[index, payload] : c.resource_payload) {
    s->guids.resource_storage[static_cast<size_t>(index)] = payload.target;
    bs::ResourceBagSetup &bag = s->resource_bags[static_cast<size_t>(index)];
    bag.present = true;
    bag.columns = static_cast<int>(payload.dimension.x);
    bag.rows = static_cast<int>(payload.dimension.y);
    bag.override_matrix.clear();
    for (const std::vector<int32_t> &column : payload.override_matrix)
      bag.override_matrix.emplace_back(column.begin(), column.end());
  }
  for (const bs::StoredItem &it : c.resource_items)
    s->items.push_back(it);
  return std::move(*s);
}

static int held_array_key(const bs::PlayerBootstrap &pb) {
  const repl::Subobject *sub = pb.pawn().find(pb.setup().guids.base_character_info);
  const schema::NetField *f = sub && sub->obj.schema ? sub->obj.schema->field("HeldItems") : nullptr;
  const repl::FastArrayState *a = f ? sub->obj.arrays.find(f->index) : nullptr;
  return a ? a->array_key() : -1;
}

static std::vector<repl::ItemStorageEvent> parse_send_storage_events(const bs::Send &send, const schema::ClassSchema &pc_cls) {
  std::vector<repl::ItemStorageEvent> out;
  const auto blocks = repl::read_content_blocks(send.emission.payload.data(), send.emission.payload.bit_pos(), 0);
  if (!blocks)
    return out;
  const schema::RpcSchema *rpc = pc_cls.rpc("ClientReceiveStorageEvent");
  if (!rpc)
    return {};
  for (const auto &b : *blocks) {
    size_t pos = b.payload_pos;
    const auto h = repl::read_field_header(pc_cls, send.emission.payload.data(), pos + b.payload_bits, pos);
    if (!h)
      return {};
    const auto params = repl::read_rpc_params(*rpc, send.emission.payload.data(), pos + h->payload_bits, pos);
    const auto ev = params ? repl::parse_storage_event(*rpc, *params) : std::nullopt;
    if (!ev)
      return {};
    out.push_back(*ev);
  }
  return out;
}

static bool events_match_scenario(const capture_equip::Scenario &sc, const bs::PlayerBootstrap &pb,
                                  const std::vector<repl::ItemStorageEvent> &rpcs, size_t event_index) {
  for (size_t i = 0; i < rpcs.size() && event_index + i < sc.nevents; ++i) {
    const auto &want = sc.events[event_index + i];
    const auto &got = rpcs[i];
    const bool inst_ok = !want.instigated ? got.instigator == repl::GuidValue{} : !(got.instigator == repl::GuidValue{});
    const emit::FIntrepidNetworkGUID want_obj = want.container == 0 ? pb.setup().guids.backpack : pb.setup().guids.equipment;
    const bool one_ok = got.type == want.type && got.targets.size() == 1 && got.targets[0].slots.size() == 1 &&
                        got.targets[0].slots[0].slot == want.slot && got.targets[0].object == want_obj &&
                        static_cast<int64_t>(got.item.record_id) == want.record &&
                        aoc::worldd::items::guid_hex(got.item.guid) == want.guid && inst_ok;
    if (!one_ok)
      return false;
  }
  return true;
}

static bs::PlayerSetup make_bootstrap_setup_from_inv(const bs::PlayerSetup &base, const Inv &inv) {
  bs::PlayerSetup setup = base;
  setup.items.clear();
  for (const aoc::worldd::svc::InventoryItem &it : inv.items()) {
    bs::StoredItem si;
    si.storage = it.container == Bag ? bs::ItemStorage::Backpack : it.container == Worn ? bs::ItemStorage::Equipment : bs::ItemStorage::Quest;
    si.slot = it.slot;
    si.item = aoc::worldd::items::to_instance(it);
    setup.items.push_back(si);
  }
  setup.held_items.array_key = bs::kInitialHeldItemsKey;
  setup.held_items.items = bs::held_items_for(inv);
  setup.appearance_ids = aoc::worldd::items::appearance_ids(inv);
  return setup;
}

static std::vector<bs::ItemEvent> build_bootstrap_item_events(const std::vector<aoc::worldd::items::Event> &src, size_t &marks) {
  std::vector<bs::ItemEvent> events;
  marks = 0;
  for (const aoc::worldd::items::Event &e : src) {
    bs::ItemEvent ev;
    ev.type = e.type;
    ev.storage = e.container == Bag ? bs::ItemStorage::Backpack : bs::ItemStorage::Equipment;
    ev.slot = e.slot;
    ev.item = aoc::worldd::items::to_instance(e.item);
    ev.instigated = e.instigated;
    events.push_back(ev);
    marks += e.container == Worn ? 1 : 0;
  }
  return events;
}

static bool apply_bootstrap_scenario_requests(const capture_equip::Scenario &sc, const FakeCatalog &catalog, Inv &inv,
                                              bs::PlayerBootstrap &pb, bool &applied) {
  const schema::ClassSchema &pc_cls = *pb.player_controller().obj.schema;
  size_t event_index = 0;
  bool sends_match = true;
  applied = true;
  for (size_t r = 0; r < sc.nrequests; ++r) {
    const aoc::worldd::items::Outcome o = inv.use(sc.requests[r].storage == 3 ? Bag : Worn, sc.requests[r].slot, sc.requests[r].guid, kAny, catalog);
    size_t marks = 0;
    const auto events = build_bootstrap_item_events(o.events, marks);
    const auto send = pb.item_events(events);
    if (!send)
      return false;
    const auto rpcs = parse_send_storage_events(*send, pc_cls);
    sends_match = sends_match && rpcs.size() == events.size() && events_match_scenario(sc, pb, rpcs, event_index);
    event_index += rpcs.size();
    if (o.equipment_changed)
      applied = applied && pb.apply_equipment(bs::held_items_for(inv), aoc::worldd::items::appearance_ids(inv), marks ? marks : 1);
  }
  return sends_match;
}

static void check_corpus_storage_payloads(bs::PlayerBootstrap &pb, const Corpus &c) {
  const schema::ClassSchema &pawn_cls = *pb.pawn().obj.schema;
  const struct {
    bs::StorageKind kind;
    const char *name;
  } kinds[] = {{bs::StorageKind::Backpack, "Backpack"}, {bs::StorageKind::Equipment, "Equipment"}, {bs::StorageKind::QuestStorage, "QuestStorage"}};
  for (size_t k = 0; k < 3; ++k) {
    const auto send = pb.storage_payload(kinds[k].kind, bs::Role::Pawn);
    const auto bits = send ? param_bits_of(*send, pawn_cls) : std::vector<BunchWriter>{};
    CHECK(bits.size() == 1 && bits_equal(bits[0], *c.payloads[k]),
          (std::string("the ") + kinds[k].name + " payload equals the capture's, " + std::to_string(c.payloads[k]->bits) + " bits").c_str());
  }
}

static bool collect_corpus_storage_adds(bs::PlayerBootstrap &pb, std::vector<BunchWriter> &mine) {
  const schema::ClassSchema &pc_cls = *pb.player_controller().obj.schema;
  for (const bs::ItemStorage s : {bs::ItemStorage::Backpack, bs::ItemStorage::Equipment, bs::ItemStorage::Quest}) {
    const auto sends = pb.storage_adds(s);
    CHECK(sends.has_value(), "the Adds of a storage build");
    if (!sends)
      return false;
    for (const bs::Send &send : *sends) {
      CHECK(send.role == bs::Role::PlayerController && send.reliable() && !send.opens_channel && send.emission.exports.empty(),
            "a chain of Adds is reliable, on the PlayerController, with no export");
      auto bits = param_bits_of(send, pc_cls);
      mine.insert(mine.end(), std::make_move_iterator(bits.begin()), std::make_move_iterator(bits.end()));
    }
  }
  return true;
}

static bool check_corpus_storage_payloads_and_adds(bs::PlayerBootstrap &pb, const Corpus &c) {
  check_corpus_storage_payloads(pb, c);
  std::vector<BunchWriter> mine;
  if (!collect_corpus_storage_adds(pb, mine))
    return false;
  bool order_ok = mine.size() == c.events.size();
  size_t first_bad = 0;
  for (size_t i = 0; order_ok && i < mine.size(); ++i) {
    if (!bits_equal(mine[i], *c.events[i])) {
      order_ok = false;
      first_bad = i;
    }
  }
  if (!order_ok)
    std::printf("    %zu events built, %zu captured; the first that differs is number %zu\n", mine.size(), c.events.size(), first_bad);
  CHECK(order_ok, "every ClientReceiveStorageEvent equals the capture's bit for bit, in the capture's order (the Backpack's, the Equipment's, the Quest storage's)");
  return true;
}

static size_t check_one_resource_container(bs::PlayerBootstrap &pb, const Corpus &c, int index, const cap::Rpc &rpc) {
  const schema::ClassSchema &pawn_cls = *pb.pawn().obj.schema;
  const schema::ClassSchema &pc_cls = *pb.player_controller().obj.schema;
  const auto send = pb.storage_payload(bs::resource_kind(index), bs::Role::Pawn);
  const auto bits = send ? param_bits_of(*send, pawn_cls) : std::vector<BunchWriter>{};
  const std::string name = "Resource" + std::to_string(index);
  CHECK(bits.size() == 1 && bits_equal(bits[0], rpc), (std::string("the ") + name + " payload equals the capture's, " + std::to_string(rpc.bits) + " bits").c_str());
  const auto st = static_cast<bs::ItemStorage>(static_cast<int>(bs::ItemStorage::Resource0) + index);
  const auto sends = pb.storage_adds(st);
  std::vector<BunchWriter> mine_r;
  for (size_t s = 0; sends && s < sends->size(); ++s) {
    auto b = param_bits_of((*sends)[s], pc_cls);
    mine_r.insert(mine_r.end(), std::make_move_iterator(b.begin()), std::make_move_iterator(b.end()));
  }
  bool same = sends.has_value() && mine_r.size() == c.resource_events[index].size();
  for (size_t i = 0; same && i < mine_r.size(); ++i)
    same = bits_equal(mine_r[i], *c.resource_events[index][i]);
  CHECK(same, (std::string("the ") + name + "'s " + std::to_string(c.resource_events[index].size()) + " Adds equal the capture's, bit for bit").c_str());
  return pb.item_count(st);
}

static void check_corpus_resource_containers(bs::PlayerBootstrap &pb, const Corpus &c, size_t expect_items) {
  const schema::ClassSchema &pc_cls = *pb.player_controller().obj.schema;
  size_t resource_items = 0;
  for (const auto &[index, rpc] : c.resource_rpc)
    resource_items += check_one_resource_container(pb, c, index, *rpc);
  CHECK(resource_items == c.resource_items.size(), "the bootstrap holds every item of the resource containers");
  const auto adds = pb.storage_adds(bs::ItemStorage::Backpack);
  bool small = adds.has_value();
  for (size_t i = 0; adds && i < adds->size(); ++i)
    small = small && param_bits_of((*adds)[i], pc_cls).size() <= bs::kMaxEventsPerChain;
  CHECK(small, "a chain holds at most kMaxEventsPerChain events");
  const size_t held_now = pb.item_count(bs::ItemStorage::Backpack) + pb.item_count(bs::ItemStorage::Equipment) + pb.item_count(bs::ItemStorage::Quest);
  CHECK(held_now == expect_items, "building the join's events leaves the bootstrap's copy as it was");
}

template <size_t N> static void test_corpus(const char *label, const cap::Rpc (&rpcs)[N], size_t expect_events, size_t expect_items) {
  std::printf("%s: the storage RPCs rebuilt from the items they hold\n", label);
  const auto c = read_corpus(rpcs);
  CHECK(c.has_value(), "the three storages' payloads and their events read");
  if (!c)
    return;
  CHECK(c->items.size() == expect_items && c->events.size() == expect_events,
        (std::to_string(c->items.size()) + " items in the payloads, " + std::to_string(c->events.size()) + " events for them").c_str());
  bs::PlayerBootstrap pb(setup_of(*c), bs::GameStateMode::Tier1);
  CHECK(pb.ok(), "the bootstrap builds from them");
  if (!pb.ok())
    return;
  const size_t total = pb.item_count(bs::ItemStorage::Backpack) + pb.item_count(bs::ItemStorage::Equipment) + pb.item_count(bs::ItemStorage::Quest);
  CHECK(total == expect_items, "and holds each of them");
  if (!check_corpus_storage_payloads_and_adds(pb, *c))
    return;
  check_corpus_resource_containers(pb, *c, expect_items);
}

static void test_captured_corpora() {
  test_corpus("the retail login (a level 20 Ranger)", cap::kLogin, 109, 64 + 29 + 16);
  test_corpus("the original server's Bard", cap::kBard, 13, 13);
}

static bool bits_match(const BunchWriter &w, const capture_blocks::Bits &b) {
  const auto want = capture_blocks::bytes_of(b.hex);
  size_t diff = 0;
  if (w.bit_pos() != b.bits) {
    std::printf("    wrote %zu bits, expected %zu (%s)\n", w.bit_pos(), b.bits, b.name);
    return false;
  }
  if (!capture_blocks::same_bit_range(w.data(), want.data(), b.bits, &diff)) {
    std::printf("    first differing bit %zu (%s)\n", diff, b.name);
    return false;
  }
  return true;
}
// (CoA) (C_Y_B)

static std::vector<svc::InventoryItem> items_of(const Corpus &c) {
  std::vector<svc::InventoryItem> out;
  for (const bs::StoredItem &si : c.items) {
    const ItemContainer container = si.storage == bs::ItemStorage::Backpack ? Bag : si.storage == bs::ItemStorage::Equipment ? Worn : ItemContainer::Quest;
    const auto it = items::item_of(si.item, container, si.slot);
    if (!it) {
      std::printf("  [FAIL] a captured item does not make an inventory item\n");
      std::exit(2);
    }
    out.push_back(*it);
  }
  return out;
}

static void test_second_state_from_the_kit() {
  std::printf("the captured Bard from its kit: the pawn's second state is pkt#107 as before\n");
  const auto corpus = read_corpus(cap::kBard);
  if (!corpus) {
    CHECK(false, "the Bard's payloads read");
    return;
  }
  const Inv inv = inventory_of(items_of(*corpus));
  auto base = bs::captured_character_setup();
  if (!base)
    std::exit(2);
  bs::PlayerSetup setup = *base;
  setup.held_items.items.clear();
  setup.appearance_ids.clear();
  setup.items = corpus->items;
  setup.held_items.array_key = bs::kInitialHeldItemsKey;
  setup.held_items.items = bs::held_items_for(inv);
  setup.appearance_ids = items::appearance_ids(inv);
  CHECK(setup.held_items.items.size() == base->held_items.items.size(), "23 HeldItems entries");
  bool held_same = true;
  for (size_t i = 0; i < setup.held_items.items.size() && i < base->held_items.items.size(); ++i) {
    const auto &a = setup.held_items.items[i];
    const auto &b = base->held_items.items[i];
    held_same = held_same && a.id == b.id && a.slot == b.slot && a.item_guid == b.item_guid && a.enchantment == b.enchantment;
  }
  CHECK(held_same && setup.held_items.array_key == base->held_items.array_key, "each with the captured id, slot and record, and the captured array key");
  CHECK(setup.appearance_ids == base->appearance_ids, "AppearanceIDs are the captured four");
  bs::PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
  CHECK(pb.ok(), "it builds");
  pb.player_controller_open();
  pb.load_tracker_before_possession();
  pb.pawn_open();
  pb.restart_camera_rotation();
  pb.possession_update();
  const auto second = pb.pawn_second_state();
  CHECK(second && second->emission.exports.size() == 1 && bits_match(second->emission.exports[0], capture_blocks::kPawnPossessedExports0) &&
            bits_match(second->emission.payload, capture_blocks::kPawnPossessedContent),
        "pkt#107: the export bunch and the 78235-bit payload (HeldItems, AppearanceIDs, the stats, the dynamic resource storage) match bit for bit");
}

static const char *role_prefix(bs::Role role) {
  return role == bs::Role::Pawn ? "pawn " : role == bs::Role::PlayerController ? "pc " : role == bs::Role::GameState ? "gs " : "ps ";
}

static void check_join_with_items_order(const std::vector<bs::Send> &seq) {
  std::vector<std::string> names;
  for (const bs::Send &s : seq)
    names.push_back(std::string(role_prefix(s.role)) + s.what);
  auto is = [&](size_t idx, const char *role_what) { return idx < names.size() && names[idx] == role_what; };
  size_t i = 6;
  bool ok = true;
  for (int n = 0; n < 4; ++n)
    ok &= is(i++, "pc ClientReceiveStorageEvent");
  ok &= is(i++, "pawn pawn open") & is(i++, "pawn ClientReceiveStoragePayload");
  for (int n = 0; n < 2; ++n)
    ok &= is(i++, "pc ClientReceiveStorageEvent");
  ok &= is(i++, "pawn ClientReceiveStoragePayload") & is(i++, "pc ClientReceiveStorageEvent") & is(i++, "pawn ClientReceiveStoragePayload");
  for (int n = 0; n < 3; ++n)
    ok &= is(i++, "pc ClientReceiveStorageEvent") & is(i++, "pawn ClientReceiveStoragePayload");
  CHECK(ok, "the retail login's order: the Backpack's four chains of Adds, the pawn's open, the Backpack's payload; the Equipment's two and its payload; the Quest storage's one and its payload; each of the three resource containers' Adds and payload");
  CHECK(is(i, "pc ClientRestart, camera mode, rotation") && seq.size() == i + 5 && names.back() == "pc XPTracker",
        "then restart, possession, the pawn's second state, the PlayerState and (plan D1) the XPTracker");
}

static void check_join_with_items_limits(const std::vector<bs::Send> &seq) {
  bool limits = true;
  for (const bs::Send &s : seq) {
    for (const BunchWriter &e : s.emission.exports)
      limits = limits && e.bit_pos() <= 7621;
    const size_t content = s.emission.payload.bit_pos();
    const size_t parts = s.emission.exports.size() + (content > 7621 ? (content + 7615) / 7616 : 1);
    limits = limits && parts <= 256 && content <= 524288;
  }
  CHECK(limits, "every chain has at most 256 parts, every export bunch at most 7621 bits and no payload is over 524,288 bits");
  bool events_ok = true;
  for (const bs::Send &s : seq) {
    if (std::string(s.what) == "ClientReceiveStorageEvent")
      events_ok = events_ok && s.role == bs::Role::PlayerController && s.reliable() && !s.opens_channel;
  }
  CHECK(events_ok, "the events are reliable and on the PlayerController's open channel");
}

static void test_join_with_items() {
  std::printf("the join sequence of a character with items\n");
  const auto corpus = read_corpus(cap::kLogin);
  if (!corpus) {
    CHECK(false, "the retail login's storages read");
    return;
  }
  bs::PlayerBootstrap pb(setup_of(*corpus), bs::GameStateMode::Tier2);
  const auto seq = pb.join_sequence();
  CHECK(seq.has_value(), "the sequence builds");
  if (!seq)
    return;
  check_join_with_items_order(*seq);
  check_join_with_items_limits(*seq);
}
// [C:Y:B x CoA]

static void run_bootstrap_scenario(const capture_equip::Scenario &sc, const FakeCatalog &catalog, size_t &sends_ok, size_t &scenarios_ok) {
  Inv inv = inventory_before(sc);
  auto base = bs::captured_character_setup();
  if (!base)
    std::exit(2);
  bs::PlayerBootstrap pb(make_bootstrap_setup_from_inv(*base, inv), bs::GameStateMode::Tier2);
  if (!pb.ok() || !pb.join_sequence()) {
    std::printf("    %d: the bootstrap does not build or join: %s\n", sc.capture, pb.error().c_str());
    return;
  }
  const int key_before = held_array_key(pb);
  bool applied = true;
  const bool sends_match = apply_bootstrap_scenario_requests(sc, catalog, inv, pb, applied);
  const int moved = held_array_key(pb) - key_before;
  const auto update = pb.equipment_update();
  const auto again = pb.equipment_update();
  const bool key_ok = moved == sc.key_moved;
  if (!sends_match || !applied || !key_ok)
    std::printf("    %d: sends %d applied %d key moved %d, retail %d\n", sc.capture, int(sends_match), int(applied), moved, sc.key_moved);
  sends_ok += sends_match ? 1 : 0;
  scenarios_ok += (sends_match && applied && key_ok && update && !again) ? 1 : 0;
}

static void test_retail_answers_through_the_bootstrap() {
  std::printf("the retail answers through the bootstrap: the events the client is sent, and the HeldItems array key\n");
  const FakeCatalog catalog = catalog_of_captures();
  size_t scenarios_ok = 0, sends_ok = 0;
  for (const auto &sc : capture_equip::kScenarios)
    run_bootstrap_scenario(sc, catalog, sends_ok, scenarios_ok);
  CHECK(sends_ok == std::size(capture_equip::kScenarios),
        "in every capture the events the bootstrap builds read back as the retail events: type, storage object, slot, record, guid and instigator");
  CHECK(scenarios_ok == std::size(capture_equip::kScenarios),
        "and the HeldItems array key moves as the retail server's did (23 for each change of the Equipment storage: 23 for a chest piece, 69 for a two-hander over a main hand and an off hand), one update carries it and a second has nothing");
}
// (C_Y_B) (C O A)

static void check_restore_equipment_cycle(bs::PlayerBootstrap &pb, const Inv &inv, size_t marks, int moves, const char *what) {
  const int key0 = held_array_key(pb);
  const auto held = bs::held_items_for(inv);
  const auto app = items::appearance_ids(inv);
  const auto cp = pb.checkpoint();
  CHECK(pb.apply_equipment(held, app, marks) && held_array_key(pb) == key0 + moves,
        (std::string(what) + ": the first build moves the array key by " + std::to_string(moves)).c_str());
  const auto first = pb.equipment_update();
  CHECK(first.has_value(), "the update builds");
  pb.restore(cp);
  CHECK(held_array_key(pb) == key0, (std::string(what) + ": restore puts the array key back").c_str());
  CHECK(pb.apply_equipment(held, app, marks) && held_array_key(pb) == key0 + moves,
        (std::string(what) + ": the retry moves it once more, by the same " + std::to_string(moves) + ", not twice").c_str());
  const auto second = pb.equipment_update();
  CHECK(first && second && first->emission.payload.bit_pos() == second->emission.payload.bit_pos() &&
            capture_blocks::same_bit_range(first->emission.payload.data(), second->emission.payload.data(), first->emission.payload.bit_pos(), nullptr),
        "and builds the same update, bit for bit");
  CHECK(!pb.equipment_update(), "once sent, a second update has nothing");
}

static void test_restore_worn_kit(const bs::PlayerSetup &base, const FakeCatalog &catalog, bool two_hander) {
  std::vector<svc::InventoryItem> kit = bard_kit();
  if (two_hander)
    kit.push_back(make_item(Bag, 0, fi::kSword2H, 300));
  Inv inv = inventory_of(kit);
  bs::PlayerBootstrap pb(make_bootstrap_setup_from_inv(base, inv), bs::GameStateMode::Tier2);
  const char *what = two_hander ? "a two-hander over a main hand and an off hand" : "the chest off";
  if (!pb.ok() || !pb.join_sequence()) {
    CHECK(false, (std::string("the bootstrap does not join: ") + pb.error()).c_str());
    return;
  }
  const items::Outcome o = two_hander ? inv.use(Bag, 0, guid_n(300), kAny, catalog) : inv.use(Worn, 2, guid_n(102), kAny, catalog);
  size_t marks = 0;
  for (const items::Event &e : o.events)
    marks += e.container == Worn ? 1 : 0;
  CHECK(o.ok && o.equipment_changed && marks == (two_hander ? 3u : 1u),
        (std::string(what) + ": the request applies and changes the Equipment " + std::to_string(two_hander ? 3 : 1) + " time(s)").c_str());
  check_restore_equipment_cycle(pb, inv, marks, two_hander ? 69 : 23, what);
}

static void test_restore_naked_pawn(const bs::PlayerSetup &base, const FakeCatalog &catalog) {
  std::vector<svc::InventoryItem> kit;
  for (auto it : bard_kit()) {
    it.container = Bag;
    it.slot = static_cast<int>(kit.size());
    kit.push_back(it);
  }
  Inv inv = inventory_of(kit);
  bs::PlayerSetup setup = make_bootstrap_setup_from_inv(base, inv);
  setup.held_items.items.clear();
  setup.appearance_ids.clear();
  bs::PlayerBootstrap pb(setup, bs::GameStateMode::Tier2);
  if (!pb.ok() || !pb.join_sequence()) {
    CHECK(false, "the bootstrap with nothing worn does not join");
    return;
  }
  const int key0 = held_array_key(pb);
  Inv worn_one = inv;
  worn_one.use(Bag, 1, guid_n(102), kAny, catalog);
  const auto held = bs::held_items_for(worn_one);
  const auto app = items::appearance_ids(worn_one);
  const auto cp = pb.checkpoint();
  CHECK(pb.apply_equipment(held, app, 1), "the first update of a naked pawn applies");
  const int key1 = held_array_key(pb);
  const auto first = pb.equipment_update();
  pb.restore(cp);
  CHECK(held_array_key(pb) == key0, "restore: the array is as it was before the first update (the key too)");
  CHECK(pb.apply_equipment(held, app, 1) && held_array_key(pb) == key1, "the retry inserts again and ends on the same key");
  const auto second = pb.equipment_update();
  CHECK(first && second && first->emission.payload.bit_pos() == second->emission.payload.bit_pos() &&
            capture_blocks::same_bit_range(first->emission.payload.data(), second->emission.payload.data(), first->emission.payload.bit_pos(), nullptr),
        "and builds the same update, bit for bit");
}

static void test_restore_undoes_the_held_items() {
  std::printf("restore(checkpoint) puts HeldItems back\n");
  const FakeCatalog catalog = fixture_catalog();
  const auto base = bs::captured_character_setup();
  if (!base)
    std::exit(2);
  for (const bool two_hander : {false, true})
    test_restore_worn_kit(*base, catalog, two_hander);
  test_restore_naked_pawn(*base, catalog);
}
// Cyb - C O A

static svc::Character kit_record() {
  svc::Character c;
  c.id = "bbec5c86eb101fe5f38c87f0fa22e7d0";
  c.account_id = 3;
  c.name = "cyb";
  c.race_id = 0;
  c.gender_id = 0;
  c.class_id = 2;
  c.archetype_id = 17747;
  c.class_name = "Bard";
  c.appearance_json = cyb::kAppearanceJsonb;
  c.level = 1;
  return c;
}

static void check_setup_bard_appearance_and_notes(const bs::CharacterSetupResult &r) {
  CHECK(r.setup->appearance_ids.size() == 4 && r.setup->appearance_ids[0].item_equip_slot == 1 &&
            static_cast<int64_t>(r.setup->appearance_ids[1].record_guid) == fi::kChest,
        "AppearanceIDs: the helmet, chest, legs and feet");
  bool note = false;
  for (const std::string &n : r.notes)
    note = note || n.find("13") != std::string::npos || n.find("items:") != std::string::npos;
  CHECK(note, "the notes say how many items the character has");
}

static void test_setup_bard_kit(const content::ContentDb &content, const bs::LoginIdentity &login, const bs::SessionValues &session) {
  svc::Character cyb = kit_record();
  cyb.items = bard_kit();
  auto r = bs::build_player_setup(cyb, content, login, session, world_stats::of(content));
  CHECK(r.setup.has_value() && r.inventory.has_value(), "a Bard with its kit builds, with its Inventory");
  if (!r.setup || !r.inventory)
    return;
  CHECK(r.setup->items.size() == 13 && r.inventory->size() == 13, "thirteen items in the setup");
  const auto held = bs::held_items_for(*r.inventory);
  CHECK(r.setup->held_items.items.size() == 23 && r.setup->held_items.array_key == bs::kInitialHeldItemsKey &&
            r.setup->held_items.items[0].item_guid == fi::kScepter && r.setup->held_items.items[1].item_guid == fi::kFocus &&
            r.setup->held_items.items[2].item_guid == fi::kBook && r.setup->held_items.items[3].item_guid == 0,
        "HeldItems has the 23 slots with the Scepter, Focus and Book");
  bool same = held.size() == r.setup->held_items.items.size();
  for (size_t i = 0; same && i < held.size(); ++i)
    same = held[i].id == r.setup->held_items.items[i].id && held[i].item_guid == r.setup->held_items.items[i].item_guid;
  CHECK(same, "as held_items_for gives them");
  check_setup_bard_appearance_and_notes(r);
}

static void check_setup_refused_items(const content::ContentDb &content, const bs::LoginIdentity &login, const bs::SessionValues &session) {
  svc::InventoryItem zero_qty = make_item(Bag, 1, 5, 1);
  zero_qty.quantity = 0;
  const std::tuple<const char *, std::vector<svc::InventoryItem>, const char *> bad_cases[] = {
      {"two items in a slot", {make_item(Bag, 1, 5, 1), make_item(Bag, 1, 6, 2)}, "two items in Backpack slot 1"},
      {"a slot outside the storage", {make_item(Bag, 70, 5, 1)}, "slot 70"},
      {"one guid twice", {make_item(Bag, 1, 5, 1), make_item(Bag, 2, 6, 1)}, "instance guid"},
      {"a zero quantity", {zero_qty}, "quantity"}};
  for (const auto &[name, items_, want] : bad_cases) {
    svc::Character broken = kit_record();
    broken.items = items_;
    const auto b = bs::build_player_setup(broken, content, login, session, world_stats::of(content));
    CHECK(!b.setup && !b.inventory && b.error.find("items cannot be played") != std::string::npos && b.error.find(want) != std::string::npos,
          (std::string(name) + " refuse the login: " + b.error).c_str());
  }
}

static void test_setup_with_items() {
  std::printf("build_player_setup with items\n");
  const fs::path dir = cf::make_temp_dir("world_items_");
  const fs::path path = dir / "content.db";
  if (!cf::make_db(path))
    std::exit(2);
  std::string error;
  auto content = content::ContentDb::open(path.string(), error);
  if (!content) {
    std::printf("  [FAIL] the fixture does not open: %s\n", error.c_str());
    std::exit(2);
  }
  const bs::LoginIdentity login{"DESKTOP-TEST", "DESKTOP-TEST"};
  bs::SessionValues session;
  session.server_id = 60;
  test_setup_bard_kit(*content, login, session);
  svc::Character bare = kit_record();
  auto r = bs::build_player_setup(bare, *content, login, session, world_stats::of(*content));
  CHECK(r.setup && r.setup->items.empty() && r.setup->held_items.items.empty() && r.setup->appearance_ids.empty() && r.inventory && r.inventory->size() == 0,
        "a character with no items has none in its setup, no HeldItems and no AppearanceIDs");
  svc::Character bag = kit_record();
  bag.items = {make_item(Bag, 3, fi::kPotion, 1), make_item(ItemContainer::Quest, 0, 77, 2)};
  r = bs::build_player_setup(bag, *content, login, session, world_stats::of(*content));
  CHECK(r.setup && r.setup->items.size() == 2 && r.setup->held_items.items.empty() && r.setup->appearance_ids.empty(),
        "items in the bag and the quest storage are sent and show nothing");
  check_setup_refused_items(*content, login, session);
  std::error_code ec;
  content.reset();
  fs::remove_all(dir, ec);
}

static void check_fixture_db_records(content::ContentDb &db) {
  std::string error;
  const auto chest = db.item(fi::kChest, error);
  CHECK(chest && chest->guid == fi::kChest && chest->name == "Gear_Armor_Light_Starter_Chest" && !chest->can_equip &&
            !chest->occupies_all_equip_slots && chest->equip_slots == std::vector<uint8_t>{2},
        "the starter chest: slot 2, CanEquip 0 (and worn)");
  const auto focus = db.item(fi::kFocus, error);
  CHECK(focus && focus->equip_slots == std::vector<uint8_t>({16, 18}), "a Focus lists Secondary and RangedOffHand, in the record's order");
  const auto ear = db.item(fi::kEarLeftFirst, error);
  CHECK(ear && ear->equip_slots == std::vector<uint8_t>({14, 13}), "an earring that lists EarLeft first");
  const auto book = db.item(fi::kBook, error);
  CHECK(book && book->occupies_all_equip_slots && book->equip_slots == std::vector<uint8_t>({17, 18}), "a Book is two-handed");
  const auto potion = db.item(fi::kPotion, error);
  CHECK(potion && potion->equip_slots.empty(), "a potion lists no slot");
  error = "stale";
  const auto none = db.item(1234567, error);
  CHECK(!none && error.empty(), "an item the file does not know is no item and no error");
}

static void test_fixture_content_items() {
  const fs::path dir = cf::make_temp_dir("world_items_db_");
  const fs::path path = dir / "content.db";
  if (!cf::make_db(path))
    std::exit(2);
  std::string error;
  auto db = content::ContentDb::open(path.string(), error);
  CHECK(db.has_value(), "the fixture opens, with the item tables");
  if (db) {
    check_fixture_db_records(*db);
    items::ContentItemCatalog catalog(*db);
    const auto info = catalog.info(fi::kBook);
    CHECK(info && info->two_handed && info->wearable() && info->equip_slots.size() == 2 && !catalog.info(1234567) && !catalog.info(fi::kPotion)->wearable(),
          "ContentItemCatalog is the same facts");
  }
  cf::DbOptions o;
  o.item_tables = false;
  const fs::path old_path = dir / "old.db";
  if (!cf::make_db(old_path, o))
    std::exit(2);
  std::string why;
  const auto old = content::ContentDb::open(old_path.string(), why);
  CHECK(!old && why.find("rebuild it with this checkout's contentc") != std::string::npos, "a file built before plan B2 is refused with the way out");
  db.reset();
  std::error_code ec;
  fs::remove_all(dir, ec);
}

static void check_real_db_kit_and_two_hander(content::ContentDb &rdb, items::ContentItemCatalog &rc) {
  std::string e2;
  const auto bag = rdb.item(6064629382556287782ll, e2);
  CHECK(bag && bag->equip_slots == std::vector<uint8_t>({23, 24, 25}), "the real resource bag: the three bag slots");
  Inv kit = inventory_of({make_item(Bag, 0, 6064630214426886486ll, 1), make_item(Bag, 1, 6064631131688402948ll, 2),
                          make_item(Bag, 2, 6064631670974054529ll, 3), make_item(Bag, 3, 6064630695486029824ll, 4)});
  bool worn = true;
  for (int s = 0; s < 4; ++s)
    worn = worn && kit.use(Bag, s, guid_n(s + 1), kAny, rc).ok;
  CHECK(worn && kit.at(Worn, 2) && kit.at(Worn, 15) && kit.at(Worn, 17) && kit.at(Worn, 16) && !kit.at(Worn, 18),
        "from the real file: the chest, Scepter, Book and Focus are worn in Chest, Primary, Ranged and Secondary");
  const auto two = rdb.item(6064630695616315423ll, e2);
  CHECK(two && two->occupies_all_equip_slots && two->equip_slots == std::vector<uint8_t>({15, 16}), "the real Sword2H: Primary and Secondary, two-handed");
}

static void test_real_content_items() {
  const char *real = std::getenv("AOC_CONTENT_DB");
  if (!real || !*real) {
    std::printf("  (AOC_CONTENT_DB is not set: the real file's half is skipped)\n");
    return;
  }
  std::string e2;
  auto rdb = content::ContentDb::open(real, e2);
  CHECK(rdb.has_value(), "the real content.db opens");
  if (!rdb)
    return;
  items::ContentItemCatalog rc(*rdb);
  const auto chest = rdb->item(6064630214426886486ll, e2);
  CHECK(chest && !chest->can_equip && chest->equip_slots == std::vector<uint8_t>{2}, "the real starter chest: CanEquip 0 and slot 2");
  const auto book = rdb->item(6064631670974054529ll, e2);
  CHECK(book && book->occupies_all_equip_slots && book->equip_slots == std::vector<uint8_t>({17, 18}), "the real Book: two-handed, Ranged and RangedOffHand");
  check_real_db_kit_and_two_hander(*rdb, rc);
}

static void test_content_items() {
  std::printf("content.db's items\n");
  test_fixture_content_items();
  test_real_content_items();
}

int main() {
  test_captured_corpora();
  test_second_state_from_the_kit();
  test_join_with_items();
  test_retail_answers_through_the_bootstrap();
  test_restore_undoes_the_held_items();
  test_setup_with_items();
  test_content_items();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
