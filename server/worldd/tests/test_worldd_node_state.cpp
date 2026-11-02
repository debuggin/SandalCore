// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_node_state.cpp  (Nodes 1, stage 2: the world knows its nodes)
// ============================================================================
#include "test_worldd_node_state_common.h"

#include "capture_blocks.h"
#include "capture_nodes.h"
#include "world/node_constants.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

using namespace aoc::worldd;
namespace emit = aoc::protocol::emit;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using emit::BunchWriter;

struct Rebuilt {
  std::vector<uint8_t> bytes;
  std::vector<repl::FastArrayField> fields;
  bool ok = false;
};

static std::vector<uint8_t> bytes_of(const char *hex) {
  std::vector<uint8_t> out;
  auto nib = [](char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
  };
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
    out.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
  return out;
}

static Rebuilt read_maps(const char *hex, size_t bits) {
  Rebuilt r;
  r.bytes = bytes_of(hex);
  size_t pos = 0;
  auto fields = repl::read_fast_array_fields(class_named("AoCGameStateBP_C"), r.bytes.data(), bits, pos, {});
  if (!fields || pos != bits)
    return r;
  r.fields = std::move(*fields);
  r.ok = true;
  return r;
}

static repl::NodeStateMaps retail_login() {
  const auto &b = capture_blocks::kGameStateFastMaps28;
  const Rebuilt r = read_maps(b.hex, b.bits);
  return r.ok ? repl::node_maps_of_fields(r.fields) : repl::NodeStateMaps();
}

static void set_retail_levels(world::CityNodes &nodes, const repl::NodeStateMaps &m) {
  std::map<uint32_t, int> percent;
  for (const auto &[rank, p] : m.percent)
    percent[rank] = p;
  std::vector<world::LiveNode> live;
  for (const repl::NodeDataEntry &n : m.nodes) {
    world::LiveNode l;
    l.guid = n.guid;
    l.level = n.level;
    l.parent_guid = n.parent;
    const world::CityNodeInfo *info = nodes.by_guid(n.guid);
    const auto p = info ? percent.find(static_cast<uint32_t>(info->rank)) : percent.end();
    if (p != percent.end() && n.level < 4) {
      const int64_t lo = world::node_constants::kExpToLevel[n.level], hi = world::node_constants::kExpToLevel[n.level + 1];
      l.experience = p->second >= 100 ? hi : lo + (static_cast<int64_t>(p->second) * (hi - lo) + 99) / 100;
    }
    live.push_back(l);
  }
  nodes.apply_snapshot(live, world::LevelSource::Service);
}

static void check_retail_node_entries(const repl::NodeStateMaps &retail, const repl::NodeStateMaps &mine) {
  std::map<int64_t, repl::NodeDataEntry> by_guid;
  for (const repl::NodeDataEntry &n : mine.nodes)
    by_guid[n.guid] = n;
  size_t found = 0, level = 0, parent = 0, radius = 0, bounty = 0, tags = 0, culture = 0, mayors = 0;
  for (const repl::NodeDataEntry &r : retail.nodes) {
    const auto it = by_guid.find(r.guid);
    if (it == by_guid.end())
      continue;
    const repl::NodeDataEntry &o = it->second;
    ++found;
    level += o.level == r.level;
    parent += o.parent == r.parent;
    radius += o.deploy_radius == r.deploy_radius;
    bounty += o.bounty == r.bounty;
    tags += o.tags == r.tags;
    culture += o.culture == r.culture;
    mayors += o.mayor_name.empty() && o.mayor_guid == std::array<uint32_t, 4>{0, 0, 0, 0};
  }
  CHECKF(found == 46 && level == 46 && parent == 46, "all 46 of retail's nodes are there at retail's level and under retail's lord (%zu, %zu, %zu)", found, level, parent);
  CHECKF(radius == 46, "the vehicle deploy radius (the level's modification: 40000 from Village, 20000 for harbours) equals retail's for all 46 (%zu)", radius);
  CHECKF(bounty == 46 && tags == 46 && culture == 46, "the bounty commodity (0 for the ocean), the SiegeInactive tag and the culture equal retail's (%zu, %zu, %zu)", bounty, tags, culture);
  CHECK(mayors == 46, "no node has a mayor (nobody is one yet)");
}

