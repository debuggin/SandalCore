// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
#include "test_world_area_common.h"

#include "combat/combat_world.h"
#include "items/inventory.h"
#include "npc/motion_table.h"
#include "npc/retail_motion.h"
#include "world/interaction_state.h"

using namespace aoc::worldd;
namespace repl = aoc::protocol::repl;
namespace schema = aoc::protocol::schema;

static void check_gm_mount_stock(const content::MountData &md, const vendor::GmVendor &mounts_vendor, const content::MountRow *&flyer, const content::MountRow *&gryphon) {
  bool all = true;
  for (const content::MountRow &m : md.mounts) {
    all = all && mounts_vendor.items.count(m.item) == 1;
    if (m.item_name == "Mount_Flying_Dawnbreaker_1")
      flyer = &m;
    if (m.item_name == "Mount_Flying_PTR_ONLY")
      gryphon = &m;
  }
  CHECK(all, "the mounts def's stock holds all 101 mount items (51 ground, 46 flying, 4 aquatic, the GM Eagle Gryphon among them)");
}

static void check_gm_mount_refusals(const content::ContentDb &db, const vendor::GmVendors &gm, const vendor::GmVendor &mounts_vendor, const content::MountData &md, int64_t flyer_item) {
  const auto &defs = gm.vendors();
  const auto other = std::find_if(defs.begin(), defs.end(), [](const vendor::GmVendor &v) {
    return v.name == "PTR_ONLY_Vendor_Materials";
  });
  repl::VendorPurchaseCart c2{other->guid, {{flyer_item, 1, std::nullopt, std::nullopt}}};
  CHECK(other != defs.end() && !other->items.count(flyer_item) && !gm.plan_purchase(db, *other, c2, 70).ok, "the materials vendor does not sell a mount");
  repl::VendorPurchaseCart c3;
  c3.vendor = mounts_vendor.guid;
  for (size_t i = 0; i < 3; ++i)
    c3.items.push_back({md.mounts[i].item, 1, std::nullopt, std::nullopt});
  CHECK(!gm.plan_purchase(db, mounts_vendor, c3, 2).ok, "three mounts do not fit two free slots: the cart is refused whole");
}

static void check_gm_mount_vendor(const content::ContentDb &db, const vendor::GmVendors &gm) {
  const auto &defs = gm.vendors();
  std::string merror;
  const auto md = db.mount_data(merror);
  const auto it = std::find_if(defs.begin(), defs.end(), [](const vendor::GmVendor &v) {
    return v.name == vendor::kMountsVendorName;
  });
  CHECK(md && md->mounts.size() == 101 && it != defs.end() && gm.mount_error().empty() && gm.mounts_total() == 101, "the mount tables are read and the mounts def is among the 26");
  if (!md || it == defs.end())
    return;
  const content::MountRow *flyer = nullptr;
  const content::MountRow *gryphon = nullptr;
  check_gm_mount_stock(*md, *it, flyer, gryphon);
  CHECK(gm.mounts_added() == 81, "81 of them are added to the 20 the def lists");
  if (!flyer || !gryphon)
    return;
  repl::VendorPurchaseCart c1;
  c1.vendor = it->guid;
  c1.items.push_back({flyer->item, 1, std::nullopt, std::nullopt});
  c1.items.push_back({gryphon->item, 1, std::nullopt, std::nullopt});
  const auto p1 = gm.plan_purchase(db, *it, c1, 70);
  CHECK(p1.ok && p1.stacks.size() == 2 && p1.stacks[0].record == flyer->item && p1.stacks[0].quantity == 1 && p1.stacks[1].record == gryphon->item, "a cart of a Dawnbreaker and the GM Eagle Gryphon: two stacks of one");
  check_gm_mount_refusals(db, gm, *it, *md, flyer->item);
}

static void check_gm_purchase_refusals(const content::ContentDb &db, const vendor::GmVendors &gm, const vendor::GmVendor &gear, const vendor::GmVendor &materials, int32_t max_stack) {
  repl::VendorPurchaseCart cart{gear.guid, {{*gear.items.begin(), 1, std::nullopt, std::nullopt}}};
  repl::VendorPurchaseCart wrong = cart;
  wrong.vendor = materials.guid;
  CHECK(!gm.plan_purchase(db, gear, wrong, 70).ok, "a cart naming another vendor is refused");
  repl::VendorPurchaseCart unlisted = cart;
  unlisted.items[0].item_record = 1;
  CHECK(!gm.plan_purchase(db, gear, unlisted, 70).ok, "an item no listing has is refused");
  for (const int32_t q : {0, -1, vendor::kMaxQuantity + 1, INT32_MIN, INT32_MAX}) {
    repl::VendorPurchaseCart c = cart;
    c.items[0].quantity = q;
    CHECK(!gm.plan_purchase(db, gear, c, 70).ok, ("a quantity of " + std::to_string(q) + " is refused").c_str());
  }
  repl::VendorPurchaseCart empty;
  empty.vendor = gear.guid;
  CHECK(!gm.plan_purchase(db, gear, empty, 70).ok, "an empty cart is refused");
  repl::VendorPurchaseCart many = cart;
  many.items.assign(vendor::kMaxCartItems + 1, cart.items[0]);
  CHECK(!gm.plan_purchase(db, gear, many, 70).ok, "a cart of 71 items is refused");
  int64_t stackable = 0;
  for (const int64_t item : materials.items) {
    std::string e;
    if (const auto rec = db.item(item, e); rec && rec->max_stack_size == max_stack) {
      stackable = item;
      break;
    }
  }
  repl::VendorPurchaseCart huge{materials.guid, {{stackable, vendor::kMaxQuantity, std::nullopt, std::nullopt}}};
  const auto big = gm.plan_purchase(db, materials, huge, 70);
  const int64_t material_cells = static_cast<int64_t>(items::kResourceContainers) * items::kMaxResourceCells;
  CHECK(big.ok == (static_cast<int64_t>(vendor::kMaxQuantity + max_stack - 1) / max_stack <= material_cells), "the largest quantity costs stacks, never a loop over the quantity");
  CHECK(!gm.plan_purchase(db, gear, cart, 0).ok, "no free slot at all refuses even one item");
}

