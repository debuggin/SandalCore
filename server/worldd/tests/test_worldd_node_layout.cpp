// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost
// and CoA (Creation of Ashes). GNU AGPL v3 with additional terms under section
// 7: see LICENSE and NOTICE.
#include "test_worldd_node_layout_common.h"
#include "world/node_origins.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

using namespace aoc::worldd;

namespace {

size_t extras_expected(int64_t node, int level, int season) {
  (void)season;
  switch (level) {
  case 4:
    return node == 6064633652044300331ll ? 13
           : node == kJoeva              ? 12
           : node == kWinstead           ? 13
                                         : 0;
  case 3:
    return node == kHalcyon ? 6 : 6;
  default:
    return 0;
  }
}

std::string first_diff(const std::vector<repl::NodeLayoutItem> &a,
                       const std::vector<repl::NodeLayoutItem> &b) {
  char buf[400];
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
    if (a[i] == b[i])
      continue;
    const auto &x = a[i];
    const auto &y = b[i];
    std::snprintf(buf, sizeof buf,
                  "item %zu: ours owner %lld/%d/%d set %lld t(%g,%g,%g) vs "
                  "retail owner %lld/%d/%d set %lld t(%g,%g,%g)",
                  i, static_cast<long long>(x.owner_guid), x.owner_secondary,
                  x.owner_index, static_cast<long long>(x.asset_set),
                  x.translation.x, x.translation.y, x.translation.z,
                  static_cast<long long>(y.owner_guid), y.owner_secondary,
                  y.owner_index, static_cast<long long>(y.asset_set),
                  y.translation.x, y.translation.y, y.translation.z);
    return buf;
  }
  if (a.size() != b.size()) {
    std::snprintf(buf, sizeof buf, "%zu items against %zu", a.size(), b.size());
    return buf;
  }
  return "";
}

bool same_item_set(std::vector<repl::NodeLayoutItem> a,
                   std::vector<repl::NodeLayoutItem> b) {
  const auto key = [](const repl::NodeLayoutItem &x) {
    return std::make_tuple(x.owner_guid, x.owner_secondary, x.owner_index);
  };
  const auto less = [&](const repl::NodeLayoutItem &x,
                        const repl::NodeLayoutItem &y) {
    return key(x) < key(y);
  };
  std::sort(a.begin(), a.end(), less);
  std::sort(b.begin(), b.end(), less);
  return a == b;
}

struct ItemStats {
  size_t no_extras = 0;
  size_t bits_equal = 0;
  size_t cases = 0;
  size_t plot_order_differs = 0;
};

void replace_prebuilt_item(repl::NodeLayoutItem &target,
                           const std::vector<repl::NodeLayoutItem> &m_plots) {
  for (const auto &mine_item : m_plots) {
    if (mine_item.owner_guid == target.owner_guid &&
        mine_item.owner_secondary == target.owner_secondary &&
        mine_item.owner_index == target.owner_index) {
      target = mine_item;
      return;
    }
  }
}

void collect_prebuilt_states(const std::vector<world::PlotRow> &plots,
                             const repl::NodeLayoutPayload &retail,
                             std::set<int64_t> &prebuilt,
                             std::map<int64_t, uint8_t> &states) {
  for (const world::PlotRow &r : plots) {
    if (!r.pre_construct)
      continue;
    prebuilt.insert(r.plot);
    uint8_t state = 0;
    for (const auto &e : retail.items)
      if (e.item.owner_guid == r.plot)
        state = static_cast<uint8_t>(e.item.owner_secondary);
    states[r.plot] = state;
  }
}

size_t merge_mine_into_retail(int64_t node_guid,
                              const std::set<int64_t> &prebuilt,
                              const std::vector<repl::NodeLayoutItem> &mine,
                              repl::NodeLayoutPayload &merged) {
  size_t li = 0, extras = 0;
  std::vector<repl::NodeLayoutItem> m_layout, m_plots;
  for (const auto &it : mine)
    (it.owner_guid == node_guid ? m_layout : m_plots).push_back(it);
  for (auto &e : merged.items) {
    if (e.item.owner_guid == node_guid && li < m_layout.size())
      e.item = m_layout[li++];
    else if (prebuilt.count(e.item.owner_guid))
      replace_prebuilt_item(e.item, m_plots);
    else
      ++extras;
  }
  return extras;
}