static void check_retail_small_maps_and_estates(const repl::NodeStateMaps &retail, const repl::NodeStateMaps &mine) {
  std::map<uint32_t, uint8_t> rp, mp, rc, mc;
  for (const auto &[rank, v] : retail.percent)
    rp[rank] = v;
  for (const auto &[rank, v] : mine.percent)
    mp[rank] = v;
  for (const auto &[rank, v] : retail.citizens)
    rc[rank] = v;
  for (const auto &[rank, v] : mine.citizens)
    mc[rank] = v;
  CHECK(rp == mp, "the progress map holds the same 26 ranks with the same percents (the nodes at level >= 1; a Town shows 0)");
  std::set<uint32_t> rc_ranks, mc_ranks;
  for (const auto &[r, v] : rc)
    rc_ranks.insert(r);
  for (const auto &[r, v] : mc)
    mc_ranks.insert(r);
  CHECK(rc_ranks == mc_ranks && mc.size() == 14, "the citizens map holds the same 14 ranks (the nodes at level >= 3); our counts are all 0");
  bool zeros = true;
  for (const auto &[r, v] : mc)
    zeros = zeros && v == 0;
  CHECK(zeros, "(no citizens yet)");
  std::set<int64_t> re, me;
  for (const auto &[k, o] : retail.estates)
    re.insert(k);
  for (const auto &[k, o] : mine.estates)
    me.insert(k);
  CHECK(re == me, "the 244 estates are retail's (the six test estates of the 250 records are left out)");
  CHECK(world::NodeWire::is_test_estate("Test_Freehold") && world::NodeWire::is_test_estate("FreeholdEstate_Playground_Bucktown_Small_01") && !world::NodeWire::is_test_estate("Miraleth_S_101"), "(the estate rule)");
}

static void check_seeded_percent_and_ranks(const world::CityNodes &seeded, const repl::NodeStateMaps &sm) {
  const world::CityNodeInfo *joeva = seeded.by_guid(kJoeva);
  const world::CityNodeInfo *halcyon = seeded.by_guid(kHalcyon);
  CHECK(sm.percent.size() == 2 && sm.citizens.size() == 2 && joeva && halcyon, "... two nodes in the progress map and two in the citizens map");
  CHECK(joeva && halcyon && sm.percent[0].first == static_cast<uint32_t>(joeva->rank) && sm.percent[1].first == static_cast<uint32_t>(halcyon->rank) && sm.citizens[0].second == 0, "... keyed by RANK (Joeva 6, Halcyon 8), with no citizens");
  const bool pct_ok = world::NodeWire::percent_of(1, 0) == 0 && world::NodeWire::percent_of(1, 6592335) == 50 && world::NodeWire::percent_of(1, 10547736) == 100 && world::NodeWire::percent_of(4, 999999999) == 0 && world::NodeWire::percent_of(3, 73834150) == 100 && world::NodeWire::percent_of(3, 31643207) == 0;
  CHECK(pct_ok, "(percent_of is the service's Progress: 50 % halfway, 100 % at the next threshold, 0 at Town)");
}

static void check_seeded_maps() {
  std::string load_error;
  auto seeded_ptr = world::CityNodes::load(db_path(), load_error);
  if (!seeded_ptr) {
    std::printf("  [FAIL] the registry does not load: %s\n", load_error.c_str());
    ++g_fail;
    return;
  }
  world::CityNodes &seeded = *seeded_ptr;
  std::string seed_error;
  size_t applied = 0;
  CHECK(seeded.apply_seed_file(NODES_SEED_PATH, seed_error, &applied) && applied >= 2, "(the deploy seed loads)");
  const repl::NodeStateMaps sm = g_wire->maps(seeded);
  size_t at_joeva = 0, at_halcyon = 0;
  int64_t halcyon_lord = -1;
  for (const repl::NodeDataEntry &n : sm.nodes) {
    if (n.guid == kJoeva)
      at_joeva = n.level;
    if (n.guid == kHalcyon) {
      at_halcyon = n.level;
      halcyon_lord = n.parent;
    }
  }
  CHECK(halcyon_lord == kJoeva, "Halcyon is Joeva's vassal in the maps (ParentNodeGuid), as in retail's the retail capture");
  CHECK(sm.nodes.size() == 46 && at_joeva == 4 && at_halcyon == 3, "with the seed's levels the maps say Joeva 4 and Halcyon 3 (the rest Wilderness)");
  check_seeded_percent_and_ranks(seeded, sm);
}