static void check_gm_purchase_plans(const content::ContentDb &db, const vendor::GmVendors &gm) {
  const auto &defs = gm.vendors();
  const vendor::GmVendor &gear = *std::find_if(defs.begin(), defs.end(), [](const vendor::GmVendor &v) {
    return v.name == "PTR_ONLY_Vendor_Gear_0-9";
  });
  const vendor::GmVendor &materials = *std::find_if(defs.begin(), defs.end(), [](const vendor::GmVendor &v) {
    return v.name == "PTR_ONLY_Vendor_Materials";
  });
  const int64_t gear_item = *gear.items.begin();
  repl::VendorPurchaseCart cart;
  cart.vendor = gear.guid;
  cart.items.push_back({gear_item, 1, std::nullopt, std::nullopt});
  auto plan = gm.plan_purchase(db, gear, cart, 70);
  CHECK(plan.ok && plan.stacks.size() == 1 && plan.stacks[0].record == gear_item && plan.stacks[0].quantity == 1, "one item of the gear def: one stack of one");
  int64_t stackable = 0;
  int32_t max_stack = 1;
  for (const int64_t item : materials.items) {
    std::string e;
    const auto rec = db.item(item, e);
    if (rec && rec->max_stack_size > 1) {
      stackable = item;
      max_stack = rec->max_stack_size;
      break;
    }
  }
  CHECK(stackable != 0, "the materials def lists an item that stacks");
  repl::VendorPurchaseCart mats;
  mats.vendor = materials.guid;
  mats.items.push_back({stackable, max_stack * 2 + 1, std::nullopt, std::nullopt});
  plan = gm.plan_purchase(db, materials, mats, 70);
  CHECK(plan.ok && plan.stacks.size() == 3 && plan.stacks[0].quantity == max_stack && plan.stacks[1].quantity == max_stack && plan.stacks[2].quantity == 1, "a quantity of two stacks and one is three stacks: MaxStackSize, MaxStackSize, 1");
  std::string material_error;
  const auto material_rec = db.item(stackable, material_error);
  CHECK(material_rec && material_rec->resource, "the item that stacks in the materials def is a material");
  plan = gm.plan_purchase(db, materials, mats, 2);
  CHECK(plan.ok && plan.stacks.size() == 3, "three stacks of materials are planned although the item bag has two free slots");
  const auto gear_rec = db.item(gear_item, material_error);
  repl::VendorPurchaseCart three = cart;
  three.items[0].quantity = (gear_rec && gear_rec->max_stack_size > 1 ? gear_rec->max_stack_size : 1) * 2 + 1;
  plan = gm.plan_purchase(db, gear, three, 2);
  CHECK(!plan.ok && plan.stacks.empty() && plan.refusal.find("free slot") != std::string::npos, "three stacks of items do not fit two free slots: the cart is refused whole");
  check_gm_purchase_refusals(db, gm, gear, materials, max_stack);
  size_t listed = 0;
  for (const vendor::GmVendor &v : defs)
    listed += v.listings;
  CHECK(listed > 5000, "the defs list thousands of listings");
}

static void check_gm_vendor_layout(size_t count) {
  const spawns::AreaConfig area;
  const auto layout = vendor::gm_layout(count, area, spawns::kLoginSpotYaw, spawns::kLoginSpotZ);
  double nearest = 1e18;
  for (size_t i = 0; i < layout.size(); ++i) {
    for (size_t j = i + 1; j < layout.size(); ++j)
      nearest = std::min(nearest, std::hypot(layout[i].x - layout[j].x, layout[i].y - layout[j].y));
  }
  CHECK(layout.size() == count && nearest > 150.0, "the layout puts every NPC at least 1.5 m from every other");
  CHECK(vendor::gm_layout(0, area, 0, 0).empty() && vendor::gm_layout(1, area, 0, 0).size() == 1 && vendor::gm_layout(13, area, 0, 0).size() == 13, "layouts of 0, 1 and 13");
}

static void test_gm_vendors(const content::ContentDb &db) {
  std::printf("the GM vendors\n");
  std::string error;
  auto gm = vendor::GmVendors::load(db, error);
  if (!gm) {
    CHECK(false, "the GM vendors load");
    return;
  }
  const auto &defs = gm->vendors();
  std::printf("    %zu PTR vendor defs, %zu item records in their stock; %zu GM/PTR item records: %zu in a stock, %zu in none\n", defs.size(), gm->stocked_records(), gm->gm_item_count(), gm->gm_items_stocked(), gm->unstocked().size());
  CHECK(defs.size() == 26, "26 PTR_ONLY vendor defs");
  const bool labels_ok = vendor::label_of("PTR_ONLY_Vendor_Gear_0-9") == "GM Gear 0-9" && vendor::kind_of("PTR_ONLY_Vendor_Gear_0-9") == "Gear" && vendor::kind_of("PTR_ONLY_Vendor_Recipes_Crafting_ANV") == "Recipes" && vendor::label_of("PTR_ONLY_Vendor_Materials") == "GM Materials";
  CHECK(labels_ok, "labels and kinds from the def's name");
  bool stocked = true, labelled = true;
  std::set<std::string> labels;
  for (const vendor::GmVendor &v : defs) {
    stocked = stocked && !v.items.empty() && v.listings >= v.items.size() / 2;
    labelled = labelled && labels.insert(v.label).second && gm->find(v.guid) == &v;
  }
  CHECK(stocked && labelled, "every def has a stock and a label of its own");
  CHECK(gm->gm_item_count() > 400 && gm->gm_items_stocked() + gm->unstocked().size() == gm->gm_item_count(), "the GM / PTR item records are counted: stocked and not");
  check_gm_mount_vendor(db, *gm);
  check_gm_vendor_layout(defs.size());
  check_gm_purchase_plans(db, *gm);
}

