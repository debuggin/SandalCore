// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_area.cpp  (plan E1, stage 1)
// ============================================================================
#include "test_worldd_area_common.h"

#include "npc/motion_table.h"
#include "spawns/mount_donors.h"

#include <algorithm>
#include <cmath>

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;
using aoc::protocol::emit::FIntrepidNetworkGUID;

static void test_captured(const spawns::CapturedData &captured) {
  std::printf("the captured spawns\n");
  std::printf("    %zu opens, %zu templates, %zu spawns\n", captured.opens().size(), captured.template_count(), captured.spawns().size());
  CHECK(captured.opens().size() >= captured.template_count() && captured.opens().size() > 50, "the header holds the opens of the templates and the spawns");
  CHECK(captured.template_count() >= 30, "templates for at least 30 archetypes");
  CHECK(!captured.spawns().empty(), "spawns the captures saw in the area");
  size_t blocks = 0;
  bool all_schema = true;
  for (const spawns::OpenTemplate &t : captured.opens()) {
    blocks += t.blocks.size();
    for (const spawns::ParsedBlock &b : t.blocks)
      all_schema = all_schema && b.schema != nullptr;
  }
  CHECK(all_schema && blocks > captured.opens().size(), "every block has a schema and was read into values");
  CHECK(captured.area_radius_cm() == 100000.0 && captured.area_center_x() == spawns::AreaConfig{}.center_x && captured.area_center_y() == spawns::AreaConfig{}.center_y, "the header was cut for the area's centre and 1 km radius (spawns/area.h)");
  const spawns::OpenTemplate *vendor = captured.template_for(spawns::kGmVendorArchetype);
  CHECK(vendor != nullptr && vendor->find("UInteractableComponent") > 0, "the vendor archetype has a template with an InteractableComponent");
}

static bool is_allowed_subobject_diff(const std::string &cls, const std::string &path) {
  static const std::set<std::string> allowed_info = {"CharacterGuid.A", "CharacterGuid.B", "CharacterGuid.C", "CharacterGuid.D", "Level", "ServerId"};
  if (cls == "UNPCInfo")
    return allowed_info.count(path) > 0;
  if (cls == "UAlignmentComponent")
    return path == "Alignments" || path == "InheritedAlignments";
  if (cls == "UNPCSecondaryInfo")
    return path == "PopInstanceId" || path == "PopAssetId" || path == "AppearanceComponent";
  return cls == "UNPCCombatInfo" && path == "bIsDead";
}

static void check_one_subobject_diff(const spawns::OpenTemplate &t, size_t idx, const repl::Subobject &s, size_t &bad, std::string &first_bad) {
  const std::string cls = s.obj.schema->name;
  for (const std::string &path : differing(t.blocks[idx + 1].state, s.obj)) {
    if (is_allowed_subobject_diff(cls, path))
      continue;
    ++bad;
    if (first_bad.empty())
      first_bad = t.archetype_path + ": " + cls + "." + path + " differs";
  }
}

static void check_subobject_diffs(const spawns::OpenTemplate &t, const repl::Actor &actor, size_t &bad, std::string &first_bad) {
  if (actor.subobjects.size() + 1 != t.blocks.size()) {
    ++bad;
    if (first_bad.empty())
      first_bad = t.archetype_path + ": a subobject is missing";
  }
  for (size_t i = 0; i + 1 < t.blocks.size() && i < actor.subobjects.size(); ++i)
    check_one_subobject_diff(t, i, actor.subobjects[i], bad, first_bad);
}

static void check_actor_diffs(const spawns::OpenTemplate &t, const repl::ObjectState &actor_obj, size_t &bad, std::string &first_bad) {
  static const std::set<std::string> allowed_actor = {"AuthServerIDReplicated", "ReplicatedMovement", "Owner", "Controller", "ReplicatedServerLastTransformUpdateTimeStamp", "Instigator", "CurrentMovementInput", "GatherableGuid.A", "GatherableGuid.B", "GatherableGuid.C", "GatherableGuid.D"};
  for (const std::string &path : differing(t.actor_block().state, actor_obj)) {
    const bool left_unset = path.rfind("AttachmentReplication.", 0) == 0 || path.rfind("ReplicatedBasedMovement.", 0) == 0 || path.rfind("RepRootMotion.", 0) == 0;
    if (allowed_actor.count(path) || left_unset)
      continue;
    ++bad;
    if (first_bad.empty())
      first_bad = t.archetype_path + ": the actor's " + path + " differs";
  }
}