static void test_maps() {
  std::printf("the GameState's node maps against the retail capture\n");
  const repl::NodeStateMaps retail = retail_login();
  CHECK(retail.nodes.size() == 46 && retail.percent.size() == 26 && retail.citizens.size() == 14 && retail.estates.size() == 244, "(the retail capture's login maps: 46 nodes, 26 progress, 14 citizens, 244 estates)");
  CHECK(g_wire->roster().size() == 46, "the registry's roster is retail's 46 nodes (the Verra_* records with a map)");
  set_retail_levels(*g_nodes, retail);
  const repl::NodeStateMaps mine = g_wire->maps(*g_nodes);
  CHECK(mine.nodes.size() == 46 && mine.estates.size() == 244, "the maps list 46 nodes and 244 estates");
  check_retail_node_entries(retail, mine);
  check_retail_small_maps_and_estates(retail, mine);
  check_seeded_maps();
}

static std::optional<repl::RpcCall> read_call(const schema::RpcSchema &rpc, const char *hex, size_t bits) {
  const std::vector<uint8_t> b = bytes_of(hex);
  std::unique_ptr<uint8_t[]> exact(new uint8_t[b.size() ? b.size() : 1]);
  std::copy(b.begin(), b.end(), exact.get());
  size_t pos = 0;
  auto p = repl::read_rpc_params(rpc, exact.get(), bits, pos, {});
  if (!p)
    return std::nullopt;
  repl::RpcCall c;
  c.rpc = &rpc;
  c.params = std::move(*p);
  c.decoded = true;
  c.param_bits = bits;
  return c;
}

static bool bits_equal(const emit::BunchWriter &w, const char *hex, size_t bits) {
  if (w.bit_pos() != bits)
    return false;
  const std::vector<uint8_t> b = bytes_of(hex);
  const std::vector<uint8_t> a = w.bytes();
  for (size_t i = 0; i < bits; ++i) {
    if (((a[i >> 3] >> (i & 7)) & 1) != ((b[i >> 3] >> (i & 7)) & 1))
      return false;
  }
  return true;
}

static void check_individual_summaries() {
  const auto joeva = g_wire->summary(kJoeva, *g_nodes, 0);
  const bool joeva_ok = joeva && joeva->level == 4 && joeva->plots.size() == 31 && joeva->max_node_essence == 70 && joeva->active_relic_slots == 3 && joeva->next_mayor_message_edit_time == 60;
  CHECK(joeva_ok, "a Town has 31 plots, 70 essence and 3 relic slots, and a message nobody can edit before 60");
  const auto lionhold = g_wire->summary(6064630651307950121ll, *g_nodes, 0);
  CHECK(lionhold && lionhold->level == 0 && lionhold->plots.size() == 1 && lionhold->max_node_essence == 0, "a node at Wilderness has its one plot and no essence");
  const auto none = g_wire->summary(6064634132054344390ll, *g_nodes, 0);
  const bool none_ok = none && none->culture == 9 && none->layout == 0 && none->plots.empty() && none->next_mayor_message_edit_time == INT64_MAX && none->time_until_next_maintenance == 0;
  CHECK(none_ok, "a dungeon record is retail's small default (culture 9, no layout, no plots)");
  CHECK(!g_wire->summary(12345, *g_nodes, 0) && !g_wire->siege(12345, *g_nodes), "a guid that is no node record has neither");
}