void check_item_case_snapshot(const capture_node_layout::Case &c,
                              const repl::NodeLayoutPayload &retail,
                              const std::set<int64_t> &prebuilt,
                              const std::vector<repl::NodeLayoutItem> &mine,
                              ItemStats &st) {
  const std::string what =
      std::string(c.source) + " (node " + std::to_string(c.node) + ", level " +
      std::to_string(c.level) + ", season " + std::to_string(c.season) + ")";
  repl::NodeLayoutPayload merged = retail;
  const size_t extras =
      merge_mine_into_retail(c.node, prebuilt, mine, merged);
  const std::vector<uint8_t> raw = bytes_of_hex(c.hex);
  emit::BunchWriter w;
  const bool exact =
      repl::write_node_layout_snapshot(replicator(), merged, w) == c.bits &&
      same_bits(w, raw, c.bits);
  CHECKF(exact,
         "%s: written with retail's header and ids, our items and retail's "
         "other %zu are the exact bits of the capture",
         what.c_str(), extras);
  st.bits_equal += exact ? 1 : 0;
  st.no_extras += extras == 0 ? 1 : 0;
  if (extras == 0) {
    std::vector<repl::NodeLayoutItem> retail_items;
    for (const auto &e : retail.items)
      retail_items.push_back(e.item);
    CHECKF(same_item_set(mine, retail_items),
           "%s: and the whole item list is ours", what.c_str());
  }
}

void check_item_case(const capture_node_layout::Case &c, ItemStats &st) {
  ++st.cases;
  const std::string what =
      std::string(c.source) + " (node " + std::to_string(c.node) + ", level " +
      std::to_string(c.level) + ", season " + std::to_string(c.season) + ")";
  const std::vector<uint8_t> raw = bytes_of_hex(c.hex);
  const auto retail =
      repl::read_node_layout_payload(replicator(), raw.data(), c.bits);
  const world::CityNodeInfo *info = g_nodes->by_guid(c.node);
  CHECK(retail && info,
        (what + ": the payload reads and the node is one of the registry's")
            .c_str());
  if (!retail || !info)
    return;
  std::set<int64_t> prebuilt;
  std::map<int64_t, uint8_t> states;
  collect_prebuilt_states(g_wire->plot_rows(g_wire->layout_of(c.node), c.level),
                          *retail, prebuilt, states);
  const world::CityZoneInfo *zone = g_nodes->zone_by_guid(info->zoi_guid);
  world::LayoutRequest req{
      c.node,
      {c.level, c.culture, info->type, c.season, zone ? zone->biome : -1},
      world::PlotPolicy::PreBuiltOperational,
      &states};
  const std::vector<repl::NodeLayoutItem> mine = g_data->items(*g_wire, req);
  std::vector<repl::NodeLayoutItem> r_layout, r_plots, m_layout, m_plots;
  size_t extras = 0;
  for (const auto &e : retail->items) {
    if (e.item.owner_guid == c.node)
      r_layout.push_back(e.item);
    else if (prebuilt.count(e.item.owner_guid))
      r_plots.push_back(e.item);
    else
      ++extras;
  }
  for (const auto &it : mine)
    (it.owner_guid == c.node ? m_layout : m_plots).push_back(it);
  CHECKF(m_layout == r_layout,
         "%s: the %zu layout items are retail's (%zu built), same indices, "
         "asset sets and transforms to the last bit [%s]",
         what.c_str(), r_layout.size(), m_layout.size(),
         first_diff(m_layout, r_layout).c_str());
  const bool plots_set = same_item_set(m_plots, r_plots);
  CHECKF(
      plots_set,
      "%s: the %zu items of the pre-built plots are retail's (%zu built) [%s]",
      what.c_str(), r_plots.size(), m_plots.size(),
      first_diff(m_plots, r_plots).c_str());
  if (plots_set && m_plots != r_plots)
    ++st.plot_order_differs;
  CHECKF(extras == extras_expected(c.node, c.level, c.season),
         "%s: retail has %zu items beyond them (built service buildings and "
         "expansions; expected %zu)",
         what.c_str(), extras, extras_expected(c.node, c.level, c.season));
  check_item_case_snapshot(c, *retail, prebuilt, mine, st);
}

void test_items() {
  std::printf("the items against the 18 retail payloads\n");
  ItemStats st;
  for (const capture_node_layout::Case &c : capture_node_layout::kCases)
    check_item_case(c, st);
  CHECK(st.cases == 18 && st.bits_equal == 18,
        "all 18 cases reproduce to the exact bits");
  CHECKF(st.plot_order_differs <= 1,
         "the plots come in the layout's order in 17 of the 18 payloads (%zu "
         "differ: Aithanahr's)",
         st.plot_order_differs);
  CHECK(st.no_extras >= 9, "nine of the cases are whole payloads from the data "
                           "alone (the harbours, Hecribba and Azmaran)");
}