static std::string check_single_instance(const spawns::OpenTemplate &t, size_t &checked, size_t &bad, size_t &unreadable, size_t &writes) {
  spawns::InstanceParams p;
  p.x = -777000;
  p.y = 616000;
  p.z = 5000;
  p.yaw_degrees = 90;
  p.server_id = 60;
  p.level = 7;
  p.pop_instance = 111;
  p.pop_asset = 222;
  p.keep_captured_position = t.category == "gatherable";
  std::string error, first_bad;
  auto w = spawns::instantiate(t, mint, p, error);
  if (!w) {
    ++bad;
    return t.archetype_path + ": " + error;
  }
  ++checked;
  check_actor_diffs(t, w->actor.obj, bad, first_bad);
  check_subobject_diffs(t, w->actor, bad, first_bad);
  repl::ActorShadow shadow;
  repl::ExportAcks acks;
  bool failed = false;
  const auto open = repl::open_actor(w->actor, shadow, acks, &failed);
  if (!open || failed) {
    ++bad;
    return first_bad.empty() ? (t.archetype_path + ": the open does not build") : first_bad;
  }
  ++writes;
  const auto views = repl::read_content_blocks(open->payload.data(), open->payload.bit_pos(), /*start=*/0);
  (void)views;
  if (open->payload.bit_pos() == 0)
    ++unreadable;
  return first_bad;
}

static void test_instances(const spawns::CapturedData &captured) {
  std::printf("an instance is the captured open but for what is an instance's own\n");
  size_t checked = 0, bad = 0, unreadable = 0, writes = 0;
  std::string first_bad;
  for (const spawns::OpenTemplate &t : captured.opens()) {
    if (t.category != "npc" && t.category != "gatherable")
      continue;
    const std::string err = check_single_instance(t, checked, bad, unreadable, writes);
    if (first_bad.empty() && !err.empty())
      first_bad = err;
  }
  if (!first_bad.empty())
    std::printf("    first problem: %s\n", first_bad.c_str());
  CHECK(checked > 30 && bad == 0, "every NPC and gatherable template instantiates and differs from its capture only in the instance's values");
  CHECK(writes == checked && unreadable == 0, "every instance's open builds (SerializeNewActor and a block per object)");
}

static void check_vendor_own_refs(const world::WorldActor &w) {
  std::set<uint64_t> own = {w.actor.guid.ObjectId, w.npc.controller};
  for (const repl::Subobject &s : w.actor.subobjects)
    own.insert(s.guid.ObjectId);
  size_t refs = 0, foreign = 0;
  std::string first;
  const auto scan = [&](const repl::ObjectState &o) {
    for (const auto &[h, v] : o.state.values()) {
      const auto *g = v.get_if<FIntrepidNetworkGUID>();
      if (!g || g->ObjectId == 0)
        continue;
      ++refs;
      if (!own.count(g->ObjectId)) {
        ++foreign;
        if (first.empty())
          first = std::string(o.schema->name) + " handle " + std::to_string(h) + " names object " + std::to_string(g->ObjectId);
      }
    }
  };
  scan(w.actor.obj);
  for (const repl::Subobject &s : w.actor.subobjects)
    scan(s.obj);
  if (!first.empty())
    std::printf("    first foreign reference: %s\n", first.c_str());
  CHECK(refs >= 3 && foreign == 0, "every object reference of a vendor names its own objects (the AppearanceComponent is the minted NPCAppearanceComponent, not the retail server's NetGUID)");
}

static const repl::Value *subobject_field(const repl::Subobject *sub, const char *field) {
  return sub ? sub->obj.state.get(*sub->obj.schema, field) : nullptr;
}

static bool check_single_npc_appearance(const spawns::OpenTemplate &t, size_t &looked) {
  spawns::InstanceParams q;
  q.x = -777000;
  q.y = 616000;
  q.z = 5000;
  q.server_id = 2;
  std::string error;
  auto x = spawns::instantiate(t, mint, q, error);
  if (!x)
    return false;
  const repl::Subobject *second = nullptr;
  const repl::Subobject *look = nullptr;
  for (const repl::Subobject &s : x->actor.subobjects) {
    if (std::string(s.obj.schema->name) == "UNPCSecondaryInfo")
      second = &s;
    else if (std::string(s.obj.schema->name) == "UNPCAppearanceComponent")
      look = &s;
  }
  const repl::Value *v = subobject_field(second, "AppearanceComponent");
  const auto *g = v ? v->get_if<FIntrepidNetworkGUID>() : nullptr;
  ++looked;
  return look && g && g->ObjectId == look->guid.ObjectId;
}