static void check_siege_summaries() {
  const schema::RpcSchema &srpc = pc_rpc("ClientReceiveSiegeSummary");
  size_t exact = 0, read = 0;
  for (const capture_nodes::Call &c : capture_nodes::kSieges) {
    const auto call = read_call(srpc, c.hex, c.bits);
    const auto retail = call ? repl::parse_siege_summary(*call) : std::nullopt;
    if (!retail)
      continue;
    ++read;
    const int64_t node = (static_cast<int64_t>(retail->target_org_guid[2]) << 32) | retail->target_org_guid[3];
    const auto mine = g_wire->siege(node, *g_nodes);
    if (!mine)
      continue;
    const auto params = repl::siege_summary_params(srpc, *mine);
    BunchWriter w;
    bool failed = false;
    if (params)
      repl::write_rpc_params(srpc, *params, w, {}, &failed);
    exact += params && !failed && bits_equal(w, c.hex, c.bits);
  }
  CHECKF(read == 46 && exact == 46, "the idle siege summary of the node (its display name and guid) is retail's exact bits for all 46 retail answers (%zu read, %zu exact)", read, exact);
}

static bool summary_writes_ok(const schema::RpcSchema &rpc, const repl::NodeSummary &mine) {
  const auto params = repl::node_summary_params(rpc, mine);
  BunchWriter w;
  bool failed = false;
  if (params)
    repl::write_rpc_params(rpc, *params, w, {}, &failed);
  return params && !failed && w.bit_pos() > 0;
}

static bool same_plot_records(const std::vector<repl::PlotEntry> &a, const std::vector<repl::PlotEntry> &b) {
  std::multiset<int64_t> rp, mp;
  for (const repl::PlotEntry &p : a)
    rp.insert(p.plot);
  for (const repl::PlotEntry &p : b)
    mp.insert(p.plot);
  return rp == mp;
}

static void test_summaries() {
  std::printf("the node summaries against the retail capture\n");
  set_retail_levels(*g_nodes, retail_login());
  const schema::RpcSchema &rpc = pc_rpc("ClientReceiveNodeSummary");
  size_t asked = 0, layout = 0, level = 0, plots = 0, plot_set = 0, essence = 0, relics = 0, culture = 0, kinds = 0, unknown = 0;
  size_t operational = 0, empty = 0, bytes_ok = 0;
  for (const capture_nodes::Call &c : capture_nodes::kSummaries) {
    const auto call = read_call(rpc, c.hex, c.bits);
    const auto retail = call ? repl::parse_node_summary(*call) : std::nullopt;
    if (!retail)
      continue;
    ++asked;
    const auto mine = g_wire->summary(c.node, *g_nodes, 1771000000);
    if (!mine) {
      ++unknown;
      continue;
    }
    layout += mine->layout == retail->layout;
    level += mine->level == retail->level;
    culture += retail->culture == mine->culture || (retail->culture != 0 && retail->culture != 9);
    plots += mine->plots.size() == retail->plots.size();
    plot_set += same_plot_records(retail->plots, mine->plots);
    essence += mine->max_node_essence == retail->max_node_essence;
    relics += mine->active_relic_slots == retail->active_relic_slots;
    for (const repl::PlotEntry &p : mine->plots) {
      operational += p.state == 2;
      empty += p.state == 0;
    }
    bytes_ok += summary_writes_ok(rpc, *mine);
    kinds += mine->node == c.node;
  }
  CHECKF(asked == 49 && unknown == 0, "all 49 nodes the client asks about have a summary (%zu asked, %zu unknown)", asked, unknown);
  CHECKF(layout == 49 && level == 49, "the layout and the level equal retail's for all 49 (%zu, %zu)", layout, level);
  CHECKF(plots == 49 && plot_set == 49, "the plots are the layout's plots at the node's level: the same count and the same plot records as retail's for all 49 (%zu, %zu)", plots, plot_set);
  CHECKF(essence == 49 && relics == 49, "the node essence and the relic slots (the level's modifications) equal retail's for all 49 (%zu, %zu)", essence, relics);
  CHECKF(culture == 49, "the culture is retail's wherever retail's is Kaelar, and 9 for a record that is no node (%zu)", culture);
  CHECKF(bytes_ok == 49 && kinds == 49, "each writes as a ClientReceiveNodeSummary (%zu)", bytes_ok);
  CHECK(operational > 0 && empty > 0, "the pre-built plots are Operational and the mayor's service plots are Empty");
  check_individual_summaries();
  check_siege_summaries();
}