void test_defaults() {
  std::printf("the default start\n");
  const auto joeva = default_items(kJoeva, 4);
  CHECKF(joeva.size() == 73 && count_owner(joeva, kJoeva, true) == 55 &&
             count_owner(joeva, kJoeva, false) == 18,
         "Joeva at Town: %zu items = %zu layout + %zu plot (73 = 55 + 18)",
         joeva.size(), count_owner(joeva, kJoeva, true),
         count_owner(joeva, kJoeva, false));
  const auto halcyon = default_items(kHalcyon, 3);
  CHECKF(halcyon.size() == 61 && count_owner(halcyon, kHalcyon, true) == 45 &&
             count_owner(halcyon, kHalcyon, false) == 16,
         "Halcyon at Village: %zu items = %zu layout + %zu plot (61 = 45 + 16)",
         halcyon.size(), count_owner(halcyon, kHalcyon, true),
         count_owner(halcyon, kHalcyon, false));
  const auto winstead = default_items(kWinstead, 4);
  CHECKF(winstead.size() == 73, "Winstead at Town: %zu items (73)",
         winstead.size());
  bool all_operational = true;
  for (const auto &it : joeva)
    all_operational =
        all_operational && (it.owner_guid == kJoeva ? it.owner_secondary == 0
                                                    : it.owner_secondary == 2);
  CHECK(all_operational, "every plot item of the start is Operational (2), "
                         "every layout item has secondary id 0");
  repl::NodeLayoutHeader h;
  h.level = 4;
  h.season = 2;
  h.tags = {repl::kSiegeInactiveTag};
  h.plot_version = 1;
  repl::FastArrays arrays;
  CHECK(repl::sync_node_layout(replicator(), arrays, h, joeva),
        "Joeva's items sync");
  const auto st = repl::read_node_layout_state(replicator(), arrays);
  emit::BunchWriter w;
  CHECK(st && repl::write_node_layout_snapshot(replicator(), *st, w) == 58881,
        "Joeva's payload is 58,881 bits (the spike's number)");
  CHECK(default_items(kJoeva, 0).size() < joeva.size(),
        "a lower level draws less");
  CHECK(default_items(6064630651307950121ll, 4).empty(),
        "a record that is no real node (Lionhold's) has no items");
  CHECK(default_items(kJoeva, 4, world::kWinter).size() == joeva.size() &&
            default_items(kJoeva, 4, world::kSummer).size() == joeva.size(),
        "the season changes nothing at Joeva (Winter, Spring, Summer)");
  world::LayoutRequest req;
  req.node = kJoeva;
  req.context.level = 4;
  req.context.node_type = g_nodes->by_guid(kJoeva)->type;
  req.context.biome = 11;
  CHECK(g_data->items(*g_wire, req) != joeva,
        "Joeva in the wrong biome draws something else");
  req.context.biome =
      g_nodes->zone_by_guid(g_nodes->by_guid(kJoeva)->zoi_guid)->biome;
  req.plots = world::PlotPolicy::DataStartingState;
  const auto data_items = g_data->items(*g_wire, req);
  size_t under_construction = 0;
  for (const auto &it : data_items)
    under_construction += it.owner_secondary == 1 ? 1 : 0;
  CHECK(under_construction > 0 && data_items.size() == 73,
        "the data's own start has the NodeXP projects under construction (1)");
}

void check_scenery_loaded_state(
    const world::WorldActor &joeva, const repl::ObjectState &loaded,
    const std::optional<repl::NodeLayoutPayload> &pj) {
  const repl::Value *g2 = loaded.state.get(*loaded.schema, "NodeGuid");
  const repl::Value *r2 = loaded.state.get(*loaded.schema, "Role");
  const repl::Value *m2 =
      loaded.state.get(*loaded.schema, "ReplicatedMovement");
  CHECK(g2 && g2->get_if<int64_t>() && *g2->get_if<int64_t>() == kJoeva && r2 &&
            m2 &&
            *m2 == *joeva.actor.obj.state.get(*joeva.actor.obj.schema,
                                              "ReplicatedMovement"),
        "with the node's guid, the roles and the movement it was written with");
  const auto back = repl::read_node_layout_state(*loaded.schema, loaded.arrays);
  CHECK(back && pj && back->header == pj->header &&
            back->items.size() == pj->items.size() &&
            back->array_key == pj->array_key,
        "and the replicator's header and items");
  bool same_items = back.has_value() && pj.has_value();
  for (size_t i = 0; same_items && i < pj->items.size(); ++i)
    same_items = back->items[i].item == pj->items[i].item;
  CHECK(same_items, "every one equal");
  emit::BunchWriter snap;
  CHECK(back && repl::write_node_layout_snapshot(*loaded.schema, *back, snap) ==
                    58881,
        "the field payload is 58,881 bits");
}