static void check_npc_appearance_refs(const spawns::CapturedData &captured) {
  size_t looked = 0, bad = 0;
  for (const spawns::OpenTemplate &t : captured.opens()) {
    if (t.category != "npc" || t.find("UNPCAppearanceComponent") < 0)
      continue;
    if (!check_single_npc_appearance(t, looked))
      ++bad;
  }
  CHECK(looked >= 10 && bad == 0, "every NPC with an appearance component points its InteractInfo.AppearanceComponent at it");
}

static void check_vendor_info_and_population(const world::WorldActor &w) {
  const repl::Subobject *info = nullptr;
  const repl::Subobject *second_info = nullptr;
  for (const repl::Subobject &s : w.actor.subobjects) {
    if (std::string(s.obj.schema->name) == "UNPCInfo")
      info = &s;
    if (std::string(s.obj.schema->name) == "UNPCSecondaryInfo")
      second_info = &s;
  }
  const repl::Value *name = subobject_field(info, "CharacterName");
  const repl::Value *owner_name = subobject_field(info, "CreatureOwnerName");
  CHECK(name && name->get_if<std::string>() && *name->get_if<std::string>() == "GM Gear 0-9" && w.npc.name == "GM Gear 0-9", "the vendor's CharacterName is the label it was given (the retail template's 'Siege Vendor' is replaced)");
  CHECK(!owner_name, "CreatureOwnerName is not set: the NPC is no one's pet");
  const repl::Value *pi = subobject_field(second_info, "PopInstanceId");
  const repl::Value *pa = subobject_field(second_info, "PopAssetId");
  const int64_t *pi_val = pi ? pi->get_if<int64_t>() : nullptr;
  const int64_t *pa_val = pa ? pa->get_if<int64_t>() : nullptr;
  CHECK(pi_val && *pi_val == 0 && pa_val && *pa_val == 0, "a GM vendor names no population records (the client would apply the retail instance's DisplayNameOverrides 'Siege Vendor' over its name)");
}

static void test_names_and_references(const spawns::CapturedData &captured) {
  std::printf("an instance's name, authority and object references\n");
  const spawns::OpenTemplate *vendor = captured.template_for(spawns::kGmVendorArchetype);
  CHECK(vendor != nullptr, "the vendor template exists");
  if (!vendor)
    return;
  spawns::InstanceParams p;
  p.x = -777000;
  p.y = 616000;
  p.z = 5000;
  p.server_id = 2;
  p.name = "GM Gear 0-9";
  p.clear_population = true;
  std::string error;
  auto w = spawns::instantiate(*vendor, mint, p, error);
  CHECK(w.has_value(), "the vendor instantiates");
  if (!w)
    return;
  check_vendor_info_and_population(*w);
  const repl::Value *auth = w->actor.obj.state.get(*w->actor.obj.schema, "AuthServerIDReplicated");
  const uint64_t *a = auth ? auth->get_if<uint64_t>() : nullptr;
  CHECK(a && *a == 0, "AuthServerIDReplicated is the class default 0, as the player's actors and the retail NPCs have it (not the mesh's server id)");
  check_vendor_own_refs(*w);
  check_npc_appearance_refs(captured);
}

static bool is_archetype_exported_by_path(const spawns::DataSpawn &sp, const repl::Emission &open) {
  const uint64_t archetype_id = sp.open->archetype[0].guid.ObjectId;
  bool exported = false;
  for (const auto &g : open.exported) {
    if (g.ObjectId == archetype_id)
      exported = true;
  }
  return exported && sp.open->archetype[1].name == sp.asset->archetype_path.substr(0, sp.asset->archetype_path.rfind('.'));
}

static bool build_single_generic_blueprint(const spawns::DataSpawn &sp, size_t &named, std::string &why) {
  spawns::InstanceParams p;
  p.x = sp.x;
  p.y = sp.y;
  p.z = sp.z;
  p.level = sp.level;
  p.server_id = 60;
  p.pop_instance = sp.instance ? sp.instance->guid : 0;
  p.pop_asset = sp.asset ? sp.asset->guid : 0;
  p.seed = sp.seed;
  auto w = spawns::instantiate(*sp.open, mint, p, why);
  if (!w) {
    why = sp.open->archetype_path + ": " + why;
    return false;
  }
  repl::ActorShadow shadow;
  repl::ExportAcks acks;
  bool failed = false;
  const auto open = repl::open_actor(w->actor, shadow, acks, &failed);
  if (!open || failed || open->payload.bit_pos() == 0) {
    why = sp.open->archetype_path + ": the open does not build";
    return false;
  }
  named += is_archetype_exported_by_path(sp, *open) ? 1 : 0;
  return true;
}