static void log_unparsed_gs_blocks(const fake::Delivered &d, const schema::ClassSchema &gs_schema) {
  const auto blocks = repl::read_content_blocks(d.content.data(), d.content.bit_pos(), d.open ? new_actor_end(d.content, false) : 0);
  if (!blocks)
    return;
  for (const repl::BlockView &b : *blocks) {
    BunchWriter w;
    w.write_bit_range(d.content.data(), b.payload_pos, b.payload_bits);
    std::string why;
    const auto one = repl::load_object_state(gs_schema, w.data(), b.payload_bits, b.header.has_rep_layout, &why);
    std::printf("    (a block of %zu bits, rep layout %d, actor %d: %s)\n", static_cast<size_t>(b.payload_bits), b.header.has_rep_layout ? 1 : 0, b.header.is_actor ? 1 : 0, one ? "reads" : why.c_str());
  }
}

static void check_join_owner_info(Harness &h) {
  bool owner_ok = false;
  for (const fake::Delivered *d : chains_on(h, h.pawn_ch, 0)) {
    const States st = states_of(*d, h.s->bootstrap().pawn());
    const uint64_t owner = h.s->bootstrap().setup().guids.owner_info.ObjectId;
    const auto it = st.find(owner);
    if (it == st.end())
      continue;
    const repl::Value *v = it->second.state.get(*it->second.schema, "CharactersNodeData");
    const auto *list = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!list || list->elements.size() != 2)
      continue;
    const auto str = [&](size_t i, uint16_t handle) {
      const auto f = list->elements[i].values.find(handle);
      const std::string *s = f == list->elements[i].values.end() ? nullptr : f->second.get_if<std::string>();
      return s ? *s : std::string();
    };
    owner_ok = str(0, 1).empty() && str(0, 2) == "BBEC5C86EB101FE5F38C87F0FA22E7D0" && str(0, 3) == "cyb" && str(1, 2) == "0123456789ABCDEF0123456789ABCDEF" && str(1, 3) == "sister";
  }
  CHECK(owner_ok, "OwnerInfo.CharactersNodeData lists the account's characters: an empty CharacterId, the guid in upper case, the name");
  {
    Harness lone(cyb_record(/*with_sister=*/false));
    lone.settle();
    CHECK(lone.s->bootstrap().setup().account_characters.size() == 1 && lone.s->bootstrap().setup().account_characters[0].name == "cyb", "an account whose list is not known lists the joining character alone");
  }
  CHECK(h.s->bootstrap().setup().citizen_node == 0, "(no citizen node: OwnerInfo.CitizenNodeGuid and MayorNode stay 0, CaravanLaunchNode stays 0)");
}

static void test_join() {
  std::printf("a Tier3 join: the maps in the GameState's open, the account's characters in OwnerInfo\n");
  Harness h(cyb_record());
  h.settle();
  const std::vector<const fake::Delivered *> gs = chains_on(h, h.gs_ch, 0);
  CHECK(!gs.empty() && gs[0]->open, "the GameState's channel opens");
  repl::NodeStateMaps client_maps;
  const schema::ClassSchema &gs_schema = class_named("AoCGameStateBP_C");
  for (const fake::Delivered *d : gs) {
    const States st = states_of(*d, h.s->bootstrap().game_state());
    const auto it = st.find(0);
    if (it == st.end()) {
      log_unparsed_gs_blocks(*d, gs_schema);
      continue;
    }
    const repl::NodeStateMaps part = repl::read_node_maps(gs_schema, it->second.arrays);
    client_maps.nodes.insert(client_maps.nodes.end(), part.nodes.begin(), part.nodes.end());
    client_maps.percent.insert(client_maps.percent.end(), part.percent.begin(), part.percent.end());
    client_maps.citizens.insert(client_maps.citizens.end(), part.citizens.begin(), part.citizens.end());
    client_maps.estates.insert(client_maps.estates.end(), part.estates.begin(), part.estates.end());
  }
  const repl::NodeStateMaps want = g_wire->maps(*h.fx.nodes);
  if (!(client_maps == want)) {
    std::printf("    (chains on the GameState channel: %zu; the client has %zu nodes, %zu progress, %zu citizens, %zu estates; the server meant %zu, %zu, %zu, %zu)\n", gs.size(), client_maps.nodes.size(), client_maps.percent.size(), client_maps.citizens.size(), client_maps.estates.size(), want.nodes.size(), want.percent.size(), want.citizens.size(), want.estates.size());
  }
  CHECK(client_maps == want, "the client's GameState holds the maps of the registry's levels: 46 nodes, 2 progress entries, 2 citizens, 244 estates");
  CHECK(client_maps.nodes.size() == 46 && client_maps.estates.size() == 244 && client_maps.percent.size() == 2, "(46, 244 and 2)");
  CHECK(h.s->node_maps_version() == h.fx.nodes->version(), "(the session knows which version of the registry the join had)");
  check_join_owner_info(h);
}