void check_scenery_open_block(
    const world::WorldActor &joeva,
    const std::optional<repl::NodeLayoutPayload> &pj) {
  repl::ActorShadow shadow;
  std::vector<repl::ContentBlock> blocks;
  const bool collected = repl::collect_open_blocks(joeva.actor, shadow, blocks);
  CHECK(collected && blocks.size() == 1 && !blocks[0].sub &&
            blocks[0].has_rep_layout,
        "the open is one content block: the actor's, with a RepLayout block");
  if (!collected || blocks.size() != 1)
    return;
  const emit::BunchWriter &w = blocks[0].payload;
  const auto loaded = repl::load_object_state(
      *joeva.actor.obj.schema, w.bytes().data(), w.bit_pos(), true);
  CHECK(loaded.has_value(),
        "it loads back as an object state (the block reads to its last bit)");
  if (!loaded)
    return;
  check_scenery_loaded_state(joeva, *loaded, pj);
}

void check_scenery_unseeded_and_wide(Scene &s, const world::WorldActor &joeva) {
  const world::CityNodeInfo *any = nullptr;
  for (const auto &[node, e] : s.scenery->entries()) {
    bool captured = false;
    for (const auto &o : world::node_origins::kOrigins)
      captured = captured || o.node == node;
    if (!captured && e.actor) {
      any = g_nodes->by_guid(node);
      break;
    }
  }
  const world::WorldActor *other = any ? s.actor_of(any->guid) : nullptr;
  CHECK(any && other && other->x == any->x && other->y == any->y &&
            !s.scenery->entry_of(any->guid)->captured_origin,
        "a node the captures do not show stands at its record's location");
  std::vector<uint64_t> found;
  s.registry.query_wide(joeva.x + 144000, joeva.y, 1000, found);
  const bool near_ok =
      std::find(found.begin(), found.end(), joeva.id) != found.end();
  found.clear();
  s.registry.query_wide(joeva.x + 147000, joeva.y, 1000, found);
  const bool far_ok =
      std::find(found.begin(), found.end(), joeva.id) == found.end();
  CHECK(near_ok && far_ok, "query_wide finds it at 1.44 km and not at 1.47 km");
}

void check_scenery_origins(Scene &s, const world::WorldActor &joeva) {
  const world::CityNodeInfo *info = g_nodes->by_guid(kJoeva);
  CHECK(std::fabs(joeva.x - (-1092010.8)) < 0.01 &&
            std::fabs(joeva.y - 522023.2) < 0.01 &&
            std::fabs(joeva.z - 13644.1) < 0.01 &&
            s.scenery->entry_of(kJoeva)->captured_origin,
        "Joeva stands where retail opened it (the layout origin: -10,920.1 m, "
        "5,220.2 m)");
  CHECK(std::hypot(joeva.x - info->x, joeva.y - info->y) > 2000 &&
            std::hypot(joeva.x - info->x, joeva.y - info->y) < 2600,
        "that is 24 m from the node record's location");
  const double dyaw =
      std::fmod(std::fabs(joeva.yaw_degrees - info->yaw), 360.0);
  CHECK(dyaw < 0.1 || dyaw > 359.9, "facing the record's yaw");
  world::NodeScenery::Config rec_cfg;
  rec_cfg.captured_origins = false;
  Scene r(rec_cfg);
  r.scenery->start();
  const world::WorldActor *rj = r.actor_of(kJoeva);
  CHECK(rj && rj->x == info->x && rj->y == info->y && rj->z == info->z &&
            !r.scenery->entry_of(kJoeva)->captured_origin,
        "with captured_origins off every node stands at its record's location");
  check_scenery_unseeded_and_wide(s, joeva);
}