static void check_generic_blueprints_build(const std::vector<spawns::DataSpawn> &all) {
  size_t built = 0, bad = 0, named = 0;
  std::string first_bad;
  std::set<std::string> seen;
  for (const spawns::DataSpawn &sp : all) {
    if (!sp.generic || !seen.insert(sp.open->archetype_path).second)
      continue;
    std::string why;
    if (!build_single_generic_blueprint(sp, named, why)) {
      ++bad;
      if (first_bad.empty())
        first_bad = why;
      continue;
    }
    ++built;
  }
  if (!first_bad.empty())
    std::printf("    first problem: %s\n", first_bad.c_str());
  std::printf("    %zu generic Blueprints: %zu open builds, %zu export their archetype by the Blueprint's path\n", seen.size(), built, named);
  CHECK(seen.size() > 150 && bad == 0 && built == seen.size() && named == built, "the open of every generic Blueprint builds and exports the Blueprint's path (no NPC is refused)");
}

static void check_generic_kill_switches(const content::PopulationData &data, const spawns::CapturedData &captured, const npc::MotionTable &table, const std::vector<spawns::DataSpawn> &spawns_a, const spawns::PopulationStats &gs) {
  const spawns::AreaConfig area;
  spawns::GenericConfig cfg;
  cfg.exclude = {"Goblin_Gravehide", "hawk_c"};
  spawns::GenericNpcs without(captured, &table, cfg);
  spawns::PopulationStats xs;
  const auto fewer = spawns::select_data_spawns(data, spawns::TemplateSource{&captured, &without}, area, xs);
  size_t gravehide = 0, left = 0;
  for (const auto &[leaf, n] : gs.generic_by_archetype) {
    if (leaf == "Goblin_Gravehide_C")
      gravehide = n;
  }
  for (const spawns::DataSpawn &sp : fewer) {
    const std::string leaf = spawns::blueprint_leaf(sp.asset->archetype_path);
    left += (leaf == "Goblin_Gravehide" || leaf == "Hawk") ? 1 : 0;
  }
  CHECK(gravehide > 100 && xs.lost_by_verdict[spawns::Verdict::Excluded] > 0 && left == 0 && fewer.size() < gs.spawned, "excluding a Blueprint by name (case apart, with or without _C) drops exactly its NPCs and no others");
  spawns::GenericConfig off;
  off.enabled = false;
  spawns::GenericNpcs disabled(captured, &table, off);
  spawns::PopulationStats os;
  const auto only = spawns::select_data_spawns(data, spawns::TemplateSource{&captured, &disabled}, area, os);
  CHECK(only.size() == spawns_a.size() && os.generic_spawned == 0, "with the generic NPCs switched off the area is stage 1's");
  spawns::GenericNpcs no_table(captured, nullptr, spawns::GenericConfig{});
  spawns::PopulationStats ns;
  const auto bare = spawns::select_data_spawns(data, spawns::TemplateSource{&captured, &no_table}, area, ns);
  const auto creatures = [](const std::vector<spawns::DataSpawn> &list) {
    size_t n = 0;
    for (const spawns::DataSpawn &s : list)
      n += s.open && s.open->category == "station" ? 0 : 1;
    return n;
  };
  CHECK(creatures(bare) == creatures(spawns_a) && ns.lost_by_verdict[spawns::Verdict::NoTable] > 0, "without npc_motion.tsv no generic NPC is made and the loader says so");
}

static bool generic_archetype_path_matches(const spawns::DataSpawn &sp) {
  return sp.open && sp.open->archetype_path == sp.asset->archetype_path && sp.open->archetype.size() == 2;
}

static bool is_distinct_generic_archetype(const spawns::CapturedData &captured, const spawns::OpenTemplate &open, std::set<uint64_t> &guid_of_archetype, std::map<std::string, uint64_t> &guid_by_path) {
  const uint64_t id = open.archetype[0].guid.ObjectId;
  const auto [it, fresh] = guid_by_path.emplace(open.archetype_path, id);
  if (it->second != id || (fresh && !guid_of_archetype.insert(id).second))
    return false;
  return captured.template_for(open.archetype_path) == nullptr;
}