static void test_player_world(const content::ContentDb &db) {
  std::printf("PlayerWorld::load_area on the real content.db\n");
  aoc::protocol::ServerGuidAllocator guids(60, 0x6ee87ee4u);
  PlayerWorld world(&db, "", bootstrap::GameStateMode::Tier2, 60, guids);
  CHECK(world.ready() && world.registry().size() == 0 && world.area_report() == nullptr, "a world with content.db is ready and has no other actors until the area is loaded");
  std::string error;
  spawns::AreaConfig small;
  small.radius_cm = 40000;
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = world.load_area(small, error);
  CHECK(ok, "load_area loads a 400 m area");
  if (!ok) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  std::printf("    %s in %s\n", world.area_report()->describe().c_str(), ms_since(t0).c_str());
  CHECK(world.registry().size() == world.area_report()->total() && world.gm_vendors() && world.gm_vendors()->vendors().size() == 26, "the registry holds what the report counts; the GM catalog is the world's");
  CHECK(world.area().radius_cm == 40000.0 && !world.load_area(small, error), "the area is the world's, and loading it twice is refused");
  std::set<uint64_t> ids;
  bool unique = true;
  world.registry().for_each([&](const world::WorldActor &a) {
    unique = unique && ids.insert(a.id).second && a.actor.guid.ServerId == 60;
  });
  CHECK(unique, "every id is unique and under the world's ServerId");
  aoc::protocol::ServerGuidAllocator guids2(60, 0x6ee87ee4u);
  PlayerWorld bare(nullptr, "no content.db", bootstrap::GameStateMode::Tier2, 60, guids2);
  CHECK(!bare.load_area(small, error) && error.find("content.db") != std::string::npos && bare.registry().size() == 0, "a world with no content.db refuses to load an area");
}

static std::unique_ptr<PlayerWorld> make_ground_test_world(const content::ContentDb &db, const std::string &motion_path, aoc::protocol::ServerGuidAllocator &g) {
  auto w = std::make_unique<PlayerWorld>(&db, "", bootstrap::GameStateMode::Tier2, 60, g);
  std::string e, table_error;
  if (!w->load_blueprints(motion_path, table_error))
    std::printf("    (no Blueprint table: %s)\n", table_error.c_str());
  if (!w->load_area(spawns::AreaConfig{}, e)) {
    std::printf("    the area does not load: %s\n", e.c_str());
    std::exit(2);
  }
  return w;
}

static void record_standing_archetype(const std::string &leaf, const auto &c, const npc::RetailMotion &rm, std::vector<std::string> &never_walks, std::vector<std::string> &stands_by_data) {
  if (c.wander + c.patrol > 0)
    return;
  const std::string what =
      leaf + " (" + std::to_string(c.total) + " NPCs: " + std::to_string(c.stand_by_data) + " Stationary by the data, " + std::to_string(c.stand_no_life) + " with no life of their own; retail: " + std::to_string(rm.moving) + " of " + std::to_string(rm.epochs) + " epochs moved, its Stationary instances " + std::to_string(rm.still_moving) + " of " + std::to_string(rm.still) + ")";
  (c.stand_no_life > 0 ? never_walks : stands_by_data).push_back(what);
}

static void check_ground_type_mode_totals(const npc::GroundReport &rep, const std::vector<std::string> &stands_by_data, const std::vector<std::string> &never_walks) {
  for (const std::string &s : stands_by_data)
    std::printf("      stands, as retail's Stationary instances do: %s%s", s.c_str(), chr_n());
  for (const std::string &s : never_walks)
    std::printf("      STANDS: %s%s", s.c_str(), chr_n());
  CHECK(never_walks.empty(), "every archetype of the area that retail moves in most of its epochs walks here, but where the data's instances of it are Stationary");
  std::map<std::string, size_t> modes;
  for (const auto &[name, c] : rep.by_type) {
    modes["stand"] += c.stand;
    modes["wander"] += c.wander;
    modes["patrol"] += c.patrol;
  }
  CHECK(rep.fly == 0 && modes["stand"] == rep.stationary && modes["wander"] == rep.wander && modes["patrol"] == rep.patrol, "the per-type counts add up to the stand / wander / patrol / fly counts (no NPC flies: retail's creatures all walk, hawks and ravens included)");
  std::printf("    stand / wander / patrol / fly: %zu / %zu / %zu / %zu in %zu types%s", rep.stationary, rep.wander, rep.patrol, rep.fly, rep.by_type.size(), chr_n());
  for (const std::string &line : rep.describe_types(10))
    std::printf("      %s%s", line.c_str(), chr_n());
}