void check_scenery_payloads(const world::WorldActor &joeva,
                            const world::WorldActor &halcyon,
                            const std::optional<repl::NodeLayoutPayload> &pj) {
  bool ids = pj.has_value();
  for (size_t i = 0; ids && i < pj->items.size(); ++i)
    ids = pj->items[i].id == static_cast<int32_t>(i + 1);
  CHECK(ids && pj->array_key == 73,
        "IDs 1..73 and ArrayReplicationKey 73: what a fresh server sends");
  CHECK(joeva.scenery.node == kJoeva && joeva.scenery.level == 4 &&
            joeva.scenery.items == 73 && joeva.scenery.payload_bits == 58881 &&
            joeva.scenery.name == "Joeva",
        "the actor says what it shows (for the log)");
  const auto ph = payload_of(halcyon);
  CHECK(ph && ph->items.size() == 61 && ph->header.level == 3,
        "Halcyon: Village, 61 items");
}

void check_scenery_actor_fields(const world::WorldActor &joeva) {
  const repl::Value *guid =
      joeva.actor.obj.state.get(*joeva.actor.obj.schema, "NodeGuid");
  const repl::Value *role =
      joeva.actor.obj.state.get(*joeva.actor.obj.schema, "Role");
  const repl::Value *remote =
      joeva.actor.obj.state.get(*joeva.actor.obj.schema, "RemoteRole");
  CHECK(guid && guid->get_if<int64_t>() && *guid->get_if<int64_t>() == kJoeva &&
            role && remote &&
            joeva.actor.obj.state.get(*joeva.actor.obj.schema,
                                      "ReplicatedMovement"),
        "NodeGuid (the client waits for it to be non-zero), Role, RemoteRole "
        "and ReplicatedMovement are set");
  CHECK(
      joeva.actor.open.archetype.size() == 2 &&
          joeva.actor.open.archetype[0].name ==
              "Default__NodeLayoutReplicator" &&
          joeva.actor.open.archetype[0].guid.ObjectId ==
              1074022900247277291ull &&
          joeva.actor.open.archetype[0].checksum == 4242000812u &&
          joeva.actor.open.level.size() == 3 &&
          joeva.actor.open.spawn.location && joeva.actor.open.spawn.rotation &&
          !joeva.actor.open.spawn.velocity && !joeva.actor.open.spawn.scale,
      "the archetype is retail's Default__NodeLayoutReplicator, the level the "
      "persistent one; the open has a location and a yaw and nothing else");
}

static void check_scenery_joeva_and_halcyon(Scene &s, size_t n,
                                            const world::WorldActor &joeva,
                                            const world::WorldActor &halcyon) {
  CHECK(joeva.kind == world::ActorKind::NodeLayout && joeva.never_dormant &&
            joeva.awake_radius_cm == 145000.0 && s.registry.wide_count() == n,
        "a never-dormant wide actor that is awake within 1.45 km");
  CHECK(joeva.id == joeva.actor.guid.ObjectId && (joeva.id & 1) == 0,
        "its id is its NetGUID's (even)");
  const auto pj = payload_of(joeva);
  CHECK(pj && pj->items.size() == 73 && pj->header.level == 4 &&
            pj->header.culture == 0 && pj->header.season == world::kSpring &&
            pj->header.tags ==
                std::vector<std::string>{repl::kSiegeInactiveTag} &&
            pj->header.plot_version == 1,
        "Joeva: Town, Kaelar, Spring, the SiegeInactive tag, PlotChangeVersion "
        "1, 73 items");
  check_scenery_payloads(joeva, halcyon, pj);
  check_scenery_actor_fields(joeva);
  check_scenery_origins(s, joeva);
  check_scenery_open_block(joeva, pj);
}

void test_scenery() {
  std::printf("the scenery: one replicator per town\n");
  Scene s;
  const size_t n = s.scenery->start();
  CHECK(n >= 2 && n == s.scenery->actors() && s.registry.size() == n &&
            s.registry.count(world::ActorKind::NodeLayout) == n,
        "start() makes one replicator per node with items at its level, in the "
        "registry");
  CHECK(s.scenery->start() == 0, "start() once");
  const world::WorldActor *joeva = s.actor_of(kJoeva);
  const world::WorldActor *halcyon = s.actor_of(kHalcyon);
  CHECK(joeva && halcyon, "Joeva (Town) and Halcyon (Village) have one");
  if (!joeva || !halcyon)
    return;
  check_scenery_joeva_and_halcyon(s, n, *joeva, *halcyon);
  const uint32_t v0 = joeva->version;
  CHECK(s.scenery->tick() == 0 && joeva->version == v0,
        "a tick with no level change changes nothing");
}