static void check_generic_archetype_exports(const spawns::CapturedData &captured, const std::vector<spawns::DataSpawn> &all) {
  bool paths_ok = true, checksum_ok = true, distinct = true;
  std::set<uint64_t> guid_of_archetype;
  std::map<std::string, uint64_t> guid_by_path;
  for (const spawns::DataSpawn &sp : all) {
    if (!sp.generic)
      continue;
    if (!generic_archetype_path_matches(sp)) {
      paths_ok = false;
      continue;
    }
    checksum_ok = checksum_ok && sp.open->archetype[0].has_checksum && sp.open->archetype[0].checksum == 0;
    distinct = distinct && is_distinct_generic_archetype(captured, *sp.open, guid_of_archetype, guid_by_path);
  }
  CHECK(paths_ok, "a generic NPC's archetype is its Blueprint's path from the data");
  CHECK(checksum_ok, "its export carries NetworkChecksum 0, which the client does not compare");
  CHECK(distinct, "each Blueprint has one NetGUID of its own, no two share one, and none is a captured archetype's");
}

static void check_generic_npcs(const content::PopulationData &data, const spawns::CapturedData &captured, const spawns::AreaConfig &area, const std::vector<spawns::DataSpawn> &spawns_a) {
  const char *env_motion = std::getenv("AOC_NPC_MOTION");
  const std::string motion_path = env_motion && *env_motion ? env_motion : "dist/content/npc_motion.tsv";
  npc::MotionTable table;
  std::string table_error;
  if (!table.load(motion_path, table_error)) {
    std::printf("  [skip] %s: the generic NPC tests need npc_motion.tsv (tools/mapc: mapc build)\n", table_error.c_str());
    return;
  }
  spawns::GenericNpcs generic(captured, &table, spawns::GenericConfig{});
  CHECK(!generic.donor().empty(), "a plain creature's open the captures hold is the donor of the generic NPCs (the Wolf's)");
  spawns::PopulationStats gs;
  const auto all = spawns::select_data_spawns(data, spawns::TemplateSource{&captured, &generic}, area, gs);
  CHECK(gs.points_in_area + gs.area_points == gs.spawned + gs.blacklisted + gs.no_asset + gs.no_template, "with the generic NPCs every point is still counted once");
  CHECK(gs.spawned > 2 * spawns_a.size() && gs.generic_spawned > 0 && gs.spawned == all.size(), "the generic NPCs more than double the NPCs of the area");
  size_t lost_props = 0;
  for (const auto &[leaf, n] : gs.lost_props)
    lost_props += n;
  CHECK(gs.no_template == lost_props + gs.lost_by_verdict[spawns::Verdict::NoDonor] + gs.lost_by_verdict[spawns::Verdict::NoTable] + gs.lost_by_verdict[spawns::Verdict::NoPath] + gs.lost_by_verdict[spawns::Verdict::Excluded] + gs.lost_by_verdict[spawns::Verdict::Disabled],
        "every point with no NPC lost it for a reason the loader names: a prop, no donor, no table, no path, excluded or switched off");
  CHECK(gs.lost_by_verdict[spawns::Verdict::NoDonor] == 0 && gs.lost_by_verdict[spawns::Verdict::NoTable] == 0 && gs.lost_by_verdict[spawns::Verdict::NoPath] == 0, "no point is lost for want of a donor, a table or a Blueprint path");
  std::printf("    with the generic NPCs: %zu spawned (%zu generic, %zu of them from area circles), %zu lost (%zu props)\n", gs.spawned, gs.generic_spawned, gs.area_spawned, gs.no_template, lost_props);
  check_generic_archetype_exports(captured, all);
  check_generic_blueprints_build(all);
  check_generic_kill_switches(data, captured, table, spawns_a, gs);
}

static void print_population_archetypes(const spawns::PopulationStats &stats) {
  std::printf("    captured archetypes only: %zu points and %zu area points: %zu spawned, %zu with no captured archetype, %zu blacklisted, %zu without an asset\n", stats.points_in_area, stats.area_points, stats.spawned, stats.no_template, stats.blacklisted, stats.no_asset);
  for (const auto &[leaf, n] : stats.by_archetype)
    std::printf("      %4zu %s\n", n, leaf.c_str());
  std::vector<std::pair<size_t, std::string>> missing;
  for (const auto &[leaf, n] : stats.missing)
    missing.push_back({n, leaf});
  std::sort(missing.rbegin(), missing.rend());
  std::printf("    the archetypes no capture opened, most points first (%zu archetypes):\n", missing.size());
  for (size_t i = 0; i < missing.size() && i < 12; ++i)
    std::printf("      %4zu %s\n", missing[i].first, missing[i].second.c_str());
}