static void check_ground_answer_key(const npc::GroundReport &rep) {
  size_t keyed = 0, present = 0, still_epochs = 0, still_moved = 0;
  std::vector<std::string> never_walks, stands_by_data;
  for (size_t i = 0; i < npc::kRetailMotionCount; ++i) {
    const npc::RetailMotion &rm = npc::kRetailMotion[i];
    still_epochs += static_cast<size_t>(rm.still);
    still_moved += static_cast<size_t>(rm.still_moving);
    if (rm.epochs < 3 || rm.moving * 2 < rm.epochs)
      continue;
    ++keyed;
    const auto it = rep.by_type.find(npc::archetype_leaf(rm.archetype));
    if (it != rep.by_type.end()) {
      ++present;
      record_standing_archetype(it->first, it->second, rm, never_walks, stands_by_data);
    }
  }
  std::printf("    the answer key: %zu archetypes moved in most of their observed retail epochs, %zu of them are in the area; the Stationary instances' epochs: %zu, %zu moved%s", keyed, present, still_epochs, still_moved, chr_n());
  CHECK(keyed >= 20 && present >= 10, "the answer key names many archetypes of the area");
  check_ground_type_mode_totals(rep, stands_by_data, never_walks);
}

static void check_ground_replicated_movement(PlayerWorld &w) {
  size_t rm_bad = 0;
  w.registry().for_each([&](const world::WorldActor &a) {
    if (a.kind != world::ActorKind::Npc || !a.actor.obj.schema)
      return;
    const repl::Value *v = a.actor.obj.state.get(*a.actor.obj.schema, "ReplicatedMovement");
    const auto *m = v ? v->get_if<repl::RepMovement>() : nullptr;
    if (!m)
      return;
    const auto loc = repl::dequantize_location(a.actor.obj.opts, m->location);
    if (std::fabs(loc.x - a.x) > 0.02 || std::fabs(loc.y - a.y) > 0.02 || std::fabs(loc.z - a.z) > 0.02)
      ++rm_bad;
  });
  CHECK(rm_bad == 0, "every NPC's ReplicatedMovement is where the registry puts it (to 0.01 cm)");
}

static void check_ground_standing_and_rep(PlayerWorld &w, size_t npcs) {
  size_t on_terrain = 0, off_terrain = 0, gm_checked = 0, gm_exact = 0;
  double worst = 0;
  w.registry().for_each([&](const world::WorldActor &a) {
    if (a.kind != world::ActorKind::Npc || !w.npcs()->has(a.id))
      return;
    double tz = 0;
    const bool has = w.nav()->terrain_height(a.x, a.y, &tz);
    const double foot = w.npcs()->mover(a.id)->foot_offset();
    if (has && std::fabs(a.z - foot - tz) < 1.5)
      ++on_terrain;
    else {
      ++off_terrain;
      worst = std::max(worst, has ? std::fabs(a.z - foot - tz) : 0.0);
    }
    if (a.npc.is_gm_vendor) {
      ++gm_checked;
      double gz = 0;
      const bool exact = w.nav()->ground_height(a.x, a.y, a.z - foot, &gz, 100, 100) && std::fabs(a.z - foot - gz) < 1.5;
      gm_exact += exact ? 1 : 0;
      if (!exact)
        std::printf("    GM vendor %s at (%.0f, %.0f): origin %.1f, foot %.1f, terrain %s%.1f\n", a.npc.name.c_str(), a.x, a.y, a.z, foot, has ? "" : "none ", has ? tz : 0.0);
    }
  });
  std::printf("    %zu NPCs stand on the terrain exactly, %zu elsewhere (a floor, no ground; the most %.0f cm off the terrain)\n", on_terrain, off_terrain, worst);
  CHECK(on_terrain >= npcs * 0.80, "most NPCs stand on the terrain: origin = its height + the half height times the scale (to 1.5 cm)");
  CHECK(gm_checked == 26 && gm_exact == 26, "all 26 GM vendors stand on the ground (the terrain or a floor) at the login spot");
  check_ground_replicated_movement(w);
}

static bool same_respawn_recipe(const world::WorldActor *f, const world::WorldActor *corpse) {
  return f && corpse && f->npc.level == corpse->npc.level && f->npc.name == corpse->npc.name && f->npc.spawn_ref == corpse->npc.spawn_ref && f->npc.loot_tables == corpse->npc.loot_tables;
}

static void verify_respawned_actor(PlayerWorld &rw, uint64_t victim, uint64_t fresh, uint64_t old_cylinder, uint64_t old_interact) {
  const world::WorldActor *corpse = rw.registry().find(victim);
  CHECK(corpse != nullptr && fresh != 0 && rw.aftermath()->stats().respawns == 1, "the spawn point refilled while the corpse still stands");
  const world::WorldActor *f = fresh ? rw.registry().find(fresh) : nullptr;
  const bool own_interact = f && (old_interact == 0 ? f->npc.interact_component == 0 : f->npc.interact_component != old_interact);
  CHECK(f && f->npc.collision_cylinder == fresh + 4 && f->npc.collision_cylinder != old_cylinder && f->npc.controller == fresh + 2 && own_interact, "the respawn's cylinder, controller and interactable are its own block's, not the first generation's");
  CHECK(f && rw.registry().find_by_component(f->npc.collision_cylinder) == f && rw.combat()->hit_component_of(fresh) == f->npc.collision_cylinder, "...and the registry and the combat world name the new cylinder");
  const bool recipe_ok = same_respawn_recipe(f, corpse) && rw.npcs()->has(fresh);
  CHECK(recipe_ok, "the recipe's data came along, and the NPC world walks it");
}

static uint64_t find_respawn_victim(PlayerWorld &rw) {
  if (!rw.combat() || !rw.aftermath())
    return 0;
  uint64_t victim = 0;
  rw.registry().for_each([&](const world::WorldActor &a) {
    if (victim == 0 && a.kind == world::ActorKind::Npc && a.npc.spawn_ref != 0 && a.npc.respawn_rule != "NoRespawn" && !a.npc.is_gm_vendor && a.npc.collision_cylinder == a.id + 4 && rw.npcs()->has(a.id))
      victim = a.id;
  });
  return victim;
}