static bool summary_matches_wire(const repl::NodeSummary &s, const world::CityNodes &nodes) {
  const auto mine = g_wire->summary(s.node, nodes, 0);
  return mine && s.level == mine->level && s.layout == mine->layout && s.plots.size() == mine->plots.size() && s.max_node_essence == mine->max_node_essence;
}

static std::vector<int64_t> asked_siege_nodes(const std::vector<int64_t> &asked) {
  std::vector<int64_t> out;
  for (const int64_t node : asked) {
    if (g_wire->is_real_node(node))
      out.push_back(node);
  }
  return out;
}

static bool siege_reply_matches_asked(const repl::RpcCall &c, const std::vector<int64_t> &asked_sieges, size_t &si) {
  const auto s = repl::parse_siege_summary(c);
  const int64_t node = s ? (static_cast<int64_t>(s->target_org_guid[2]) << 32) | s->target_org_guid[3] : 0;
  const bool ok = si < asked_sieges.size() && node == asked_sieges[si];
  ++si;
  return ok;
}

static void check_request_order_and_match(Harness &h, const std::vector<int64_t> &asked, const std::vector<repl::RpcCall> &kept) {
  bool in_order = true, match = true;
  size_t si = 0, ni = 0;
  const std::vector<int64_t> asked_sieges = asked_siege_nodes(asked);
  for (const repl::RpcCall &c : kept) {
    if (std::string(c.rpc->name) == "ClientReceiveSiegeSummary") {
      in_order = in_order && siege_reply_matches_asked(c, asked_sieges, si);
    } else if (std::string(c.rpc->name) == "ClientReceiveNodeSummary") {
      const auto s = repl::parse_node_summary(c);
      in_order = in_order && s && ni < asked.size() && s->node == asked[ni];
      match = match && s && summary_matches_wire(*s, *h.fx.nodes);
      ++ni;
    }
  }
  CHECK(in_order, "each answer is for the node asked, in the order asked");
  CHECK(match, "and is the summary of the registry's level (the seed's: Joeva a Town with 31 plots, Halcyon a Village with 23)");
}

static void check_repeat_and_cooldown_requests(Harness &h) {
  const size_t again_from = h.got.size();
  h.client_sends({h.client.actor(h.pc_ch, rpcs_bunch(class_named("AoCPlayerControllerBP_C"), {{"ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", kJoeva)}, {"ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", kJoeva)}, {"ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", kHalcyon)}}), true)});
  h.settle();
  const std::vector<std::string> again = pc_calls_from(h, again_from);
  CHECK(std::count(again.begin(), again.end(), std::string("ClientReceiveNodeSummary")) == 2, "the node window's repeated request is answered once per node (Joeva, Halcyon)");

  const size_t cd_from = h.got.size();
  h.client_sends({h.client.actor(h.pc_ch, rpcs_bunch(class_named("AoCPlayerControllerBP_C"), {{"ServerRequestAllNodeSiegeCooldowns", {}}, {"ServerSetEssentialCitizenshipWindowData", {}}}), true)});
  h.settle();
  std::vector<repl::RpcCall> cd;
  const std::vector<std::string> cd_names = pc_calls_from(h, cd_from, &cd);
  if (cd_names != std::vector<std::string>{"ClientReceiveAllCurrentNodeSiegeCooldowns"}) {
    std::printf("    (the PlayerController got:");
    for (const std::string &n : cd_names)
      std::printf(" %s", n.c_str());
    std::printf("; cooldown answers sent %zu, requests %zu)\n", h.s->siege_cooldowns_sent(), h.s->node_requests());
  }
  CHECK(cd_names == std::vector<std::string>{"ClientReceiveAllCurrentNodeSiegeCooldowns"}, "ServerRequestAllNodeSiegeCooldowns is answered (the empty array); the citizenship window is not");
  CHECK(cd.size() == 1 && cd[0].param_bits == 1, "the cooldown answer is one bit, as retail's");
  CHECK(h.s->citizenship_requests() == 1 && h.s->siege_cooldowns_sent() == 1, "(the citizenship request is counted and logged once)");
}