static void check_population_bounds_and_radius(const content::PopulationData &data, const spawns::CapturedData &captured, const spawns::AreaConfig &area, const std::vector<spawns::DataSpawn> &spawns_a, const spawns::PopulationStats &stats) {
  bool inside = true, levels = true, templates = true;
  for (const spawns::DataSpawn &s : spawns_a) {
    inside = inside && area.contains(s.x, s.y);
    levels = levels && s.level >= 0 && s.level <= 25 + 60;
    templates = templates && s.open && s.asset && s.open == captured.template_for(s.asset->archetype_path);
  }
  CHECK(inside, "every NPC is inside the area");
  CHECK(templates, "every NPC's template is the one of its asset's archetype");
  CHECK(levels, "levels are drawn from the instance's range");
  print_population_archetypes(stats);
  spawns::AreaConfig small;
  small.radius_cm = 25000;
  spawns::PopulationStats stats_small;
  const auto few = spawns::select_data_spawns(data, spawns::TemplateSource{&captured}, small, stats_small);
  CHECK(few.size() < spawns_a.size() && stats_small.points_in_area < stats.points_in_area, "a smaller radius selects fewer NPCs");
  check_generic_npcs(data, captured, area, spawns_a);
}

static void test_population(const content::ContentDb &db, const spawns::CapturedData &captured) {
  std::printf("the population: the data's NPCs\n");
  std::string error;
  const auto t0 = std::chrono::steady_clock::now();
  const auto data = db.population_data(error);
  CHECK(data.has_value(), "content.db's population chain reads");
  if (!data) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  std::printf("    read in %s: %zu assets, %zu instances, %zu sets, %zu placements, %zu areas\n", ms_since(t0).c_str(), data->assets.size(), data->instances.size(), data->sets.size(), data->placements.size(), data->areas.size());
  CHECK(data->assets.size() == 4576 && data->instances.size() == 16902 && data->sets.size() == 4728 && data->placements.size() == 15179 && data->areas.size() == 608, "the chain is the research's: 4,576 assets, 16,902 instances, 4,728 sets, 15,179 placements, 608 areas");
  const spawns::AreaConfig area;
  spawns::PopulationStats stats, stats_b;
  const auto t1 = std::chrono::steady_clock::now();
  const auto spawns_a = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured}, area, stats);
  std::printf("    selected in %s\n", ms_since(t1).c_str());
  const auto spawns_b = spawns::select_data_spawns(*data, spawns::TemplateSource{&captured}, area, stats_b);
  bool same = spawns_a.size() == spawns_b.size();
  for (size_t i = 0; same && i < spawns_a.size(); ++i) {
    same = spawns_a[i].x == spawns_b[i].x && spawns_a[i].y == spawns_b[i].y && spawns_a[i].level == spawns_b[i].level && spawns_a[i].open == spawns_b[i].open && spawns_a[i].seed == spawns_b[i].seed;
  }
  CHECK(same && !spawns_a.empty(), "the same inputs give the same NPCs, in the same order, with the same assets and levels");
  CHECK(stats.points_in_area == 3781 && stats.area_sets == 101, "3,781 placement points are inside a kilometre of the login spot and 101 area sets keep their NPCs there (the research's 3,929 counted 148 offsets of the area sets' members)");
  CHECK(stats.area_points == 817, "the circles of the 101 area sets hold 817 random points in the area");
  CHECK(stats.points_in_area + stats.area_points == stats.spawned + stats.blacklisted + stats.no_asset + stats.no_template, "every point is counted once: spawned or dropped for a reason");
  check_population_bounds_and_radius(*data, captured, area, spawns_a, stats);
}

static void check_component_lookup(const world::ActorRegistry &registry) {
  const world::WorldActor *any = nullptr;
  registry.for_each([&](const world::WorldActor &a) {
    if (!any && a.npc.is_gm_vendor)
      any = &a;
  });
  CHECK(any && registry.find_by_component(any->npc.interact_component) == any, "an InteractableComponent finds its actor");
  CHECK(registry.find_by_component(1) == nullptr && registry.find_by_component(0) == nullptr, "an unknown component finds nothing");
}