static void check_ground_respawn(const content::ContentDb &db, const std::string &nav_path, const std::string &motion_path) {
  aoc::protocol::ServerGuidAllocator guids_r(60, 0x6ee87ee4u);
  auto rw = make_ground_test_world(db, motion_path, guids_r);
  std::string e_r;
  CHECK(rw->load_ground(nav_path, motion_path, e_r) && rw->combat() != nullptr && rw->aftermath() != nullptr, "a world of its own for the respawn, with its ground, its fights and its aftermath");
  const uint64_t victim = find_respawn_victim(*rw);
  CHECK(victim != 0, "a data NPC with a spawn point, its cylinder at its id + 4");
  if (victim == 0)
    return;
  const uint32_t ref = rw->registry().find(victim)->npc.spawn_ref;
  const uint64_t old_cylinder = rw->registry().find(victim)->npc.collision_cylinder, old_interact = rw->registry().find(victim)->npc.interact_component;
  rw->registry().modify(victim, world::Change::State, [](world::WorldActor &a) {
    a.npc.respawn_seconds = 2.0;
    a.npc.respawn_variance = 0.0;
    a.npc.respawn_rule = "Refill";
  });
  combat::Resolved r;
  r.mods.push_back({1, content::kGuidHealth, -1.0e9});
  double now = 50.0;
  rw->tick_world(now);
  const combat::Delivery d = rw->combat()->apply_hit_to_npc(victim, 0, r, now);
  CHECK(d.applied && d.lethal && rw->aftermath()->respawns_pending() == 1, "a hit of a billion kills it, and its spawn point is due to refill in 2 s");
  for (int i = 0; i < 30; ++i)
    rw->tick_world(now += 0.1);
  uint64_t fresh = 0;
  rw->registry().for_each([&](const world::WorldActor &a) {
    if (a.id != victim && a.npc.spawn_ref == ref)
      fresh = a.id;
  });
  verify_respawned_actor(*rw, victim, fresh, old_cylinder, old_interact);
}

struct WanderPathCounts {
  size_t tried = 0, no_start = 0, no_target = 0, no_path = 0, partial = 0, too_long = 0, ok = 0, npcs_ok = 0, npcs_sampled = 0;
};

static bool try_single_wander_target(PlayerWorld &w, const nav::Vec3 &g, int k, WanderPathCounts &c) {
  const double ang = (k + 0.5) * 0.7853981633974483, r = 600.0 + 150.0 * k;
  nav::Vec3 np;
  ++c.tried;
  if (!w.nav()->nearest_point({g.x, g.y, g.z}, 200, 400, &np)) {
    ++c.no_start;
    return false;
  }
  if (!w.nav()->nearest_point({g.x + std::cos(ang) * r, g.y + std::sin(ang) * r, g.z}, 150, 300, &np)) {
    ++c.no_target;
    return false;
  }
  const auto pr = w.nav()->find_path(g, np, 200, 400);
  if (!pr.found) {
    ++c.no_path;
    return false;
  }
  if (!pr.complete) {
    ++c.partial;
    return false;
  }
  if (pr.length > r * 3.0 + 500.0) {
    ++c.too_long;
    return false;
  }
  ++c.ok;
  return true;
}

static void try_npc_wander_targets(PlayerWorld &w, const nav::Vec3 &g, WanderPathCounts &c) {
  ++c.npcs_sampled;
  bool any = false;
  for (int k = 0; k < 8; ++k) {
    if (try_single_wander_target(w, g, k, c))
      any = true;
  }
  c.npcs_ok += any ? 1 : 0;
}

static void check_ground_wander_paths(PlayerWorld &w) {
  WanderPathCounts c;
  size_t i = 0;
  w.registry().for_each([&](const world::WorldActor &a) {
    if (a.kind != world::ActorKind::Npc || !w.npcs()->has(a.id) || (++i % 7) != 0)
      return;
    try_npc_wander_targets(w, w.npcs()->mover(a.id)->ground(), c);
  });
  std::printf("    %zu NPCs sampled, %zu with a wander path: of %zu targets %zu are off the navmesh (%zu at the start), %zu have no path, %zu a partial one, %zu a long detour, %zu are good\n", c.npcs_sampled, c.npcs_ok, c.tried, c.no_target, c.no_start, c.no_path, c.partial, c.too_long, c.ok);
  CHECK(c.npcs_sampled > 100 && c.npcs_ok * 100 >= c.npcs_sampled * 90, "from where it stands 90 % of the NPCs can walk to a wander target");
}

static void check_walkers_on_ground(PlayerWorld &w, const std::map<uint64_t, std::array<double, 3>> &start) {
  size_t off_ground = 0, checked = 0;
  w.registry().for_each([&](const world::WorldActor &a) {
    if (a.kind != world::ActorKind::Npc || !w.npcs()->has(a.id))
      return;
    const auto &s = start.at(a.id);
    if (std::hypot(a.x - s[0], a.y - s[1]) < 100.0)
      return;
    ++checked;
    double gz = 0;
    const double foot = w.npcs()->mover(a.id)->foot_offset();
    if (!w.nav()->ground_height(a.x, a.y, a.z - foot, &gz, 100, 100) || std::fabs(a.z - foot - gz) > 1.5) {
      ++off_ground;
      if (off_ground <= 6) {
        double tz2 = 0;
        const bool has_t = w.nav()->terrain_height(a.x, a.y, &tz2);
        std::printf("    off the ground: %s (%.0f, %.0f) origin %.1f foot %.1f ground %.1f (misses %zu), started at (%.0f, %.0f, %.1f), terrain %s%.1f\n", a.npc.name.c_str(), a.x, a.y, a.z, foot, gz, w.npcs()->mover(a.id)->ground_misses(), s[0], s[1], s[2], has_t ? "" : "none ", has_t ? tz2 : 0.0);
      }
    }
  });
  CHECK(checked > 0 && off_ground * 20 <= checked, "(nearly) every NPC that walked stands on the ground where it stopped");
}