void check_level_change_wrap_and_empty() {
  Scene w;
  w.scenery->start();
  for (int i = 0; i < 260; ++i) {
    w.set_level(kHalcyon, (i % 2) ? 3 : 2);
    w.scenery->tick();
  }
  const auto pv = payload_of(*w.actor_of(kHalcyon));
  CHECK(pv && pv->header.plot_version != 0,
        "PlotChangeVersion skips 0 when it wraps (260 level changes)");
  int empty_level = -1;
  for (int l = 0; l <= 6 && empty_level < 0; ++l) {
    if (default_items(kJoeva, l).empty())
      empty_level = l;
  }
  if (empty_level >= 0) {
    Scene e;
    e.scenery->start();
    const size_t before = e.registry.size();
    e.set_level(kJoeva, empty_level);
    e.scenery->tick();
    CHECK(e.actor_of(kJoeva) == nullptr && e.registry.size() == before - 1,
          "a level with nothing to draw removes the replicator");
    e.set_level(kJoeva, 4);
    e.scenery->tick();
    CHECK(e.actor_of(kJoeva) != nullptr &&
              payload_of(*e.actor_of(kJoeva))->items.size() == 73,
          "and a level with something makes a new one");
  } else {
    std::printf("  (every level 0 to 6 of Joeva draws something: no removal to "
                "test)\n");
  }
}

void check_level_change_return_and_multi(Scene &s) {
  s.set_level(kJoeva, 4);
  CHECK(s.scenery->tick() == 1, "Joeva back to Town");
  const auto p4 = payload_of(*s.actor_of(kJoeva));
  CHECK(p4 && p4->items.size() == 73 && p4->header.level == 4 &&
            p4->header.plot_version == 3,
        "73 items again, PlotChangeVersion 3");
  s.set_level(kHalcyon, 4);
  s.set_level(kWinstead, 2);
  CHECK(s.scenery->tick() == 2, "two nodes changed together are two rebuilds");
  const auto ph = payload_of(*s.actor_of(kHalcyon));
  const auto pw = payload_of(*s.actor_of(kWinstead));
  CHECK(ph && ph->header.level == 4 && ph->header.plot_version == 2 && pw &&
            pw->header.level == 2 &&
            pw->items.size() == default_items(kWinstead, 2).size(),
        "each has its own level and PlotChangeVersion");
  s.set_level(kHalcyon, 4);
  CHECK(s.scenery->tick() == 0, "the same level again changes nothing");
  check_level_change_wrap_and_empty();
}

void test_level_change() {
  std::printf("the scenery: a level change is a new full payload\n");
  Scene s;
  s.scenery->start();
  const world::WorldActor *joeva = s.actor_of(kJoeva);
  CHECK(joeva != nullptr, "Joeva has a replicator");
  if (!joeva)
    return;
  const uint64_t id = joeva->id;
  const uint32_t version = joeva->version, reliable = joeva->reliable_version;
  s.set_level(kJoeva, 3);
  CHECK(s.scenery->tick() == 1 && s.scenery->rebuilds() == 1,
        "Joeva to Village: the tick rebuilds one node");
  joeva = s.actor_of(kJoeva);
  CHECK(joeva && joeva->id == id, "it is the same actor");
  const auto p3 = payload_of(*joeva);
  const auto want3 = default_items(kJoeva, 3);
  bool same = p3 && p3->items.size() == want3.size();
  for (size_t i = 0; same && i < want3.size(); ++i)
    same = p3->items[i].item == want3[i];
  CHECK(
      same && p3->header.level == 3 && p3->header.plot_version == 2,
      "its payload is the Village list in full, level 3, PlotChangeVersion 2");
  CHECK(p3 && p3->items.front().id > 73 && p3->array_key > 73,
        "with new ReplicationIDs after the old ones (the client's delta "
        "deletes 1..73 and adds the new)");
  CHECK(joeva->version > version && joeva->reliable_version > reliable,
        "the actor changed reliably: every connection that holds it is sent "
        "the update");
  CHECK(joeva->scenery.level == 3 && joeva->scenery.items == want3.size() &&
            joeva->scenery.plot_version == 2,
        "and says what it shows now");
  CHECK(s.scenery->tick() == 0, "the next tick has nothing to do");
  check_level_change_return_and_multi(s);
}

} // namespace

int main() {
  if (const int rc = init_node_layout_globals(); rc != 0)
    return rc;
  test_items();
  test_defaults();
  test_scenery();
  test_level_change();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