static void check_loaded_actors(const world::ActorRegistry &registry, const spawns::AreaConfig &area, const vendor::GmVendors &gm) {
  bool even = true, inside = true, controllers = true, interact = true;
  std::set<uint64_t> ids, components;
  size_t gm_count = 0;
  double gm_min = 1e18, gm_max = 0;
  registry.for_each([&](const world::WorldActor &a) {
    even = even && (a.id & 1) == 0 && a.id == a.actor.guid.ObjectId && ids.insert(a.id).second;
    if (a.npc.is_gm_vendor) {
      ++gm_count;
      const double d = std::hypot(a.x - area.center_x, a.y - area.center_y);
      gm_min = std::min(gm_min, d);
      gm_max = std::max(gm_max, d);
      interact = interact && a.npc.interact_component != 0 && components.insert(a.npc.interact_component).second && gm.find(a.npc.vendor_guid) != nullptr;
      return;
    }
    inside = inside && area.contains(a.x, a.y);
    if (a.kind == world::ActorKind::Npc)
      controllers = controllers && a.npc.controller != 0;
  });
  CHECK(even, "every id is even, unique and the actor's own ObjectId");
  CHECK(inside, "every actor but the GM vendors is inside the area");
  CHECK(controllers, "every NPC has an AI controller object");
  CHECK(gm_count == gm.vendors().size() && interact, "every GM vendor has its own InteractableComponent and a vendor def the catalog knows");
  CHECK(gm_min >= 700 && gm_max <= 1400, "the GM vendors stand 8 to 12.5 m from the login spot");
  check_component_lookup(registry);
}

static void check_load_area_radius_and_determinism(const spawns::AreaSources &sources, const spawns::AreaReport &report) {
  std::string error;
  world::ActorRegistry small_registry;
  spawns::AreaConfig small;
  small.radius_cm = 30000;
  const auto small_report = spawns::load_area(small_registry, sources, small, mint, 60, 0.0, error);
  CHECK(small_report && small_report->total() < report.total() && small_report->gm_vendors == report.gm_vendors, "a radius of 300 m loads fewer actors, the same GM vendors");
  spawns::AreaSources none = sources;
  none.gm = nullptr;
  world::ActorRegistry no_gm;
  const auto r3 = spawns::load_area(no_gm, none, small, mint, 60, 0.0, error);
  CHECK(r3 && r3->gm_vendors == 0, "without a GM catalog there are no GM vendors");
  world::ActorRegistry again;
  g_next = 900000;
  const auto r4 = spawns::load_area(again, sources, small, mint, 60, 0.0, error);
  world::ActorRegistry again2;
  g_next = 900000;
  const auto r5 = spawns::load_area(again2, sources, small, mint, 60, 0.0, error);
  bool same = r4 && r5 && again.size() == again2.size();
  if (same) {
    again.for_each([&](const world::WorldActor &a) {
      const world::WorldActor *b = again2.find(a.id);
      same = same && b && b->x == a.x && b->y == a.y && b->npc.level == a.npc.level && b->npc.name == a.npc.name;
    });
  }
  CHECK(same, "the same inputs and ids make the same world, level for level");
}

static void test_load_area(const content::ContentDb &db, const spawns::CapturedData &captured) {
  std::printf("load_area: the registry\n");
  std::string error;
  auto gm = vendor::GmVendors::load(db, error);
  CHECK(gm.has_value(), "the GM vendors load");
  if (!gm) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  spawns::AreaSources sources;
  sources.db = &db;
  sources.captured = &captured;
  sources.gm = &*gm;
  world::ActorRegistry registry;
  const spawns::AreaConfig area;
  const auto t0 = std::chrono::steady_clock::now();
  const auto report = spawns::load_area(registry, sources, area, mint, 60, 0.0, error);
  CHECK(report.has_value(), "the area loads");
  if (!report) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  std::printf("    loaded in %s\n    %s\n", ms_since(t0).c_str(), report->describe().c_str());
  for (const std::string &n : report->notes)
    std::printf("    note: %s\n", n.c_str());
  CHECK(report->refused == 0, "no spawn was refused");
  CHECK(registry.size() == report->total(), "the registry holds every actor the report counts");
  CHECK(report->data_npcs > 400 && report->captured_npcs > 0 && report->gatherables >= 20 && report->gm_vendors == gm->vendors().size(), "NPCs from the data and from the captures, resource nodes, and one GM vendor per PTR def");
  const size_t npcs = registry.count(world::ActorKind::Npc), nodes = registry.count(world::ActorKind::Gatherable);
  CHECK(npcs == report->data_npcs + report->captured_npcs + report->gm_vendors && nodes == report->gatherables, "the registry's kinds match the report");
  check_loaded_actors(registry, area, *gm);
  std::vector<uint64_t> near, within_350;
  registry.query(area.center_x, area.center_y, 15000, near);
  registry.query(area.center_x, area.center_y, 35000, within_350);
  std::printf("    %zu actors within 150 m of the login spot, %zu within 350 m\n", near.size(), within_350.size());
  CHECK(near.size() >= gm->vendors().size(), "the GM vendors are within the awake ring of the login spot");
  check_load_area_radius_and_determinism(sources, *report);
}