static void record_walked_lively(PlayerWorld &w, const std::vector<uint64_t> &lively, const std::map<uint64_t, std::array<double, 3>> &start, std::set<uint64_t> &walked) {
  for (const uint64_t id : lively) {
    const world::WorldActor *a = w.registry().find(id);
    const auto &s = start.at(id);
    if (a && std::hypot(a->x - s[0], a->y - s[1]) > 100.0)
      walked.insert(id);
  }
}

static void print_npc_life_stats(PlayerWorld &w, size_t steps, double total_us, double max_us) {
  std::printf("    %zu steps: %.0f us on average, %.0f us at most; %llu legs, %llu registry writes\n", steps, total_us / static_cast<double>(steps), max_us, static_cast<unsigned long long>(w.npcs()->stats().moves_started), static_cast<unsigned long long>(w.npcs()->stats().writes));
  const auto st = w.npcs()->stats();
  std::printf("    NPCs now: %zu standing, %zu idling, %zu wandering, %zu patrolling, %zu pausing on a patrol, %zu returning; %zu active now, %llu wander/patrol targets with no path\n", st.standing, st.idling, st.wandering, st.patrolling, st.patrol_pausing, st.returning, st.active_now, static_cast<unsigned long long>(st.no_path));
  std::printf("    wander: %llu asks, %llu no point, %llu no path, %llu deferred; patrol: %llu asks, %llu no path, %llu deferred\n", (unsigned long long)st.wander_asks, (unsigned long long)st.wander_no_point, (unsigned long long)st.wander_no_path, (unsigned long long)st.wander_deferred, (unsigned long long)st.patrol_asks, (unsigned long long)st.patrol_no_path,
              (unsigned long long)st.patrol_deferred);
}

static void check_ground_two_minute_life(PlayerWorld &w, const spawns::AreaConfig &area) {
  std::map<uint64_t, std::array<double, 3>> start;
  std::vector<uint64_t> lively;
  w.registry().for_each([&](const world::WorldActor &a) {
    start[a.id] = {a.x, a.y, a.z};
    if (a.kind == world::ActorKind::Npc && w.npcs()->has(a.id) && !w.npcs()->stands(a.id) && std::hypot(a.x - area.center_x, a.y - area.center_y) <= 15000.0)
      lively.push_back(a.id);
  });
  std::sort(lively.begin(), lively.end());
  std::set<uint64_t> walked;
  double t = 100.0, total_us = 0, max_us = 0;
  size_t steps = 0;
  for (int i = 0; i < 1800; ++i) {
    t += 1.0 / 15.0;
    if (i % 15 == 0)
      record_walked_lively(w, lively, start, walked);
    w.note_player(1, area.center_x, area.center_y);
    const auto s0 = std::chrono::steady_clock::now();
    w.tick_world(t);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - s0).count();
    total_us += us;
    max_us = std::max(max_us, us);
    ++steps;
  }
  size_t moved = 0, moved_far = 0, awake = 0;
  double farthest = 0;
  w.registry().for_each([&](const world::WorldActor &a) {
    if (a.kind != world::ActorKind::Npc)
      return;
    const auto &s = start[a.id];
    const double d0 = std::hypot(s[0] - area.center_x, s[1] - area.center_y);
    const double moved_by = std::hypot(a.x - s[0], a.y - s[1]);
    if (d0 <= 17000.0)
      ++awake;
    if (moved_by > 100.0) {
      ++moved;
      farthest = std::max(farthest, moved_by);
      if (d0 > 17000.0 + 2000.0 + 2500.0)
        ++moved_far;
    }
  });
  std::printf("    a player at the login spot for 120 s: %zu NPCs within 170 m, %zu of the area moved more than 1 m (up to %.0f cm), %zu of those began beyond the margin\n", awake, moved, farthest, moved_far);
  print_npc_life_stats(w, steps, total_us, max_us);
  std::printf("    of the %zu NPCs within 150 m that have a life of their own, %zu walked more than 1 m in the two minutes%s", lively.size(), walked.size(), chr_n());
  CHECK(lively.size() > 50 && walked.size() * 100 >= lively.size() * 90, "90 % of the NPCs near the player that wander or patrol walked within two minutes");
  CHECK(moved > 5, "NPCs near the player walked");
  CHECK(moved_far == 0, "NPCs far from the player did not");
  CHECK(total_us / static_cast<double>(steps) < 3000.0, "a step takes under 3 ms on average with the area's crowd");
  check_walkers_on_ground(w, start);
}

static void check_ground_determinism(const content::ContentDb &db, PlayerWorld &w, const std::string &nav_path, const std::string &motion_path) {
  const spawns::AreaConfig area;
  aoc::protocol::ServerGuidAllocator guids2(60, 0x6ee87ee4u);
  auto w2 = make_ground_test_world(db, motion_path, guids2);
  std::string e2;
  w2->load_ground(nav_path, motion_path, e2);
  double t2 = 100.0;
  for (int i = 0; i < 1800; ++i) {
    t2 += 1.0 / 15.0;
    w2->note_player(1, area.center_x, area.center_y);
    w2->tick_world(t2);
  }
  bool identical = true;
  w.registry().for_each([&](const world::WorldActor &a) {
    const world::WorldActor *b = w2->registry().find(a.id);
    identical = identical && b && b->x == a.x && b->y == a.y && b->z == a.z;
  });
  CHECK(identical, "a second world, built the same way and run the same, ends with every NPC where the first has it");
}