static void test_requests() {
  std::printf("the client's requests: 49 siege + 49 summary requests in one bunch, answered in retail's order\n");
  Harness h(cyb_record());
  h.play();
  const size_t from = h.got.size();
  std::vector<int64_t> asked;
  for (const capture_nodes::Call &c : capture_nodes::kSummaries)
    asked.push_back(c.node);
  std::vector<std::pair<const char *, repl::RpcParams>> calls;
  for (const int64_t node : asked) {
    calls.emplace_back("ServerRequestCurrentSiegeSummary", node_param("ServerRequestCurrentSiegeSummary", "NodeId", node));
    calls.emplace_back("ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", node));
  }
  calls.emplace_back("ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", asked[3]));
  calls.emplace_back("ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", 0));
  calls.emplace_back("ServerRequestNodeSummary", node_param("ServerRequestNodeSummary", "Node", 987654321));
  for (size_t i = 0; i < calls.size(); i += 30) {
    const std::vector<std::pair<const char *, repl::RpcParams>> part(calls.begin() + static_cast<std::ptrdiff_t>(i), calls.begin() + static_cast<std::ptrdiff_t>(std::min(calls.size(), i + 30)));
    h.client_sends({h.client.actor(h.pc_ch, rpcs_bunch(class_named("AoCPlayerControllerBP_C"), part), true)});
  }
  h.settle();
  std::vector<repl::RpcCall> kept;
  const std::vector<std::string> names = pc_calls_from(h, from, &kept);
  size_t sieges = 0, summaries = 0, other = 0, last_siege = 0, first_summary = names.size();
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] == "ClientReceiveSiegeSummary") {
      ++sieges;
      last_siege = i;
    } else if (names[i] == "ClientReceiveNodeSummary") {
      ++summaries;
      first_summary = std::min(first_summary, i);
    } else {
      ++other;
    }
  }
  CHECKF(sieges == 46 && summaries == 49, "46 siege summaries and 49 node summaries come back (%zu, %zu): retail answers no siege request of the two dungeons and the jungle, and the repeated and the unknown requests get none", sieges, summaries);
  CHECK(other == 0 && last_siege < first_summary, "every siege summary comes before every node summary, as retail answers (the retail capture: lines 505-516, then 540)");
  CHECK(h.s->node_requests() == 101 && h.s->siege_summaries_sent() == 46 && h.s->node_summaries_sent() == 49, "(the session counted 101 requests and 95 answers)");
  check_request_order_and_match(h, asked, kept);
  check_repeat_and_cooldown_requests(h);
}

int main() {
  spdlog::set_level(std::getenv("AOC_TEST_LOG") ? spdlog::level::debug : spdlog::level::off);
  std::string error;
  g_nodes = world::CityNodes::load(db_path(), error);
  if (!g_nodes) {
    std::printf("  [FAIL] the real content.db has no city nodes (%s): %s\n", db_path().c_str(), error.c_str());
    return 2;
  }
  g_wire = world::NodeWire::load(db_path(), *g_nodes, error);
  if (!g_wire) {
    std::printf("  [FAIL] the node wire does not load (%s): %s\n", db_path().c_str(), error.c_str());
    return 2;
  }
  test_maps();
  test_summaries();
  test_join();
  test_requests();
  run_node_state_part2();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