static bool donor_components_ok(const spawns::mount_donors::Open &o, const char *kind) {
  namespace md = spawns::mount_donors;
  const std::string want = std::string(kind) == "ground" ? "GroundMountComponent" : std::string(kind) == "aquatic" ? "AquaticMountComponent"
                                                                                                                   : "FlyingMountComponent";
  bool has_mount_component = false, ok = true;
  for (uint32_t i = 1; i < o.blocks; ++i) {
    const md::Block &b = md::kBlocks[o.first_block + i];
    if (std::string(b.cls) == "UMountComponent")
      has_mount_component = std::string(b.name) == want && b.checksum != 0 && b.kind == 1;
    ok = ok && b.kind == 1 && b.checksum != 0 && !std::string(b.name).empty();
  }
  return ok && has_mount_component;
}

static void check_donor_open(const spawns::mount_donors::Donor &d, bool &shape, bool &archetypes, bool &components) {
  namespace md = spawns::mount_donors;
  const md::Open &o = md::kOpens[d.open];
  shape = shape && o.blocks == 9 && std::string(o.actor_class) == "ANPCCharacter" && o.location.present && md::kBlocks[o.first_block].kind == 0 && std::string(md::kBlocks[o.first_block].cls) == "ANPCCharacter";
  const md::Chain &ac = md::kChains[o.archetype_chain];
  const md::Link &arch = md::kLinks[ac.first];
  archetypes = archetypes && std::string(o.archetype).rfind("/Game/GameBlueprints/Mounts/", 0) == 0 && std::string(arch.name).rfind("Default__Mount_", 0) == 0 && arch.checksum == 47039926u && ac.count == 2;
  components = components && donor_components_ok(o, d.kind);
}

static void test_mount_donors(const content::ContentDb &db) {
  namespace md = spawns::mount_donors;
  std::printf("the mount donors\n");
  std::set<std::string> kinds;
  bool shape = true, archetypes = true, components = true;
  for (const md::Donor &d : md::kDonors) {
    kinds.insert(d.kind);
    check_donor_open(d, shape, archetypes, components);
  }
  CHECK(md::kDonors[0].open == 0 && sizeof(md::kDonors) / sizeof(md::kDonors[0]) == 3 && kinds.size() == 3 && kinds.count("ground") && kinds.count("aquatic") && kinds.count("flying"), "three donors: a ground, an aquatic and a flying mount");
  CHECK(shape && archetypes && components, "each is an ANPCCharacter open with eight stably named subobjects (the MountComponent named for its kind) and its archetype exported");
  std::string error;
  const auto mounts = db.mount_data(error);
  std::set<std::string> from_data;
  if (mounts) {
    for (const content::MountRow &m : mounts->mounts)
      from_data.insert(m.blueprint_path);
  }
  bool all_in_data = mounts.has_value();
  for (const md::Donor &d : md::kDonors) {
    std::string path = md::kOpens[d.open].archetype;
    if (const size_t at = path.find(".Default__"); at != std::string::npos)
      path.replace(at, 10, ".");
    all_in_data = all_in_data && from_data.count(path) == 1;
  }
  CHECK(all_in_data, "the donors' archetypes are the Blueprints content.db's mount items name (SummonNotifyClassPath)");
}

int main() {
  const char *env = std::getenv("AOC_CONTENT_DB");
  const std::string path = env && *env ? env : AREA_CONTENT_DB_PATH;
  std::string error;
  auto db = content::ContentDb::open(path, error);
  if (!db) {
    std::printf("  [FAIL] content.db %s does not open: %s\n", path.c_str(), error.c_str());
    return 2;
  }
  std::printf("content.db %s (contentc version %s)\n", path.c_str(), db->meta().contentc_version.c_str());
  const auto t0 = std::chrono::steady_clock::now();
  auto captured = spawns::CapturedData::load(error);
  if (!captured) {
    std::printf("  [FAIL] the captured spawns do not load: %s\n", error.c_str());
    return 2;
  }
  std::printf("the captured spawns parsed in %s\n", ms_since(t0).c_str());
  test_captured(*captured);
  test_instances(*captured);
  test_names_and_references(*captured);
  test_population(*db, *captured);
  test_load_area(*db, *captured);
  test_mount_donors(*db);
  run_area_part2(*db, *captured);
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