static void test_ground(const content::ContentDb &db) {
  std::printf("the ground (stage 2): the NPCs on the real area's navmesh\n");
  const char *env_nav = std::getenv("AOC_AREA_NAV");
  const std::string nav_path = env_nav && *env_nav ? env_nav : "dist/content/area_nav.bin";
  const char *env_motion = std::getenv("AOC_NPC_MOTION");
  const std::string motion_path = env_motion && *env_motion ? env_motion : "dist/content/npc_motion.tsv";
  std::string error;
  const spawns::AreaConfig area;
  aoc::protocol::ServerGuidAllocator guids(60, 0x6ee87ee4u);
  auto w = make_ground_test_world(db, motion_path, guids);
  std::map<uint64_t, std::array<double, 3>> before;
  w->registry().for_each([&](const world::WorldActor &a) {
    before[a.id] = {a.x, a.y, a.z};
  });
  CHECK(!w->load_ground("no/such/area_nav.bin", motion_path, error) && error.find("cannot open") != std::string::npos && w->npcs() == nullptr && w->nav() == nullptr, "a missing navmesh file: no ground, no NPC world, a message");
  bool same = true;
  w->registry().for_each([&](const world::WorldActor &a) {
    same = same && before[a.id] == std::array<double, 3>{a.x, a.y, a.z};
  });
  CHECK(same, "...and every actor is where it was");
  w->tick_world(10.0);
  w->note_player(1, area.center_x, area.center_y);
  w->forget_player(1);
  CHECK(true, "tick_world, note_player and forget_player without a ground do nothing (no crash)");
  if (FILE *f = std::fopen(nav_path.c_str(), "rb")) {
    std::fclose(f);
  } else {
    std::printf("  [skip] %s is not there (tools/mapc: mapc build): the rest of the ground tests need it\n", nav_path.c_str());
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(w->load_ground(nav_path, motion_path, error), "the real ground loads");
  if (!w->npcs()) {
    std::printf("    %s\n", error.c_str());
    return;
  }
  const npc::GroundReport rep = *w->ground_report();
  std::printf("    loaded and populated in %s: %s\n", ms_since(t0).c_str(), rep.describe().c_str());
  CHECK(!w->load_ground(nav_path, motion_path, error), "the ground is loaded once");
  const size_t npcs = w->registry().count(world::ActorKind::Npc);
  CHECK(rep.npcs == npcs && rep.on_ground + rep.no_ground == npcs, "every NPC of the registry is under the NPC world's care");
  CHECK(rep.on_ground >= npcs * 0.95, "at least 95 % of the NPCs found a ground (the rest are outside the extracted terrain or over a hole)");
  CHECK(rep.wander > 100 && rep.stationary > 100, "wanderers and standing NPCs both");
  check_ground_answer_key(rep);
  check_ground_standing_and_rep(*w, npcs);
  check_ground_respawn(db, nav_path, motion_path);
  check_ground_wander_paths(*w);
  check_ground_two_minute_life(*w, area);
  check_ground_determinism(db, *w, nav_path, motion_path);
}

static std::vector<bool> can_input_of(const world::WorldActor &a) {
  std::vector<bool> out;
  for (const repl::Subobject &s : a.actor.subobjects) {
    if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
      continue;
    const repl::Value *v = s.obj.state.get(*s.obj.schema, "CanInputActivate");
    const auto *arr = v ? v->get_if<repl::ArrayValue>() : nullptr;
    if (!arr)
      continue;
    for (const auto &el : arr->elements) {
      const auto it = el.values.find(1);
      out.push_back(it != el.values.end() && it->second.get_if<bool>() && *it->second.get_if<bool>());
    }
  }
  return out;
}

static std::vector<size_t> listed_players_of(const repl::Actor &a, const char *map) {
  std::vector<size_t> out;
  for (const repl::Subobject &s : a.subobjects) {
    if (!s.obj.schema || std::string(s.obj.schema->name) != "UInteractableComponent")
      continue;
    const repl::FastArrayState *arr = s.obj.arrays.find(s.obj.schema->field(map)->index);
    if (!arr)
      continue;
    for (const repl::FastArrayItem &it : arr->items()) {
      const auto v = it.values.values.find(2);
      const auto *w = v == it.values.values.end() ? nullptr : v->second.get_if<repl::GuidToBoolWrapper>();
      out.push_back(w ? w->pairs.size() : 0);
    }
  }
  return out;
}

static bool same_fast_arrays(const repl::Actor &first, const repl::Actor &second) {
  bool same_ids = true;
  for (size_t i = 0; i < first.subobjects.size(); ++i) {
    for (const auto &[field, st] : first.subobjects[i].obj.arrays.all()) {
      const repl::FastArrayState *o = second.subobjects[i].obj.arrays.find(field);
      same_ids = same_ids && o && o->items().size() == st.items().size();
      for (size_t k = 0; same_ids && k < st.items().size(); ++k) {
        same_ids = st.items()[k].id == o->items()[k].id && st.items()[k].values == o->items()[k].values;
      }
    }
  }
  return same_ids;
}

static void check_per_player_interaction_copy(const world::ActorRegistry &reg) {
  const world::WorldActor *vend = nullptr;
  reg.for_each([&](const world::WorldActor &a) {
    if (!vend && a.npc.is_gm_vendor)
      vend = &a;
  });
  if (!vend)
    return;
  const repl::GuidValue me{0xD8B2FE19u, 0x95FB4DF9u, 0x9B1E3958u, 0x6C2B2BD4u}, other{1, 2, 3, 4};
  const std::vector<int64_t> keys = world::interaction_inputs(vend->actor);
  CHECK(!keys.empty() && listed_players_of(vend->actor, "InteractVisibleStateMap").empty() && listed_players_of(vend->actor, "InteractGameplayStateMap").empty(), "a GM vendor has inputs with definitions and its registry maps are empty");
  repl::Actor mine = vend->actor;
  CHECK(world::set_player(mine, me, true), "the player is added to a copy");
  const auto v1 = listed_players_of(mine, "InteractVisibleStateMap"), g1 = listed_players_of(mine, "InteractGameplayStateMap");
  CHECK(v1.size() == keys.size() && g1 == v1 && v1.front() == 1, "...each input with a definition lists the player once in both maps");
  CHECK(listed_players_of(vend->actor, "InteractVisibleStateMap").empty() && listed_players_of(vend->actor, "InteractGameplayStateMap").empty(), "...and the registry's actor is as it was");
  repl::Actor again = mine;
  world::set_player(again, me, true);
  CHECK(listed_players_of(again, "InteractVisibleStateMap") == v1, "adding the same player twice lists it once");
  repl::Actor two = mine;
  world::set_player(two, other, true);
  CHECK(listed_players_of(two, "InteractVisibleStateMap").front() == 2, "a second player is listed beside the first");
  world::set_player(two, other, false);
  world::set_player(two, me, false);
  CHECK(listed_players_of(two, "InteractVisibleStateMap").front() == 0 && listed_players_of(two, "InteractGameplayStateMap").front() == 0, "taking both off leaves the lists empty");
  repl::Actor first = vend->actor, second = vend->actor;
  world::set_player(first, me, true);
  world::set_player(second, me, true);
  CHECK(same_fast_arrays(first, second), "the same player on the same actor always gives the same state and item ids");
  repl::Actor plain;
  bool found_plain = false;
  reg.for_each([&](const world::WorldActor &a) {
    if (!found_plain && a.kind == world::ActorKind::Gatherable && !world::is_interactable(a.actor)) {
      plain = a.actor;
      found_plain = true;
    }
  });
  if (found_plain)
    CHECK(!world::set_player(plain, me, true) && !world::allow_inputs(plain), "an actor with no interaction definition is not changed");
}

static void test_interaction_state(const content::ContentDb &db, const spawns::CapturedData &captured) {
  std::printf("the interaction gate: CanInputActivate and the per-player maps\n");
  std::string error;
  auto gm = vendor::GmVendors::load(db, error);
  if (!gm) {
    std::printf("  [FAIL] the GM vendors do not load: %s\n", error.c_str());
    g_fail++;
    return;
  }
  spawns::AreaSources sources;
  sources.db = &db;
  sources.captured = &captured;
  sources.gm = &*gm;
  spawns::AreaConfig area;
  area.radius_cm = 40000.0;
  world::ActorRegistry reg;
  const auto report = spawns::load_area(reg, sources, area, mint, 60, 0.0, error);
  CHECK(report && report->gm_vendors == 26 && report->refused == 0, "the area loads with its 26 GM vendors and nothing refused");
  if (!report)
    return;
  size_t vendors = 0, allowed = 0, defined = 0, others = 0, others_interactable = 0, others_allowed = 0;
  reg.for_each([&](const world::WorldActor &a) {
    if (a.npc.is_gm_vendor) {
      ++vendors;
      allowed += can_input_of(a) == std::vector<bool>({true, true, true, true}) ? 1 : 0;
      defined += world::is_interactable(a.actor) ? 1 : 0;
    } else if (a.kind == world::ActorKind::Npc) {
      ++others;
      if (world::is_interactable(a.actor))
        ++others_interactable;
      const auto c = can_input_of(a);
      others_allowed += std::find(c.begin(), c.end(), true) != c.end() ? 1 : 0;
    }
  });
  CHECK(vendors == 26 && allowed == 26 && defined == 26, "all 26 GM vendors have CanInputActivate true for the four inputs and an input with an interaction definition");
  CHECK(others_allowed == 0, "no other NPC is given CanInputActivate");
  std::printf("    %zu other NPCs, %zu with an interaction definition\n", others, others_interactable);
  check_per_player_interaction_copy(reg);
  spawns::GenericNpcs generic(captured, nullptr, spawns::GenericConfig{});
  content::NpcAssetRow quest_object;
  quest_object.guid = 9000001;
  quest_object.archetype_path = "/Game/DesignAssets/Interactables/Quest/CSQ_INT_PropKeyCheck.CSQ_INT_PropKeyCheck_C";
  spawns::Verdict v_prop = spawns::Verdict::Generic, v_pawn = spawns::Verdict::Generic;
  const spawns::OpenTemplate *as_prop = generic.prop_template(quest_object, &v_prop);
  const spawns::OpenTemplate *as_pawn = generic.template_for(quest_object, &v_pawn);
  CHECK(as_prop && as_prop->category == "prop", "a quest object's Blueprint is made as a prop from a recorded quest object");
  CHECK(as_prop && (!as_pawn || as_pawn != as_prop), "the same Blueprint asked for as a pawn does not get the prop's open (the prop is kept under its own key)");
}

void run_area_vendors_ground(const content::ContentDb &db, const spawns::CapturedData &captured) {
  test_interaction_state(db, captured);
  test_gm_vendors(db);
  test_player_world(db);
  test_ground(db);
}
